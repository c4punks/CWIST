# db-crypt examples

Encrypt and decrypt a database byte buffer with the CWIST database
encryption API declared in `include/cwist/security/db_crypt/db_crypt.h`.
Typical use: seal the bytes produced by `sqlite3_serialize()` before
writing them to disk or shipping them over the network, and open them
again after reading.

| Step | Shows |
|------|-------|
| [`step-1-seal-open`](step-1-seal-open/main.c) | sealing plaintext with `cwist_db_crypt_seal`, recovering it with `cwist_db_crypt_open`, and rejecting a wrong key |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build the example (it ships its own Makefile):

```sh
make -C example/db-crypt/step-1-seal-open
```

`make examples-check` also builds it.

## Run

```sh
./example/db-crypt/step-1-seal-open/step-1-seal-open
```

The program:

1. Fills a 32-byte KEK (Key Encryption Key) with a hardcoded demo
   pattern. In production this key must come from a strong source, e.g.
   a password derived with PBKDF2 or Argon2 — never a `memset`.
2. Takes a mock database payload (a string starting with the SQLite
   magic `SQLite format 3`).
3. Seals it: `cwist_db_crypt_seal` returns a newly allocated blob that
   starts with a `CWIST_DB_CRYPT_HDR_LEN`-byte header followed by the
   ciphertext.
4. Opens the blob again and byte-compares the recovered plaintext with
   the original.
5. Tries to open the same blob with a different key, which must fail.

Example output:

```
=== DB Crypt: Seal & Open ===

Plaintext  (16 bytes): "SQLite format 3"

[Seal]
Sealed blob: 141 bytes (header=109 + ciphertext)
First 16 bytes: 4357444201e3037e3fc22a78cba062d0 ...

[Open]
Recovered  (16 bytes): "SQLite format 3"
Match: YES

[Wrong key]
Decrypt with wrong key: failed (correct)

=== Done ===
```

The sealed bytes are random (the seal uses a random nonce), so the hex
dump differs on every run. The wrong-key open must always print
`failed (correct)`; if it prints `succeeded (unexpected!)` the
integrity check is broken.

## Clean

```sh
make -C example/db-crypt/step-1-seal-open clean
```
