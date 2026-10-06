# SipHash example

A small program that hashes a string with SipHash-2-4, the keyed 64-bit hash
declared in `include/cwist/core/siphash/siphash.h`.

| Step | Shows |
|------|-------|
| [`simple-hashing`](simple-hashing/main.c) | generating a random 128-bit key with `cwist_generate_hash_seed`, then hashing a string with `siphash24` |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build the example:

```sh
make -C example/siphash/simple-hashing
```

`make examples-check` also builds it.

## Run

```sh
./example/siphash/simple-hashing/simple-hashing
```

The program does four things:

1. Calls `cwist_generate_hash_seed(key)` to fill a 16-byte key with random
   bytes, and prints the key as 32 hex digits.
2. Takes the string `The quick brown fox jumps over the lazy dog`.
3. Calls `siphash24(data, strlen(data), key)`, which returns a `uint64_t`.
4. Prints the hash in decimal and in hex.

Example output:

```
=== SipHash Simple Example ===
Generating random key...
Key: 1981c7e4cc4df7a309fcf8669c9be628
Data: "The quick brown fox jumps over the lazy dog"
SipHash-2-4: 10111689906133436649 (0x8c53f06545fc88e9)
```

The key is random, so the key and the hash differ on every run. The hash is a
function of the key and the data: calling `siphash24` again with the same key
and the same bytes returns the same value, and changing one bit of the key
changes the result.

## The two functions

```c
void cwist_generate_hash_seed(uint8_t key[16]);
uint64_t siphash24(const void *src, size_t len, const uint8_t key[16]);
```

* `cwist_generate_hash_seed` writes 16 random bytes into `key`.
* `siphash24` hashes `len` bytes starting at `src` with the 16-byte `key` and
  returns the 64-bit result. `src` is not required to be a C string, so the
  length is passed explicitly.
