# simple-server

A single-file CWIST server that demonstrates the high-level app API: route
registration, HTML builder, CSS composer, and response body handling.

| Route | Method | Shows |
|-------|--------|-------|
| `/` | GET | HTML page composed with `cwist_html_builder` and `cwist_css_composer` |
| `/health` | GET | JSON response written directly to `res->body` |
| `/echo` | POST | Request body mirrored back with the original `Content-Type` |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build the server:

```sh
make -C example/simple-server
```

## Run

```sh
./example/simple-server/simple_server
```

The server listens on port 8080. Test the endpoints:

```sh
curl http://localhost:8080/
curl http://localhost:8080/health
curl -X POST http://localhost:8080/echo -H 'Content-Type: text/plain' -d 'hello'
```

## Clean

```sh
make -C example/simple-server clean
```
