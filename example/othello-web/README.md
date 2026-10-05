# Othello & Reversi Web Example

A web-based Othello and Reversi game implementation powered by the CWIST C web framework.

## Features
- Built with the high-performance **CWIST** C web framework.
- Supports single-player (vs AI bot) and multi-player room-based matchmaking.
- Real-time game state synchronization using SQLite and JSON APIs.
- Optional HTTPS support via OpenSSL/BoringSSL.

## Building and Running

### Prerequisites
Make sure `libcwist.a` is built at the project root:
```bash
make
```

### Build Othello Web Server
```bash
cd example/othello-web
make
```

### Run
Generate self-signed certificates (optional):
```bash
./keygen.sh
```

Run server (default port: `31744`):
```bash
./server
```
Or run without HTTPS:
```bash
./server --no-certs
```

Access the game in your browser at `https://localhost:31744` (or `http://localhost:31744` if run with `--no-certs`).
