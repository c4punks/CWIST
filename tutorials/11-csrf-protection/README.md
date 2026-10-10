# Tutorial 11: CSRF Defense Protection

Generate and validate CSRF tokens to protect state-changing HTTP form submissions.

## Key Concepts
- Registering CSRF middleware with `cwist_app_use(app, cwist_mw_csrf(app))`.
- Reading the per-request token from `cwist_csrf_token(req)` to embed in HTML forms.
- Validation is automatic: the middleware rejects POST/PUT/PATCH/DELETE requests whose token does not match.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut11
```
