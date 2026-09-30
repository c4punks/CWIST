# rust-hello

A minimal CWIST application written in Rust using the `cwist` safe wrapper.

## Build

CWIST must be built and installed first so `cwist-sys` can find it through
`pkg-config`:

```bash
make && make install PREFIX=$HOME/.local
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig
```

Then build the example:

```bash
cargo build --release --manifest-path example/rust-hello/Cargo.toml
```

## Run

```bash
cargo run --manifest-path example/rust-hello/Cargo.toml
```

The server listens on `http://127.0.0.1:8080` and serves:

- `GET /` → `Hello, World!`
- `GET /users/:id` → `user <id>`
