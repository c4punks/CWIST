# Tutorial 25: Big Dumb Reply (BDR) Cache

Bypass heavy application allocation for static responses using the Big Dumb Reply zero-copy response engine.

## Key Concepts
- Storing a pre-serialized HTTP response with `cwist_bdr_put()` and
  retrieving it zero-copy with `cwist_bdr_get()`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut25
```
