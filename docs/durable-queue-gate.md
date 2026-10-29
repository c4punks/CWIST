# Durable queue required gate

`tests/test_durable_queue.c` covers enqueue/claim/ack, nack redelivery,
visibility timeout and dead-letter behavior on Redis and NATS JetStream.
Missing servers remain optional in local `make test` runs.

`CWIST_TEST_REQUIRE_DURABLE_QUEUE=1` makes missing servers, unavailable
JetStream, queue factory errors and scenario failures fail the test.
Success requires both backends to execute all scenarios. Unset, empty
or `0` keeps the existing optional behavior; other nonempty values enable
strict mode. Redis factory errors continue to fail even in optional mode.

## Local runs

```sh
make test_durable_queue
CWIST_TEST_REQUIRE_DURABLE_QUEUE=1 ./test_durable_queue
```

The binary honors the existing `CWIST_REDIS_HOST`, `CWIST_REDIS_PORT` and
`CWIST_NATS_URL` settings. Point it only at test instances.

## Isolated integration gate

Install `redis-server`, `nats-server` and Python 3, then run:

```sh
make test_durable_queue
python3 scripts/ci/durable_queue_gate.py
python3 -m unittest discover -s scripts/ci -p 'test_durable_queue_gate.py'
```

The gate starts its own loopback-only Redis and NATS children. It reserves
random unused ports, including during absent-server cases, and never uses
port 6379 or 4222 by default. Optional `CWIST_DQ_REDIS_PORT`,
`CWIST_DQ_NATS_PORT` and `CWIST_DQ_NOJS_PORT` overrides must be unused.
Occupied ports fail before any test request. `CWIST_DQ_REDIS_BIN` and
`CWIST_DQ_NATS_BIN` select broker executables. Startup waits are bounded
by `CWIST_DQ_READY_SECS` (default 20, maximum 120); each test invocation
has a 60-second timeout.

The seven modes are:

1. Optional, both absent: exit 0 with both skip messages.
2. Strict, both absent: exit 1 with both REQUIRED diagnostics.
3. Strict, Redis live and NATS absent: exit 1; Redis tests must pass.
4. Strict, Redis live and JetStream disabled: exit 1; Redis tests must pass.
5. Strict, Redis and JetStream live: exit 0; both suites must pass.
6. Strict, Redis absent and JetStream live: exit 1; NATS tests must pass.
7. Explicit optional (`0`), both absent: exit 0 with both skip messages.

Negative results require exactly exit 1 plus the specific diagnostics;
crashes and timeouts are not accepted. Temporary data and owned child
processes are cleaned on success, failure, INT and TERM. No `FLUSHALL`,
external broker shutdown, global installation or production queue API
changes are performed by the local gate.

## CI

`.github/workflows/durable-queue.yml` installs checksum-pinned Redis 8.0.3
and NATS 2.12.3 on an Ubuntu runner, builds the real test binary, and runs
both the helper tests and the same live seven-mode gate. The job requests
only `contents: read` and has a 30-minute limit. This tests the existing
queue scenarios, not broker restart durability or crash-recovery semantics.
