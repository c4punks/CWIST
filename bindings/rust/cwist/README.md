# cwist

Safe Rust bindings for [CWIST](https://github.com/c4punks/CWIST), a C17 web
framework and application server.

This crate wraps the raw FFI provided by `cwist-sys` with an idiomatic Rust API
for routing, middleware, request/response handling, and graceful shutdown.

```sh
# Install CWIST first; cwist-sys links it through pkg-config.
make && make install PREFIX=$HOME/.local
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig

cargo add cwist
```

## Quick example

```rust
use cwist::{App, Error};

fn main() -> Result<(), Error> {
    let mut app = App::new()?;
    app.get("/", |_req, res| {
        res.set_status(200);
        let _ = res.set_body("Hello, CWIST from Rust!");
    });
    app.listen(8080)
}
```
