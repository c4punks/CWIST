# Tutorial 23: CORS Cross-Origin Policy

Configure Cross-Origin Resource Sharing (CORS) headers to allow frontend Single Page Applications (SPAs) to consume APIs safely.

## Key Concepts
- Automatic injection of CORS headers by attaching `cwist_mw_cors()` via `cwist_app_use(app, cwist_mw_cors())`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut23
```
