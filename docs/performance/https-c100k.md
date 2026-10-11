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
- Server memory with all connections open stays at or below 8 KiB per
  connection, measured above the idle baseline. (The limit was 16 KiB
  until parked connections released their read buffer; see below.)

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

## Results (2026-10-11, dev)

| Mode | Machine | Established | Dropped | Round 1 | Round 2 | Ramp | Memory / open conn |
|------|---------|-------------|---------|---------|---------|------|--------------------|
| C1M (default) | local, 12 cores | 100000 | 0 | 100000 ok, 71.1k rps | 100000 ok, 110.2k rps | 19.4 s | 13189 B |
| Classic (`CWIST_C1M_MODE=0`) | local, 12 cores | 100000 | 0 | 100000 ok, 74.9k rps | 100000 ok, 113.0k rps | 19.3 s | 13218 B |
| Full GC (`CWIST_TEST_FULL_GC=1`) | local, 12 cores | 100000 | 0 | 100000 ok, 60.9k rps | 100000 ok, 107.4k rps | 19.7 s | 13208 B |
| C1M / classic / full GC | GitHub runner, 4 cores | 100000 | 0 | 100000 ok, ~33k rps | 100000 ok, ~52-56k rps | ~33.8 s | ~13.2 KB |

Total server memory is 32 MB idle and about 1.3 GB with 100k connections
open, across the master and 3 forked workers. With `CWIST_WORKERS=4` the
master serves connections too.

### Where the memory goes

The table above was measured before parked connections released their read
buffer. Since then, the gate measures ~5.0 KB per open connection locally
(C1M 4982 B, full GC 5064 B, 2026-10-11). Total server memory with 100k
connections open dropped from ~1.3 GB to ~0.52 GB.

- Before the change, the connection's 16 KiB `read_buf` stayed allocated
  while the connection was idle in the park set. `cwist_https_park()` now
  frees it when nothing is buffered, and `cwist_https_receive_request()`
  allocates a new one when the connection is served again.
- The remaining ~5 KB is the TLS session state (BoringSSL `SSL` object)
  plus the conn, park entry and allocator overhead.

An earlier local run reported ~3.4 KB per connection. That run was wrong. A
server left over from a previous run was still listening on the same port
with `SO_REUSEPORT`, so it received part of the connections. The script now
fails at start if the port already answers. It also counts swapped-out pages
together with RSS.

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
