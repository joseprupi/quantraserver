#!/usr/bin/env python3
"""
Envoy Configuration Generator for Quantra

Generates Envoy proxy configuration for load balancing across Quantra worker processes.

Usage:
    # As a module (used by quantra CLI)
    from envoy_config import generate_envoy_config, write_envoy_config
    
    config = generate_envoy_config(port=50051, base_port=50055, workers=4, admin_port=9901)
    write_envoy_config(config, "envoy.yaml")
    
    # As a standalone script
    python3 envoy_config.py --port 50051 --base-port 50055 --workers 4 --output envoy.yaml
"""

import argparse
import copy
import json
import os
import sys
from pathlib import Path
from typing import Dict, Any, Optional

try:
    import yaml
    HAS_YAML = True
except ImportError:
    HAS_YAML = False


DEFAULT_ROUTE_TIMEOUT_MS = 30000


def envoy_duration(ms: int) -> str:
    """Milliseconds as an Envoy duration string: 30000 -> "30s", 1500 -> "1.5s"."""
    if ms % 1000 == 0:
        return f"{ms // 1000}s"
    return f"{ms / 1000:.3f}".rstrip("0") + "s"


def route_timeout_from_env(environ=None) -> str:
    """Envoy route timeout from QUANTRA_ROUTE_TIMEOUT_MS (default 30000 ms).

    A non-positive or unparseable value logs one line to stderr and keeps the
    default. Kept in lockstep with the inline copy in tools/quantra-manager/quantra.
    """
    env = os.environ if environ is None else environ
    raw = str(env.get("QUANTRA_ROUTE_TIMEOUT_MS", "")).strip()
    ms = DEFAULT_ROUTE_TIMEOUT_MS
    if raw:
        try:
            value = int(raw)
        except ValueError:
            value = 0
        if value > 0:
            ms = value
        else:
            print(f"QUANTRA_ROUTE_TIMEOUT_MS={raw!r} is not a positive integer; "
                  f"using {DEFAULT_ROUTE_TIMEOUT_MS} ms", file=sys.stderr)
    return envoy_duration(ms)


DEFAULT_GRPC_TIMEOUT_HEADER_MAX_MS = 60000


def duration_ms(duration: str) -> float:
    """An Envoy duration string ("30s", "1.5s", "250ms") in milliseconds."""
    d = duration.strip()
    if d.endswith("ms"):
        return float(d[:-2])
    if d.endswith("s"):
        return float(d[:-1]) * 1000.0
    raise ValueError(f"not an Envoy duration in s or ms: {duration!r}")


def grpc_timeout_header_max_for(route_timeout: str) -> str:
    """Cap on a caller's grpc-timeout header: max(60s, route timeout).

    For gRPC requests Envoy uses the caller's grpc-timeout header (capped
    here) instead of the route timeout, so a route timeout above 60s only
    takes effect for gRPC traffic when the cap follows it. Kept in lockstep
    with the inline copy in tools/quantra-manager/quantra.
    """
    ms = max(DEFAULT_GRPC_TIMEOUT_HEADER_MAX_MS, duration_ms(route_timeout))
    return envoy_duration(int(round(ms)))


def least_request_choice_count(workers: int) -> int:
    """Number of workers LEAST_REQUEST samples per request (see the cluster).

    Kept in lockstep with the inline copy in tools/quantra-manager/quantra.
    """
    return max(2, 4 * workers)


# --- BEGIN load control (identical in scripts/envoy_config.py and tools/quantra-manager/quantra) ---
# Optional knobs on the generated Envoy config. With none of them set the
# config is exactly the one generated before they existed (a golden test
# holds that). Each knob is a keyword argument of generate_envoy_config and an
# environment variable; an explicit argument wins over the variable.
DEFAULT_MAX_REQUESTS = 10000
DEFAULT_MAX_PENDING = 10000
DEFAULT_RETRIES = 0
DEFAULT_STATS_PORT = 9902
WORKERS_CLUSTER = "quantra_workers"
META_CLUSTER = "quantra_meta"
ADMIN_CLUSTER = "envoy_admin"
META_RPC_PATH = "/quantra.QuantraServer/Meta"


def _int_knob(environ, name: str, default: int, minimum: int, maximum: Optional[int] = None) -> int:
    """An integer environment knob: unset or empty keeps the default; a value
    that is not an integer in [minimum, maximum] logs one line and keeps it."""
    raw = str(environ.get(name, "")).strip()
    if not raw:
        return default
    try:
        value = int(raw)
    except ValueError:
        value = None
    if value is None or value < minimum or (maximum is not None and value > maximum):
        bound = f">= {minimum}" if maximum is None else f"in [{minimum}, {maximum}]"
        print(f"{name}={raw!r} is not an integer {bound}; using {default}", file=sys.stderr)
        return default
    return value


def load_control_from_env(environ=None) -> Dict[str, Any]:
    """The load-control knobs read from QUANTRA_MAX_REQUESTS,
    QUANTRA_MAX_PENDING, QUANTRA_RETRIES, QUANTRA_STATS_BIND and
    QUANTRA_STATS_PORT."""
    env = os.environ if environ is None else environ
    return {
        "max_requests": _int_knob(env, "QUANTRA_MAX_REQUESTS", DEFAULT_MAX_REQUESTS, 1),
        "max_pending": _int_knob(env, "QUANTRA_MAX_PENDING", DEFAULT_MAX_PENDING, 1),
        "retries": _int_knob(env, "QUANTRA_RETRIES", DEFAULT_RETRIES, 0),
        "stats_bind": str(env.get("QUANTRA_STATS_BIND", "")).strip() or None,
        "stats_port": _int_knob(env, "QUANTRA_STATS_PORT", DEFAULT_STATS_PORT, 1, 65535),
    }


def resolve_load_control(max_requests=None, max_pending=None, retries=None,
                         stats_bind=None, stats_port=None, environ=None) -> Dict[str, Any]:
    """Explicit arguments over environment knobs over defaults."""
    knobs = load_control_from_env(environ)
    for key, value in (("max_requests", max_requests), ("max_pending", max_pending),
                       ("retries", retries), ("stats_bind", stats_bind),
                       ("stats_port", stats_port)):
        if value is not None:
            knobs[key] = value
    if knobs["max_requests"] < 1 or knobs["max_pending"] < 1:
        raise ValueError("max_requests and max_pending must be >= 1")
    if knobs["retries"] < 0:
        raise ValueError("retries must be >= 0")
    if not 1 <= knobs["stats_port"] <= 65535:
        raise ValueError("stats_port must be in [1, 65535]")
    return knobs


def _router_filter() -> Dict[str, Any]:
    return {
        "name": "envoy.filters.http.router",
        "typed_config": {
            "@type": "type.googleapis.com/envoy.extensions.filters.http.router.v3.Router"
        },
    }


def apply_load_control(config: Dict[str, Any], knobs: Dict[str, Any], admin_port: int) -> Dict[str, Any]:
    """Apply the load-control knobs to a generated config, in place.

    - max_requests (other than the default): the workers cluster refuses a call
      over the limit at once. Envoy's own answer to that refusal (response flag
      UO, "upstream overflow") is rewritten to grpc-status 8
      RESOURCE_EXHAUSTED and HTTP 429, so the JSON gateway answers 429. The
      remaining-capacity gauges are tracked. The Meta RPC is routed to a
      second cluster on the same workers with the default limits, so the
      pricing limit never refuses it.
    - max_pending: the plain max_pending_requests breaker. It covers calls
      waiting for a connection to a worker; it does not queue calls for a busy
      worker.
    - retries (> 0): a call whose worker dies or cannot be reached is resent
      to another worker. Envoy does not retry its own overflow refusal, and a
      busy worker returns no error, so neither is retried.
    - stats_bind: an extra listener on stats_bind:stats_port that serves only
      GET /stats/prometheus and GET /ready from the admin interface (which
      itself stays on 127.0.0.1); every other path answers 404.
    """
    resources = config["static_resources"]
    listener = resources["listeners"][0]
    hcm = listener["filter_chains"][0]["filters"][0]["typed_config"]
    routes = hcm["route_config"]["virtual_hosts"][0]["routes"]
    workers = resources["clusters"][0]
    thresholds = workers["circuit_breakers"]["thresholds"][0]

    thresholds["max_pending_requests"] = knobs["max_pending"]

    if knobs["max_requests"] != DEFAULT_MAX_REQUESTS:
        meta = copy.deepcopy(workers)
        meta["name"] = META_CLUSTER
        meta["load_assignment"]["cluster_name"] = META_CLUSTER
        meta["circuit_breakers"]["thresholds"][0]["max_requests"] = DEFAULT_MAX_REQUESTS
        thresholds["max_requests"] = knobs["max_requests"]
        thresholds["track_remaining"] = True
        hcm["local_reply_config"] = {
            "mappers": [{
                "filter": {"response_flag_filter": {"flags": ["UO"]}},
                "status_code": 429,
                "headers_to_add": [{
                    "header": {"key": "grpc-status", "value": "8"},
                    "append_action": "OVERWRITE_IF_EXISTS_OR_ADD",
                }],
            }]
        }
        meta_route = copy.deepcopy(routes[0])
        meta_route["match"] = {"path": META_RPC_PATH}
        meta_route["route"]["cluster"] = META_CLUSTER
        routes.insert(0, meta_route)
        resources["clusters"].append(meta)

    if knobs["retries"] > 0:
        for route in routes:
            route["route"]["retry_policy"] = {
                "retry_on": "reset,connect-failure",
                "num_retries": knobs["retries"],
                "retry_host_predicate": [{
                    "name": "envoy.retry_host_predicates.previous_hosts",
                    "typed_config": {
                        "@type": "type.googleapis.com/envoy.extensions.retry.host.previous_hosts.v3.PreviousHostsPredicate"
                    },
                }],
                "host_selection_retry_max_attempts": 5,
            }

    if knobs["stats_bind"]:
        get_only = [{"name": ":method", "string_match": {"exact": "GET"}}]
        resources["listeners"].append({
            "name": "quantra_stats_listener",
            "address": {
                "socket_address": {"address": knobs["stats_bind"], "port_value": knobs["stats_port"]}
            },
            "filter_chains": [{
                "filters": [{
                    "name": "envoy.filters.network.http_connection_manager",
                    "typed_config": {
                        "@type": "type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager",
                        "codec_type": "AUTO",
                        "stat_prefix": "quantra_stats",
                        "route_config": {
                            "name": "stats_route",
                            "virtual_hosts": [{
                                "name": "quantra_stats",
                                "domains": ["*"],
                                "routes": [
                                    {"match": {"path": path, "headers": get_only},
                                     "route": {"cluster": ADMIN_CLUSTER}}
                                    for path in ("/stats/prometheus", "/ready")
                                ],
                            }],
                        },
                        "http_filters": [_router_filter()],
                    },
                }],
            }],
        })
        resources["clusters"].append({
            "name": ADMIN_CLUSTER,
            "connect_timeout": "0.25s",
            "type": "STATIC",
            "load_assignment": {
                "cluster_name": ADMIN_CLUSTER,
                "endpoints": [{"lb_endpoints": [{"endpoint": {"address": {
                    "socket_address": {"address": "127.0.0.1", "port_value": admin_port}
                }}}]}],
            },
        })
    return config
# --- END load control ---


def generate_envoy_config(
    port: int = 50051,
    base_port: int = 50055,
    workers: int = 4,
    admin_port: int = 9901,
    connect_timeout: str = "0.25s",
    health_check_interval: str = "1s",
    route_timeout: Optional[str] = None,
    grpc_timeout_header_max: Optional[str] = None,
    max_requests: Optional[int] = None,
    max_pending: Optional[int] = None,
    retries: Optional[int] = None,
    stats_bind: Optional[str] = None,
    stats_port: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Generate Envoy configuration for load balancing across Quantra workers.

    Args:
        port: Client-facing port (Envoy listens here)
        base_port: First worker port (workers use base_port, base_port+1, ...)
        workers: Number of worker processes
        admin_port: Envoy admin interface port
        connect_timeout: Connection timeout for upstream workers
        health_check_interval: Health check interval
        route_timeout: Overall per-request route timeout. Envoy aborts the
            request (504 to the client) once this elapses, so a worker stuck on
            a slow computation cannot pin the connection indefinitely. None
            (the default) reads QUANTRA_ROUTE_TIMEOUT_MS (default 30s).
        grpc_timeout_header_max: Upper bound applied to a client-supplied
            grpc-timeout header. The client's own deadline is still honored, but
            it can never exceed this cap. For gRPC requests this, not the
            route timeout, bounds the request (Envoy replaces the route timeout
            with the caller's grpc-timeout, capped here). None (the default) is
            max(60s, route_timeout).

    Returns:
        Envoy configuration dictionary
    """
    if route_timeout is None:
        route_timeout = route_timeout_from_env()
    if grpc_timeout_header_max is None:
        grpc_timeout_header_max = grpc_timeout_header_max_for(route_timeout)

    # Build endpoint list for all workers
    endpoints = []
    for i in range(workers):
        endpoints.append({
            "endpoint": {
                "address": {
                    "socket_address": {
                        "address": "127.0.0.1",
                        "port_value": base_port + i
                    }
                }
            }
        })

    config = {
        "admin": {
            "address": {
                "socket_address": {
                    "address": "127.0.0.1",
                    "port_value": admin_port
                }
            }
        },
        "static_resources": {
            "listeners": [{
                "name": "quantra_listener",
                "address": {
                    "socket_address": {
                        "address": "0.0.0.0",
                        "port_value": port
                    }
                },
                "filter_chains": [{
                    "filters": [{
                        "name": "envoy.filters.network.http_connection_manager",
                        "typed_config": {
                            "@type": "type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager",
                            "codec_type": "AUTO",
                            "stat_prefix": "quantra_grpc",
                            "route_config": {
                                "name": "local_route",
                                "virtual_hosts": [{
                                    "name": "quantra_service",
                                    "domains": ["*"],
                                    "routes": [{
                                        "match": {"prefix": "/"},
                                        "route": {
                                            "cluster": "quantra_workers",
                                            "timeout": route_timeout,
                                            "max_stream_duration": {
                                                "grpc_timeout_header_max": grpc_timeout_header_max
                                            }
                                        }
                                    }]
                                }]
                            },
                            "http_filters": [{
                                "name": "envoy.filters.http.router",
                                "typed_config": {
                                    "@type": "type.googleapis.com/envoy.extensions.filters.http.router.v3.Router"
                                }
                            }]
                        }
                    }]
                }]
            }],
            "clusters": [{
                "name": "quantra_workers",
                "connect_timeout": connect_timeout,
                "type": "STATIC",
                # LEAST_REQUEST samples choice_count workers at random (with
                # replacement) and picks the one with the fewest in-flight
                # requests; Envoy 1.28 has no full scan. Workers are
                # single-threaded, so a request lands behind a busy one only
                # when every sample is busy: with k of n busy, (k/n)^2 at the
                # default of 2 samples, (k/n)^(4n) here (about 1% with 3 of 4
                # busy).
                "lb_policy": "LEAST_REQUEST",
                "least_request_lb_config": {
                    "choice_count": least_request_choice_count(workers)
                },
                "typed_extension_protocol_options": {
                    "envoy.extensions.upstreams.http.v3.HttpProtocolOptions": {
                        "@type": "type.googleapis.com/envoy.extensions.upstreams.http.v3.HttpProtocolOptions",
                        "explicit_http_config": {
                            "http2_protocol_options": {}
                        }
                    }
                },
                "circuit_breakers": {
                    "thresholds": [{
                        "priority": "DEFAULT",
                        "max_connections": 10000,
                        "max_requests": 10000,
                        "max_pending_requests": 10000
                    }]
                },
                "health_checks": [{
                    "timeout": "1s",
                    "interval": health_check_interval,
                    "unhealthy_threshold": 2,
                    "healthy_threshold": 1,
                    # gRPC health check against the standard grpc.health.v1.Health
                    # service the engine serves. An empty service name asks for the
                    # server's overall serving status. A worker busy on a long
                    # computation still answers here (health runs on gRPC's own
                    # threads), so only genuinely dead workers are ejected.
                    "grpc_health_check": {}
                }],
                "load_assignment": {
                    "cluster_name": "quantra_workers",
                    "endpoints": [{
                        "lb_endpoints": endpoints
                    }]
                }
            }]
        }
    }

    knobs = resolve_load_control(max_requests, max_pending, retries, stats_bind, stats_port)
    return apply_load_control(config, knobs, admin_port)


def write_envoy_config(config: Dict[str, Any], output_path: str, format: str = "yaml") -> None:
    """
    Write Envoy configuration to file.
    
    Args:
        config: Envoy configuration dictionary
        output_path: Output file path
        format: Output format ('yaml' or 'json')
    """
    path = Path(output_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    
    with open(path, "w") as f:
        if format == "yaml":
            if not HAS_YAML:
                raise ImportError("PyYAML is required for YAML output. Install with: pip install pyyaml")
            yaml.dump(config, f, default_flow_style=False)
        else:
            json.dump(config, f, indent=2)


def check_cluster_health(admin_port: int = 9901) -> Dict[str, Any]:
    """
    Check health of all workers via Envoy admin API.
    
    Args:
        admin_port: Envoy admin port
        
    Returns:
        Dictionary with health status for each worker
    """
    import urllib.request
    
    url = f"http://localhost:{admin_port}/clusters?format=json"
    
    try:
        with urllib.request.urlopen(url, timeout=5) as response:
            data = json.loads(response.read().decode())
            
            results = {"healthy": [], "unhealthy": [], "all_healthy": True}
            
            for cluster in data.get("cluster_statuses", []):
                if cluster.get("name") == "quantra_workers":
                    for host in cluster.get("host_statuses", []):
                        addr = host.get("address", {}).get("socket_address", {})
                        port = addr.get("port_value", "?")
                        
                        health = host.get("health_status", {})
                        is_unhealthy = isinstance(health, dict) and any(
                            k.startswith("failed") for k in health.keys()
                        )
                        
                        if is_unhealthy:
                            results["unhealthy"].append(port)
                            results["all_healthy"] = False
                        else:
                            results["healthy"].append(port)
            
            return results
            
    except Exception as e:
        return {"error": str(e), "all_healthy": False}


def print_health_status(admin_port: int = 9901) -> bool:
    """
    Print health status of all workers.
    
    Args:
        admin_port: Envoy admin port
        
    Returns:
        True if all workers are healthy
    """
    results = check_cluster_health(admin_port)
    
    if "error" in results:
        print(f"Could not check health: {results['error']}")
        return False
    
    for port in results.get("healthy", []):
        print(f"  Port {port}: ✓ healthy")
    
    for port in results.get("unhealthy", []):
        print(f"  Port {port}: ✗ UNHEALTHY")
    
    if results["all_healthy"]:
        print("\n✓ All workers healthy")
    else:
        print("\n✗ Some workers unhealthy")
    
    return results["all_healthy"]


def main():
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Generate Envoy configuration for Quantra load balancing",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Generate config file
  python3 envoy_config.py --workers 4 --output envoy.yaml
  
  # Generate with custom ports
  python3 envoy_config.py --port 8080 --base-port 9000 --workers 8 --output envoy.yaml
  
  # Output as JSON
  python3 envoy_config.py --workers 4 --format json --output envoy.json
  
  # Check worker health
  python3 envoy_config.py --health --admin-port 9901
        """
    )
    
    parser.add_argument("-w", "--workers", type=int, default=4,
                        help="Number of worker processes (default: 4)")
    parser.add_argument("-p", "--port", type=int, default=50051,
                        help="Client-facing port (default: 50051)")
    parser.add_argument("--base-port", type=int, default=50055,
                        help="First worker port (default: 50055)")
    parser.add_argument("--admin-port", type=int, default=9901,
                        help="Envoy admin port (default: 9901)")
    parser.add_argument("--request-timeout", type=str, default=None,
                        help="Per-request route timeout as an Envoy duration "
                             "(e.g. 30s); Envoy returns 504 once it elapses "
                             "(default: QUANTRA_ROUTE_TIMEOUT_MS, else 30s)")
    parser.add_argument("--grpc-timeout-header-max", type=str, default=None,
                        help="Cap applied to a client-supplied grpc-timeout "
                             "header (default: max(60s, route timeout))")
    parser.add_argument("--max-requests", type=int, default=None,
                        help="Calls in flight across all workers before Envoy refuses "
                             "the next one with RESOURCE_EXHAUSTED / HTTP 429 "
                             "(default: QUANTRA_MAX_REQUESTS, else 10000 = no limit "
                             "in practice, overflow answered UNAVAILABLE)")
    parser.add_argument("--max-pending", type=int, default=None,
                        help="Calls waiting for a connection to a worker "
                             "(default: QUANTRA_MAX_PENDING, else 10000)")
    parser.add_argument("--retries", type=int, default=None,
                        help="Resend a call whose worker died or could not be "
                             "reached, up to N times (default: QUANTRA_RETRIES, else 0)")
    parser.add_argument("--stats-bind", type=str, default=None,
                        help="Address of a read-only listener serving only "
                             "GET /stats/prometheus and GET /ready "
                             "(default: QUANTRA_STATS_BIND, else none)")
    parser.add_argument("--stats-port", type=int, default=None,
                        help="Port of that listener (default: QUANTRA_STATS_PORT, else 9902)")
    parser.add_argument("-o", "--output", type=str, default=None,
                        help="Output file path (default: stdout)")
    parser.add_argument("--format", choices=["yaml", "json"], default="yaml",
                        help="Output format (default: yaml)")
    parser.add_argument("--health", action="store_true",
                        help="Check worker health instead of generating config")
    
    args = parser.parse_args()
    
    # Health check mode
    if args.health:
        print("Checking worker health...")
        success = print_health_status(args.admin_port)
        sys.exit(0 if success else 1)
    
    # Generate config
    try:
        resolve_load_control(args.max_requests, args.max_pending, args.retries,
                             args.stats_bind, args.stats_port)
    except ValueError as e:
        parser.error(str(e))
    config = generate_envoy_config(
        port=args.port,
        base_port=args.base_port,
        workers=args.workers,
        admin_port=args.admin_port,
        route_timeout=args.request_timeout,
        grpc_timeout_header_max=args.grpc_timeout_header_max,
        max_requests=args.max_requests,
        max_pending=args.max_pending,
        retries=args.retries,
        stats_bind=args.stats_bind,
        stats_port=args.stats_port,
    )
    
    if args.output:
        write_envoy_config(config, args.output, format=args.format)
        print(f"Generated Envoy config: {args.output}")
        print(f"  Client port: {args.port}")
        print(f"  Worker ports: {args.base_port}-{args.base_port + args.workers - 1}")
        print(f"  Admin port: {args.admin_port}")
    else:
        # Output to stdout
        if args.format == "yaml":
            if not HAS_YAML:
                print("Error: PyYAML required. Install with: pip install pyyaml", file=sys.stderr)
                sys.exit(1)
            print(yaml.dump(config, default_flow_style=False))
        else:
            print(json.dumps(config, indent=2))


if __name__ == "__main__":
    main()