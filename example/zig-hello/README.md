# zig-hello

A minimal CWIST application written in Zig, using the bindings in
`bindings/zig` (Zig 0.17.0).

## Build

CWIST must be built and installed first so the bindings can find it through
`pkg-config`:

```bash
make && make install PREFIX=$HOME/.local
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig
```

Then build and run the example:

```bash
cd example/zig-hello
zig build run
```

The server listens on port 8080 (all IPv4 interfaces) and serves:

- `GET /` -> `Hello, World!`
- `GET /users/:id` -> `user <id>`

Stop it with Ctrl-C (SIGINT) or SIGTERM; it shuts down gracefully.
