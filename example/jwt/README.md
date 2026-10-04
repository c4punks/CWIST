# JWT examples

Two small programs that use the JWT helpers in
`include/cwist/security/jwt/jwt.h`: signing and verifying a token directly, and
protecting an HTTP route with a bearer token.

| Step | Shows | Port |
|------|-------|------|
| [`step-1-sign-verify`](step-1-sign-verify/main.c) | `cwist_jwt_sign`, `cwist_jwt_verify`, `cwist_jwt_claims_get`, and rejection of a tampered token | none (prints and exits) |
| [`step-2-http-auth`](step-2-http-auth/main.c) | issuing a token from `POST /login` and checking it on `GET /profile` | 8084 |

Both steps use a hard-coded HMAC secret (`my-super-secret-key` in step 1,
`change-me-in-production` in step 2). They are placeholders for the example;
do not reuse them.

## Build

Build the library first, from the repository root:

```sh
make
```

Then build a step:

```sh
make -C example/jwt/step-1-sign-verify
make example/jwt/step-2-http-auth/jwt-auth
```

`make examples` builds step 2, and `make examples-check` builds both steps.

## Step 1: sign and verify

```sh
./example/jwt/step-1-sign-verify/step-1-sign-verify
```

The program signs the payload `{"sub":"42","name":"Alice","role":"admin"}` with
a lifetime of 3600 seconds, verifies the token, prints three claims, then flips
one bit of the token and verifies it again. The output looks like this (the
token value differs on every run because it contains the issue time):

```
=== JWT Sign & Verify ===

[Sign]
Token: <header>.<payload>.<signature>

[Verify]
sub  : 42
name : Alice
role : admin
exp  : (null)

[Tampered token]
Tampered verify result: rejected (correct)

=== Done ===
```

`exp` prints `(null)` although the token contains an `exp` claim.
`cwist_jwt_claims_get` returns the value only for string claims, and `exp` and
`iat` are numbers, so it returns `NULL` for them. `cwist_jwt_verify` still
checks `exp` itself and rejects an expired token.

## Step 2: protect a route with a bearer token

Start the server:

```sh
./example/jwt/step-2-http-auth/jwt-auth
```

It listens on port 8084 and has two routes:

* `POST /login` signs a token for the user `demo` that is valid for 3600
  seconds and returns it as `{"token":"..."}`. It does not check any
  credentials.
* `GET /profile` requires an `Authorization: Bearer <token>` header and
  returns the user name from the token.

In another terminal, call the protected route without a token, log in, and call
it again with the token:

```sh
curl -i http://127.0.0.1:8084/profile
curl -s -X POST http://127.0.0.1:8084/login
curl -i -H "Authorization: Bearer <token>" http://127.0.0.1:8084/profile
```

| Request | Status | Body |
|---------|--------|------|
| `GET /profile` with no header | `401` | `{"error":"missing or malformed Authorization header"}` |
| `GET /profile` with a valid token | `200` | `{"user":"demo","message":"Welcome to your profile!"}` |
| `GET /profile` with a modified token | `401` | `{"error":"invalid or expired token"}` |

Stop the server with `Ctrl+C`.
