# Tutorial 18: Kubernetes Health Check (/healthz)

Expose `/healthz` endpoints for Kubernetes probes and load balancer health checks.

## Key Concepts
- Enabling the `/healthz` endpoint with `cwist_app_enable_healthz(app)`, which returns JSON health status for infrastructure probes.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut18
```
