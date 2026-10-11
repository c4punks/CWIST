# HTTPS C100k gate

`scripts/ci/c100k_https.sh` checks that one CWIST HTTPS server holds and
serves 100,000 concurrent TLS connections. CI runs it in
`.github/workflows/c100k.yml`.

## What runs

- Server: `tests/rss_gate_server` on the normal `cwist_app_listen()` path
  with 4 prefork workers and a throwaway ECDSA P-256 certificate.
  `CWIST_HTTPS_IDLE_TIMEOUT_MS` is set to 600000 so the idle budget outlasts
  the ramp and the hold.
- Client: `tests/c100k_https_client`, plain OpenSSL + epoll, 4 threads, at
  most 512 handshakes in flight per thread.

The test has three phases:

1. **Ramp.** Open 100,000 connections (TCP and TLS 1.3 handshake). All of
   them stay open.
2. **Hold.** Leave every connection idle for 5 s. The server must not close
   any of them. Idle HTTPS connections live in the park set.
3. **Rounds.** Two keep-alive rounds. Each round writes one `GET /` on every
   connection before it reads any response. Every response must be a 200
   with the expected body.

PASS requires all of the following:

- 100,000 connections established.
- No connection dropped.
- Every request answered in every round.
- The ramp finishes within 180 s.
- Server RSS with all connections open stays at or below 16 KiB per
  connection, measured above the idle baseline.

Each limit can be overridden with an environment variable. The variables
are listed in the script header.

## Loopback port space

The client spreads connections over 127.0.0.1 to 127.0.0.4. The server binds
`0.0.0.0`. Each destination address has its own ephemeral-port space, so
the default Linux range (32768 to 60999, about 28k ports) gives about 112k
ports across the four addresses.

With only two addresses and the ports nearly used up, the ramp took 55 s
instead of 18.5 s. As the free ports run out, `connect()` takes longer to
find one. The slowdown came from the client side, not from the server.

## Results (local, 12 cores, 2026-10-11, dev)

| Mode | Established | Dropped | Round 1 | Round 2 | Ramp | RSS / open conn |
|------|-------------|---------|---------|---------|------|-----------------|
| C1M (default) | 100000 | 0 | 100000 ok, 73.7k rps | 100000 ok, 123.3k rps | 18.6 s | 3390 B |
| Classic (`CWIST_C1M_MODE=0`) | 100000 | 0 | 100000 ok, 62.1k rps | 100000 ok, 125.7k rps | 18.8 s | 3430 B |
| Full GC (`CWIST_TEST_FULL_GC=1`) | 100000 | 0 | 100000 ok, 74.7k rps | 100000 ok, 130.0k rps | 18.5 s | 3403 B |

Total server RSS was 26 MB idle and 357 MB with 100k connections open,
across the master and 4 workers.

## Running it

```sh
make rss_gate_server c100k_https_client
scripts/ci/c100k_https.sh                      # C1M
CWIST_C1M_MODE=0 scripts/ci/c100k_https.sh     # classic
CWIST_TEST_FULL_GC=1 scripts/ci/c100k_https.sh # full GC
```

The script needs a hard `RLIMIT_NOFILE` of at least 104096. The client and
the server each hold one fd per connection, in separate processes. CI raises
the limit with `prlimit`. It also widens `ip_local_port_range` and raises
`somaxconn`.
