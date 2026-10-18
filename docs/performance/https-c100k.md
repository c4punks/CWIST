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

## Results (2026-10-17, dev)

Local machine, 12 cores, C1M reactor mode (default), HTTPS gate server
`tests/rss_gate_server` (4 prefork workers) driven by
`tests/c100k_https_client` through `scripts/ci/c100k_https.sh`:

| Scale | Established | Dropped idle | Round 1 | Round 2 | Ramp | Memory / open conn |
|-------|-------------|--------------|---------|---------|------|--------------------|
| C10K (`C100K_CONNS=10000`) | 10000 | 0 | 10000 ok, 64.0k rps | 10000 ok, 115.9k rps | 1.97 s | 5906 B |
| C100K (default) | 100000 | 0 | 100000 ok, 73.0k rps | 100000 ok, 106.4k rps | 18.14 s | 4975 B |

Both runs PASS every gate criterion (ramp under 180 s, no dropped
connection, every request answered, memory at most 8192 B per open
connection). The per-connection figure is consistent with the ~5 KB
parked-connection cost measured 2026-10-11 after parked connections
started releasing their 16 KiB read buffer.

## Results (2026-10-18, dev): C1M over TLS

Same server, driven by 8 `c100k_https_client` processes x 125,000
connections through `scripts/ci/c1m_local.sh` inside an unprivileged
user+network namespace (`unshare -Urn`), clients linked against the
repo's BoringSSL:

| Scale | Established | Dropped idle | Ramp | Rounds | Server peak |
|-------|-------------|--------------|------|--------|-------------|
| C1M (8x125K, TLS 1.3) | 1,000,000 | 0 | 328-341 s per client | all 2,000,000 answered, 0 failed | 4.56 GiB (4866 B/conn) |

The ramp is TLS-handshake-bound, not server-bound: ~2,900 full P-256
handshakes/s across the 8 clients while 12 cores are shared with the
server, so opening 1M connections takes ~340 s (C100K: 18 s). Every
client reported 125000 ok in both keep-alive rounds. The server holds
~4.9 KB per open TLS connection — the same parked-connection cost as
C100K. The whole client fleet resident-uses ~7 GiB thanks to BoringSSL
honoring `SSL_MODE_RELEASE_BUFFERS` (the OpenSSL build idles at ~4.2 GiB
per 125K-connection client; see below).

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

## Scaling past the gate on a single host

Five limits gate a 1M-connection HTTPS run, all outside CWIST. The first
two are why `c1m_local.sh` self-configures its namespace; the last three
were found by failing at each of them in turn:

- **conntrack table.** Every loopback flow occupies an entry wherever
  conntrack is active. The host's `nf_conntrack_max` of 262144 froze one
  2026-10-17 attempt at exactly 262090 connections (the rest of the table
  was unrelated traffic), recycling at ~180/s as entries timed out. A fresh
  network namespace has no conntrack hooks at all (nothing references it),
  so `unshare -Urn` sidesteps the table entirely without touching the host.
- **Ephemeral port range.** A fresh network namespace starts from kernel
  defaults: `ip_local_port_range` is 32768-60999. With three destination
  addresses per client that caps a client at exactly 3 x 28232 = 84696
  connections — every further `connect()` fails with EADDRNOTAVAIL, and
  the client burns the rest of its ramp deadline retrying. The script
  widens the range to 1024-65535 itself (written via `/proc/sys`, because
  `sysctl(8)` lives in `/usr/sbin`, off a bare `unshare` PATH).
- **Per-client address isolation.** With all clients spreading over the
  same 127.0.0.1-3 addresses, their ephemeral-port ranges collide in the
  kernel 4-tuple space and the ramp deadlocks around ~180k total
  connections: client `connect()` burns kernel time retrying, there are no
  SYN retransmits, and the server sits idle. Giving each client its own
  loopback /16 (127.0.<client+1>.1-3) removes the overlap, because the
  destination address is part of the 4-tuple.
- **SIGPIPE in the load client.** When the server closes any connection
  under extreme load, the client's next request write raises SIGPIPE and
  the default action kills the whole client — observed as silent deaths
  (exit 141, empty log) at 500k+ connections. `c100k_https_client` now
  ignores SIGPIPE so the failed write is counted as a round failure.
- **Client fleet memory.** An idle TLS connection costs the server ~5 KB
  but the *client* keeps two 16 KiB buffers per connection — unless the
  TLS stack honors `SSL_MODE_RELEASE_BUFFERS`. BoringSSL (what the repo
  vendors and the server uses) does; system OpenSSL does not, inflating
  an idle 125K-connection client from ~0.9 GiB to ~4.2 GiB. For 8x125K
  the fleet needs ~7 GiB linked against BoringSSL versus ~34 GiB against
  OpenSSL. Build the BoringSSL client with:
  ```sh
  gcc -O2 -o c100k_bssl_client tests/c100k_https_client.c \
      -Ilib/boringssl/include lib/boringssl/build/libssl.a \
      lib/boringssl/build/libcrypto.a -lpthread -ldl -lstdc++
  ```

`scripts/ci/c1m_local.sh` automates the 8x125K run: per-client loopback
/16s, namespace self-configuration, server RSS+swap sampling, and
per-client gate accounting. Example:

```sh
make rss_gate_server c100k_https_client
unshare -Urn bash -c '
  ip link set lo up
  C1M_CLIENT_BIN=./c100k_bssl_client scripts/ci/c1m_local.sh
'
```

On a host with less free RAM, stagger the client starts so at most one
ramp is in flight (`C1M_STAGGER_S=60`) and keep earlier clients' sockets
open with `C1M_LINGER_S` until the last client finishes; both trade wall
time for peak memory.

## Running it

```sh
make rss_gate_server c100k_https_client
scripts/ci/c100k_https.sh                      # C1M
scripts/ci/c1m_local.sh                        # C1M scale: 8x125K clients
CWIST_C1M_MODE=0 scripts/ci/c100k_https.sh     # classic
CWIST_TEST_FULL_GC=1 scripts/ci/c100k_https.sh # full GC
```

The script needs a hard `RLIMIT_NOFILE` of at least 104096. The client and
the server each hold one fd per connection, in separate processes. CI raises
the limit with `prlimit`. It also widens `ip_local_port_range` and raises
`somaxconn`.
