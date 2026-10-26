# HTTP/3 connection migration

## Result for issue #407

The HTTP/3 server uses the connection and stream contexts that lsquic provides.
It does not use a client address as the key for request state.
`cwist_http3_server_loop()` passes each packet's peer address to lsquic.
The output callback sends each packet to the address that lsquic selects.
lsquic performs path validation and selects the connection by its Connection ID.

The address-change test passes with the existing HTTP/3 implementation on `dev`.
No change to the transport implementation is necessary for these test cases.
The missing part was an end-to-end test and a CI check.
The test does not prove that all mobile network changes are supported.

## Run the test

Install Python 3.11 or later and `aioquic==1.4.0` in a virtual environment.
Then run:

```sh
make -j4 http3-migration-e2e PYTHON=/path/to/venv/bin/python
```

The fixture uses an ephemeral certificate and a loopback UDP listener.
It does not need a certificate file, an external service, or root access.
Certificate verification is disabled only in the test client.

The default test uses `127.0.0.1` and `127.0.0.2` as source addresses.
Linux permits both addresses without a separate interface alias.
On macOS, select a second IPv4 address that is already configured on this host:

```sh
make http3-migration-e2e PYTHON=/path/to/venv/bin/python \
  HTTP3_MIGRATION_ARGS="--migration-host <local-IPv4-address>"
```

Do not use an address from another host.
The test fails if the second address cannot be bound. It does not skip the test.
With `CWIST_HTTP3=0`, Make skips this target because HTTP/3 is not available.

## Test coverage

Two clients connect through separate UDP proxies to one CWIST HTTP/3 server.
Each client first completes a request to identify its server-side connection.
Each client then performs 200 address changes:

1. Open three POST streams. Send a different binary prefix on each stream.
2. Keep each request open without FIN. Wait for a QUIC PING acknowledgment.
3. Change the proxy's source socket. Close the old socket.
4. Alternate port-only changes with IP address changes.
5. On the first IP change, wait for a spare server Connection ID.
   Change the Connection ID and check that it actually changed.
6. Wait for a PING acknowledgment through the new socket.
7. Send the remaining binary data and FIN on each original stream.
8. Check the HTTP status, the complete response body, and the connection identity.

The default run checks 400 address changes and 1,200 in-flight requests.
The two initial requests are additional checks.
The two clients must keep different server-side connection identities.
Each client must keep its original identity through all address changes.
The server must stop with exit code zero.

The test uses the server's Connection ID only through lsquic.
The fixture's response header identifies the `lsquic_conn_t` object, not a wire CID.
A wire CID can change without a change to that object.

The test waits for a spare Connection ID because lsquic limits CID issue rate.
Calling aioquic's `change_connection_id()` without a spare CID has no effect.
The script checks private aioquic CID fields for this reason. CI pins aioquic 1.4.0.

Use `HTTP3_MIGRATION_ARGS="--rounds N"` to change the number of test rounds.
The script limits the complete run to 120 seconds and each PING or response to 10 seconds.
This is a bounded migration-churn test, not a long-duration production soak test.

## Limits

The test covers IPv4 NAT rebinding and an IPv4 source-address change.
It does not cover IPv4-to-IPv6 changes, packet loss, a server address change,
a load balancer, or a real Wi-Fi-to-cellular transition.
`Interop / HTTP/3 connection migration (aioquic)` runs the default test on Linux CI.
