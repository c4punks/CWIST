# Compile-time build options

CWIST feature knobs are set as Make variables. The main subsystem defaults are shown below; `CWIST_TINY=1` changes the HTTP/3, WebSocket, database, NATS, JWT, WebRTC, and WebTransport defaults to disabled.

| Option | Default | Purpose |
| --- | --- | --- |
| `CWIST_HTTP3` | `1` | Build HTTP/3 and QUIC support. |
| `CWIST_WS` | `1` | Build WebSocket support. |
| `CWIST_DB` | `1` | Build SQLite, ORM, and database support. |
| `CWIST_NATS` | `1` | Build NATS support. |
| `CWIST_JWT` | `1` | Build JWT support. |
| `CWIST_SQLITE_FTS5` | `0` | Enable SQLite FTS5 virtual tables and `MATCH` queries. |

Enable FTS5 with:

```sh
make CWIST_SQLITE_FTS5=1
```

The option adds `SQLITE_ENABLE_FTS5` only while compiling the vendored `lib/sqlite3/sqlite3.o`. It does not change application compiler flags or enable `CWIST_DB` when the database subsystem is disabled. Switching the option rebuilds SQLite with the selected configuration; running `make clean` is not required.

`CWIST_SQLITE_FTS5` accepts only `0` and `1`. Other values stop the build with an error.
