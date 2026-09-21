# Tutorial 22: Throughput Rate Limiting Middleware

Protect sensitive API endpoints from Denial-of-Service (DoS) flooding using rate-limiting middleware.

## Key Concepts
- Attaching a per-IP sliding-window rate limiter with `cwist_mw_rate_limit_ip(requests_per_minute)`.
- The middleware responds with `429 Too Many Requests` when the limit is exceeded and short-circuits the handler chain.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut22
```
