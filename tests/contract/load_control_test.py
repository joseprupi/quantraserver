"""Load control on the engine's Envoy, live.

Each scenario starts its own small cluster with the process manager
(tools/quantra-manager/quantra start --foreground: 2 workers + Envoy) on
ports of its own, sends gRPC calls straight to Envoy, and reads Envoy's admin
interface. A structured note on a local-vol surface at 1,024 paths keeps a
worker busy for a few seconds. Waits are polls on Envoy's own counters, not
fixed sleeps.

Covered:
- --max-requests 2: with 2 slow calls in flight a third is refused at once
  with RESOURCE_EXHAUSTED (gRPC) and 429 (JSON gateway); the Meta RPC is not
  refused and answers while both workers are busy; GET /meta reports the
  load in `capacity`, and `capacity` is null when the admin cannot be read.
- the default config: the third call is not refused (it waits behind a busy
  worker, today's behaviour), and Meta still answers at once.
- --retries 1: a slow call whose worker is killed is resent to the other
  worker and succeeds; with retries 0 it fails UNAVAILABLE.
- --stats-bind: the read-only listener serves /stats/prometheus and 404s the
  rest of the admin interface; Envoy keeps running.
- --envoy-config: a valid caller-supplied file is the one Envoy runs.
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

grpc = pytest.importorskip("grpc")
flatbuffers = pytest.importorskip("flatbuffers")
yaml = pytest.importorskip("yaml")

REPO_ROOT = Path(__file__).resolve().parents[2]
MANAGER = REPO_ROOT / "tools" / "quantra-manager" / "quantra"
GENERATOR = REPO_ROOT / "scripts" / "envoy_config.py"
SYNC_SERVER = REPO_ROOT / "build" / "server" / "sync_server"
JSON_SERVER = REPO_ROOT / "build" / "jsonserver" / "json_server"
FBS_DIR = REPO_ROOT / "flatbuffers" / "fbs"
NOTE_EXAMPLE = (REPO_ROOT / "examples" / "data" / "structured_note" /
                "synthetic_ndx_rty_5y_autocall_on_svi_flat_local_vol.json")
SLOW_PATHS = 1024

NOTE_RPC = "/quantra.QuantraServer/PriceStructuredNote"
META_RPC = "/quantra.QuantraServer/Meta"

pytestmark = pytest.mark.skipif(
    not (SYNC_SERVER.is_file() and JSON_SERVER.is_file() and shutil.which("envoy")),
    reason="needs the built engine, the gateway and envoy (run inside the test image)")


def _flatc():
    exe = shutil.which("flatc") or "/opt/quantra-deps/bin/flatc"
    if not Path(exe).exists():
        pytest.skip("flatc not found")
    return exe


@pytest.fixture(scope="module")
def slow_note(tmp_path_factory):
    """(flatbuffer bytes, JSON text) of a note that keeps a worker busy for a few seconds."""
    tmp = tmp_path_factory.mktemp("slow_note")
    request = json.loads(NOTE_EXAMPLE.read_text())
    for note in request["notes"]:
        note["monte_carlo"]["paths"] = SLOW_PATHS
    path = tmp / "slow_note.json"
    path.write_text(json.dumps(request))
    subprocess.run([_flatc(), "--binary", "-I", str(FBS_DIR), "-o", str(tmp),
                    str(FBS_DIR / "price_structured_note_request.fbs"), str(path)],
                   check=True, capture_output=True, text=True)
    return (tmp / "slow_note.bin").read_bytes(), path.read_text()


def _meta_request():
    b = flatbuffers.Builder(16)
    b.StartObject(0)
    b.Finish(b.EndObject())
    return bytes(b.Output())


def _http_get(url, timeout=2.0):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.status, resp.read().decode()


def _http(method, url, body=None, timeout=10.0):
    req = urllib.request.Request(url, data=body, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def _poll(predicate, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.02)
    pytest.fail(f"timed out after {timeout}s waiting for {what}", pytrace=False)


class Cluster:
    """A process-manager cluster (2 workers + Envoy) on its own ports."""

    def __init__(self, tmp_path, offset, args=(), env=None):
        self.port = 50310 + 10 * offset
        self.base_port = self.port + 4
        self.admin_port = 9980 + offset
        self.state = tmp_path / f"state{offset}"
        self.workers = 2
        cmd = [sys.executable, str(MANAGER), "start", "--workers", str(self.workers),
               "--port", str(self.port), "--base-port", str(self.base_port),
               "--admin-port", str(self.admin_port), "--foreground", *args]
        full_env = dict(os.environ, QUANTRA_STATE_DIR=str(self.state),
                        QUANTRA_HOME=str(REPO_ROOT), **(env or {}))
        for name in ("QUANTRA_MAX_REQUESTS", "QUANTRA_MAX_PENDING", "QUANTRA_RETRIES",
                     "QUANTRA_STATS_BIND", "QUANTRA_STATS_PORT", "QUANTRA_ENVOY_CONFIG"):
            if name not in (env or {}):
                full_env.pop(name, None)
        self.log = open(tmp_path / f"manager{offset}.log", "w")
        self.proc = subprocess.Popen(cmd, env=full_env, stdout=self.log,
                                     stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     start_new_session=True)
        try:
            _poll(self._healthy, 30, "both workers healthy in Envoy")
        except BaseException:
            self.stop()
            raise
        self.channel = grpc.insecure_channel(f"127.0.0.1:{self.port}")
        grpc.channel_ready_future(self.channel).result(timeout=10)

    def stats(self):
        try:
            _, body = _http_get(
                f"http://127.0.0.1:{self.admin_port}/stats?filter=cluster.quantra_workers.",
                timeout=1)
        except OSError:
            return {}
        out = {}
        for line in body.splitlines():
            name, _, value = line.partition(": ")
            try:
                out[name.replace("cluster.quantra_workers.", "")] = int(value)
            except ValueError:
                pass
        return out

    def _healthy(self):
        if self.proc.poll() is not None:
            pytest.fail(f"manager exited early ({self.proc.returncode}); see {self.log.name}",
                        pytrace=False)
        return (self.stats().get("membership_healthy") == self.workers
                and (self.state / "quantra.pid").is_file())

    def worker_pids(self):
        pids = json.loads((self.state / "quantra.pid").read_text())
        return [w["pid"] for w in pids["workers"]]

    def call(self, method, payload, timeout):
        """(status code name, seconds) of one unary call through Envoy."""
        stub = self.channel.unary_unary(method)
        start = time.monotonic()
        try:
            stub(payload, timeout=timeout)
            return "OK", time.monotonic() - start
        except grpc.RpcError as e:
            return e.code().name, time.monotonic() - start

    def call_in_background(self, method, payload, timeout):
        result = {}

        def run():
            result["status"], result["seconds"] = self.call(method, payload, timeout)

        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        return thread, result

    def stop(self):
        if getattr(self, "channel", None) is not None:
            self.channel.close()
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)  # the manager stops Envoy and the workers
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=5)
        # Workers and Envoy run in the manager's session; make sure none is left.
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except OSError:
            pass
        self.log.close()


def _cpu_ticks(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return int(fields[11]) + int(fields[12])
    except (OSError, IndexError, ValueError):
        return -1


def _busy_worker(pids):
    """The worker pid that is computing (gains CPU time over 200 ms), or None."""
    before = {pid: _cpu_ticks(pid) for pid in pids}
    time.sleep(0.2)
    gains = {pid: _cpu_ticks(pid) - before[pid] for pid in pids}
    pid, gain = max(gains.items(), key=lambda kv: kv[1])
    return pid if gain >= 10 else None  # >= 0.1 s of CPU in 0.2 s


def _start_two_slow_calls(cluster, slow):
    calls = [cluster.call_in_background(NOTE_RPC, slow, 60) for _ in range(2)]
    _poll(lambda: cluster.stats().get("upstream_rq_active") == 2, 10,
          "2 slow calls in flight")
    return calls


def _join(calls):
    for thread, _ in calls:
        thread.join(timeout=60)
    return [result for _, result in calls]


class Gateway:
    def __init__(self, tmp_path, http_port, grpc_port, admin_target):
        env = dict(os.environ, QUANTRA_ENVOY_ADMIN=admin_target)
        self.base = f"http://127.0.0.1:{http_port}"
        self.log = open(tmp_path / f"gateway{http_port}.log", "w")
        self.proc = subprocess.Popen([str(JSON_SERVER), f"127.0.0.1:{grpc_port}", str(http_port)],
                                     env=env, stdout=self.log, stderr=subprocess.STDOUT)

        def up():
            try:
                return _http_get(f"{self.base}/health", timeout=0.5)[0] == 200
            except OSError:
                return False

        _poll(up, 15, "the gateway")

    def meta(self):
        start = time.monotonic()
        status, body = _http_get(f"{self.base}/meta")
        assert status == 200
        return json.loads(body), time.monotonic() - start

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self.log.close()


_limited_offsets = iter(range(3, 9))


@pytest.fixture
def limited_cluster(tmp_path):
    # Env twin on purpose: the container entrypoint passes only env variables.
    # Each use gets its own ports: the previous cluster's admin port can still
    # hold closed connections when the next one starts.
    offset = next(_limited_offsets)
    cluster = Cluster(tmp_path, offset, env={"QUANTRA_MAX_REQUESTS": "2"})
    gateway = Gateway(tmp_path, 8090 + offset, cluster.port,
                      f"http://127.0.0.1:{cluster.admin_port}")
    blind_gateway = Gateway(tmp_path, 8070 + offset, cluster.port, "http://127.0.0.1:1")
    yield cluster, gateway, blind_gateway
    blind_gateway.stop()
    gateway.stop()
    cluster.stop()


def test_max_requests_refuses_at_once_and_meta_still_answers(limited_cluster, slow_note):
    cluster, gateway, blind_gateway = limited_cluster
    slow, slow_json = slow_note
    meta = _meta_request()

    # Idle: capacity reports the cluster and its limit.
    capacity, _ = gateway.meta()
    assert capacity["capacity"] == {"workers": 2, "healthy": 2, "in_flight": 0, "pending": 0,
                                    "max_requests": 2}

    calls = _start_two_slow_calls(cluster, slow)

    # gRPC: the third call is refused at once with RESOURCE_EXHAUSTED.
    status, seconds = cluster.call(NOTE_RPC, slow, 30)
    assert status == "RESOURCE_EXHAUSTED"
    assert seconds < 1.0

    # JSON gateway caller: HTTP 429 at once.
    start = time.monotonic()
    code, body = _http("POST", f"{gateway.base}/price-structured-note", slow_json.encode())
    assert code == 429, body
    assert json.loads(body)["code_name"] == "RESOURCE_EXHAUSTED"
    assert time.monotonic() - start < 1.0

    # Meta is routed past the pricing limit and answered off the pricing thread.
    status, seconds = cluster.call(META_RPC, meta, 10)
    assert status == "OK"
    assert seconds < 0.5

    # GET /meta reports the load; with the admin unreachable, capacity is null.
    data, seconds = gateway.meta()
    assert data["capacity"]["in_flight"] == 2
    assert data["capacity"]["workers"] == 2
    assert seconds < 0.5
    data, seconds = blind_gateway.meta()
    assert data["capacity"] is None
    assert seconds < 0.5

    results = _join(calls)
    assert [r["status"] for r in results] == ["OK", "OK"]
    assert cluster.stats().get("upstream_rq_pending_overflow", 0) >= 2


def _kill_the_busy_worker(cluster, slow):
    thread, result = cluster.call_in_background(NOTE_RPC, slow, 60)
    pid = _poll(lambda: _busy_worker(cluster.worker_pids()), 10, "a busy worker")
    os.kill(pid, signal.SIGKILL)
    thread.join(timeout=60)
    return result


def test_without_retries_a_killed_worker_fails_the_call(limited_cluster, slow_note):
    cluster, _, _ = limited_cluster
    result = _kill_the_busy_worker(cluster, slow_note[0])
    assert result["status"] == "UNAVAILABLE"


def test_retry_resends_a_call_whose_worker_died(tmp_path, slow_note):
    cluster = Cluster(tmp_path, 1, args=("--retries", "1", "--stats-bind", "127.0.0.1",
                                         "--stats-port", "9991"))
    try:
        result = _kill_the_busy_worker(cluster, slow_note[0])
        assert result["status"] == "OK"
        assert cluster.stats().get("upstream_rq_retry") == 1

        # The read-only stats listener: stats and ready, nothing else.
        base = "http://127.0.0.1:9991"
        status, body = _http("GET", f"{base}/stats/prometheus")
        assert status == 200 and "envoy_cluster_upstream_rq_active" in body
        assert _http("GET", f"{base}/ready")[0] == 200
        assert _http("GET", f"{base}/config_dump")[0] == 404
        assert _http("GET", f"{base}/clusters")[0] == 404
        assert _http("POST", f"{base}/quitquitquit", b"")[0] == 404
        assert _http("POST", f"{base}/stats/prometheus", b"")[0] == 404
        assert _http_get(f"http://127.0.0.1:{cluster.admin_port}/ready")[0] == 200
        assert cluster.proc.poll() is None
    finally:
        cluster.stop()


def test_custom_config_without_limit_keeps_todays_behaviour(tmp_path, slow_note):
    # A caller-supplied config: the default generated one with a 31 s route
    # timeout, so we can tell it is the file Envoy runs.
    custom = tmp_path / "custom_envoy.yaml"
    port, base_port, admin_port = 50330, 50334, 9982
    subprocess.run([sys.executable, str(GENERATOR), "--workers", "2", "--port", str(port),
                    "--base-port", str(base_port), "--admin-port", str(admin_port),
                    "--request-timeout", "31s", "-o", str(custom)],
                   check=True, capture_output=True, text=True,
                   env={k: v for k, v in os.environ.items() if not k.startswith("QUANTRA_")})
    cluster = Cluster(tmp_path, 2, args=("--envoy-config", str(custom)))
    try:
        _, dump = _http_get(f"http://127.0.0.1:{admin_port}/config_dump")
        assert '"timeout": "31s"' in dump
        calls = _start_two_slow_calls(cluster, slow_note[0])
        # No limit: the third call is not refused; it waits behind a busy
        # worker (cut short here by its own deadline).
        status, _ = cluster.call(NOTE_RPC, slow_note[0], 1.0)
        assert status == "DEADLINE_EXCEEDED"
        assert cluster.stats().get("upstream_rq_pending_overflow", 0) == 0
        # Meta answers while both workers are busy.
        status, seconds = cluster.call(META_RPC, _meta_request(), 10)
        assert status == "OK"
        assert seconds < 0.5
        _join(calls)
    finally:
        cluster.stop()
