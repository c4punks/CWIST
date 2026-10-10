# Tutorial 12: Cookie & Session Management

Issue secure HTTP-only cookies and manage server-side user session lifecycles.

## Key Concepts
- Starting a session with `cwist_session_start(NULL, req, res)`.
- Storing values with `cwist_session_set(sess, "key", "value")`.
- Flushing the session cookie with `cwist_session_commit(sess, res)`.
- Releasing session memory with `cwist_session_destroy(sess)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut12
```
