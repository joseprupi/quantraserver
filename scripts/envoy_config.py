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
import json
import sys
from pathlib import Path
from typing import Dict, Any, Optional

try:
    import yaml
    HAS_YAML = True
except ImportError:
    HAS_YAML = False


def generate_envoy_config(
    port: int = 50051,
    base_port: int = 50055,
    workers: int = 4,
    admin_port: int = 9901,
    connect_timeout: str = "0.25s",
    health_check_interval: str = "1s",
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
        
    Returns:
        Envoy configuration dictionary
    """
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
                                            "timeout": "0s",
                                            "max_stream_duration": {
                                                "grpc_timeout_header_max": "0s"
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
                "lb_policy": "ROUND_ROBIN",
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
                    "tcp_health_check": {}
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

    return config


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
    config = generate_envoy_config(
        port=args.port,
        base_port=args.base_port,
        workers=args.workers,
        admin_port=args.admin_port,
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