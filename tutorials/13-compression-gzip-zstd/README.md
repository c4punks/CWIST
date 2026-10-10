# Tutorial 13: Payload Compression (Gzip / Zstandard)

Compress HTTP response payloads transparently using Gzip or Zstandard algorithms.

## Key Concepts
- Registering compression backends with `cwist_compress_register_backend()`.
- Built-in backends: `cwist_compress_backend_gzip`, `cwist_compress_backend_deflate`, `cwist_compress_backend_brotli`, `cwist_compress_backend_zstd`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut13
```
