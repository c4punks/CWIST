# Zero-downtime restart via socket handoff

A CWIST server can be replaced by a new process — a new binary, a new
configuration, a new version — without dropping the listener. The new process
binds the same port while the old one is still serving, the old one is then
signalled to drain, and clients never see the port refuse a connection.

This uses `SO_REUSEPORT` socket sharing, not file-descriptor passing: there is
no handoff channel to set up, no supervisor protocol, and the two processes
never need to trust each other beyond sharing a port.

## Requirements

- **Linux kernel 3.9 or later** for `SO_REUSEPORT` on TCP. FreeBSD, macOS, and
  the other BSDs support it as well; where the option does not exist CWIST
  compiles it out and a second bind fails with `EADDRINUSE` as before.
- Both processes must set `SO_REUSEPORT`. CWIST sets it by default on every
  listen socket (TCP and the HTTP/3 UDP socket), so two CWIST processes — or
  two generations of the same binary — always qualify.
- `CWIST_REUSEPORT` controls the behavior. It defaults to on; setting it to
  `0` or `false` restores the classic "second bind fails" behavior, which some
  deployments prefer as a guard against accidentally starting two instances.
  Worker processes re-bind the port after `fork()`, so disabling the knob
  requires `CWIST_WORKERS=1`.

## Handoff sequence

1. Start the new process on the same port. The bind succeeds because of
   `SO_REUSEPORT`; the kernel now load-balances **new** connections across
   both processes' accept queues.
2. Send `SIGTERM` (or `SIGINT`) to the old process. CWIST's graceful shutdown
   closes the old listening sockets at once, so from that moment every new
   connection hashes onto the new process only.
3. The old process finishes in-flight requests — up to `CWIST_DRAIN_TIMEOUT`
   seconds, default 5, exiting early once nothing is in flight — and exits.
   Idle keep-alive connections it still holds are closed gracefully (FIN), so
   clients with the usual "retry on a stale keep-alive connection" logic
   reconnect onto the new process without a failed request.

Established connections are never migrated: a flow accepted by the old
process is served by the old process until it ends, and a flow accepted after
step 2 always reaches the new process. During the short overlap window
between steps 1 and 2 a new connection may still land on the old process; it
is served normally and closes at the drain, so size the drain timeout to your
longest expected request, not to the overlap window.

```sh
new-binary &                  # step 1: binds the same port via SO_REUSEPORT
sleep 1                       # let it finish startup and health-check itself
kill -TERM "$old_pid"         # steps 2-3: old process drains and exits
```

`tests/test_socket_handoff.c` exercises exactly this sequence: 256 held
keep-alive connections against the old process, the new process bound on top,
`SIGTERM` to the old one, then verification that every request still gets a
response (answered by the draining old process or by the new one after the
client reconnects) and that all fresh connections reach the new process.

## systemd integration

Two patterns work:

- **Overlap restart (recommended for CWIST).** Give the service its own
  restart logic: on deploy, start the new process as above and `SIGTERM` the
  old one. A `Type=simple` unit plus a small deploy script is enough; do not
  use `systemctl restart`, which stops the old process before starting the
  new one and reopens the downtime window this feature exists to close.
- **Socket activation (alternative).** Let systemd hold the listener:
  a `.socket` unit with `ListenStream=` passes an already-bound fd to each
  service instance, so `systemctl restart` never unbinds the port at all.
  This also survives full crashes (the socket outlives the process), at the
  cost of systemd-specific plumbing and no per-process accept queue.

## UDP and HTTP/3

The HTTP/3 (QUIC) UDP socket also gets `SO_REUSEPORT` by default, so the
overlap bind works the same way and new QUIC Initial packets reach whichever
process the kernel hashes them to. What does **not** migrate is connection
state: QUIC connections are identified by connection IDs whose encryption
keys live in the process that accepted them, and CWIST (lsquic) cannot hand a
live connection to another process. Datagrams for connections owned by the
draining process may be delivered to the new process, which cannot decrypt
them; the client's QUIC stack treats that as path loss and recovers by
retransmitting, and falls back to a fresh connection (or TLS session resumption
against the new process) in the worst case. In practice: TCP handoffs are
lossless, HTTP/3 handoffs are lossless at the request level but a small share
of QUIC connections may reset and reconnect. Plan HTTP/3 deploys accordingly —
drain longer, and prefer clients that tolerate a connection retry.

## Verifying a deploy

- The new process logs `CWIST App running on port ...` only after its bind
  and listen succeeded; if the port were unavailable it would exit instead.
- The old process logs `[CWIST] Draining connections for N seconds...` when
  it receives `SIGTERM` and exits 0 after the drain.
- During the overlap, `ss -ltnp 'sport = :PORT'` shows both processes on the
  same port.
