# cwist-sys

Raw FFI bindings to [CWIST](https://github.com/c4punks/CWIST), a C17 web
framework and application server.

This crate uses `bindgen` to generate Rust bindings from the installed CWIST
headers and links `libcwist.a` through `pkg-config`. You must build and install
CWIST separately before building this crate.

```sh
# Install CWIST first, for example:
make && make install PREFIX=$HOME/.local
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig

cargo add cwist-sys
```

See the top-level `bindings/rust/` workspace and the `cwist` crate for the safe
Rust wrapper.
