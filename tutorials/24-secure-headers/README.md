# Tutorial 24: Hardened HTTP Security Headers

Inject a full set of security hardening headers into every response using
`cwist_http_response_add_security_headers()`.

## Key Concepts
- Calling `cwist_http_response_add_security_headers()` to set all seven
  standard hardening headers in one call (X-Frame-Options,
  X-Content-Type-Options, Referrer-Policy, Content-Security-Policy,
  Cross-Origin-Resource-Policy, Permissions-Policy,
  Cross-Origin-Opener-Policy).
- First-one-wins semantics: set a header before calling the function to
  override the default value for that header.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut24
```
