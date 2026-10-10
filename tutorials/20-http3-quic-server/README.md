# Tutorial 20: HTTP/3 & QUIC Transport Server

Run high-speed HTTP/3 servers over QUIC transport via lsquic integration.

## Key Concepts
- Initializing a QUIC/TLS context with `cwist_http3_init_context_ephemeral(&ctx)`.
- Running the server loop with `cwist_http3_server_loop(udp_fd, ctx, handler, user_ctx)`.
- Tearing down the context with `cwist_http3_destroy_context(ctx)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut20
```
