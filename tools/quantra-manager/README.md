# Quantra Process Manager

A CLI tool to manage multiple Quantra server processes with optional Envoy load balancing.

## Why Multi-Process?

QuantLib is single-threaded due to its use of global state (e.g., `Settings::instance().evaluationDate()`). 
To achieve parallelism, we run multiple independent processes, each handling requests on its own port.
Envoy provides transparent load balancing across these processes.

## Architecture

```
                    Client Request
                          │
                          ▼
                 ┌─────────────────┐
                 │  Envoy Proxy    │  Port 50051
                 │  (Round Robin)  │
                 └────────┬────────┘
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
   ┌─────────┐       ┌─────────┐       ┌─────────┐
   │ Worker 1│       │ Worker 2│       │ Worker N│
   │  :50055 │       │  :50056 │       │ :50055+N│
   └─────────┘       └─────────┘       └─────────┘
```

## Installation

```bash
# Copy to your quantra installation
cp quantra /usr/local/bin/
cp quantra-simple /usr/local/bin/

# Or add to PATH
export PATH=$PATH:/path/to/quantra-manager

# Set QUANTRA_HOME (optional, defaults to current directory)
export QUANTRA_HOME=/path/to/quantraserver
```

## Dependencies

**Full version (`quantra`):**
- Python 3.6+
- PyYAML (`pip install pyyaml`)
- Envoy proxy (https://www.envoyproxy.io/)

**Simple version (`quantra-simple`):**
- Python 3.6+ (no additional dependencies)

## Usage

### Full Version (with Envoy load balancing)

```bash
# Start cluster with 8 workers
quantra start --workers 8

# Start with custom port
quantra start --workers 4 --port 9000

# Check status
quantra status

# Check health via Envoy admin API
quantra health

# View logs
quantra logs
quantra logs --follow

# Restart with different worker count
quantra restart --workers 16

# Stop cluster
quantra stop
```

### Simple Version (no Envoy)

```bash
# Start 4 workers on ports 50051-50054
quantra-simple start --workers 4 --base-port 50051

# Check status
quantra-simple status

# Stop all workers
quantra-simple stop
```

## Configuration

### Environment Variables

- `QUANTRA_HOME` - Path to quantraserver installation (default: current directory)

### Default Ports

- `50051` - Client-facing gRPC port (Envoy)
- `50055+` - Worker ports (50055, 50056, ...)
- `9901` - Envoy admin API

## Files

The process manager creates files in `$QUANTRA_HOME/.quantra/`:

```
.quantra/
├── quantra.pid          # Process IDs (JSON)
├── envoy.yaml           # Generated Envoy config
└── logs/
    ├── envoy.log
    ├── worker_50055.log
    ├── worker_50056.log
    └── ...
```

## Docker Usage

In your Dockerfile:

```dockerfile
# Install Envoy
RUN apt-get update && apt-get install -y envoy

# Copy process manager
COPY quantra /usr/local/bin/

# Start command
CMD ["quantra", "start", "--workers", "10"]
```

In docker-compose.yml:

```yaml
services:
  quantra:
    build: .
    ports:
      - "50051:50051"
    environment:
      - QUANTRA_HOME=/app
    command: quantra start --workers 10
```

## Kubernetes Usage

The process manager runs inside each pod, managing local workers.
K8s service handles distribution across pods.

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: quantra
spec:
  replicas: 3  # 3 pods
  template:
    spec:
      containers:
      - name: quantra
        command: ["quantra", "start", "--workers", "10"]  # 10 workers per pod
        ports:
        - containerPort: 50051
```

Total capacity: 3 pods × 10 workers = 30 parallel requests

## Troubleshooting

### Workers not starting

```bash
# Check if ports are available
netstat -tlnp | grep 5005

# Check logs
quantra logs

# Verify binary exists
ls -la $QUANTRA_HOME/build/server/sync_server
```

### Envoy not connecting to workers

```bash
# Check Envoy admin
curl http://localhost:9901/clusters

# Check worker health
quantra health
```

### Performance tuning

```bash
# More workers for CPU-bound workloads
quantra start --workers $(nproc)

# Adjust Envoy circuit breakers in generated config
# Edit .quantra/envoy.yaml if needed
```

## Comparison with Original Scripts

The `quantra` CLI provides several improvements over the original `start.sh`:

- Automatic PID tracking (vs. manual process management)
- Built-in `restart` command
- `status` and `health` commands
- Centralized log management with `logs` command
- CLI arguments with sensible defaults (vs. environment variables)

## License

Same as Quantra (MIT / Apache 2.0)
