# Tutorial 08: Server-Sent Events (SSE) Real-Time Streaming

Set up unidirectional real-time event streaming over HTTP using Server-Sent Events.

## Key Concepts
- Initialize SSE response headers using `cwist_sse_response_init(res)`.
- Dispatch formatted event chunks with `cwist_sse_response_event(res, event_name, id, retry_ms, data)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut08
```

Test with `curl`:
```bash
curl -N http://127.0.0.1:8087/events
```
