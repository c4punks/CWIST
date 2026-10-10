# Tutorial 21: WAF Security Inspection

Inspect query parameters and request bodies against malicious injection patterns with `cwist_waf_is_safe`.

## Key Concepts
- Attaching the lightweight WAF middleware with `cwist_app_use(app, cwist_mw_waf_lite())`.
- Checking individual strings with `cwist_waf_is_safe(str, len)` before processing.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut21
```
