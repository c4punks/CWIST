# mem examples

Two steps that introduce the CWIST memory API declared in
`include/cwist/core/mem/alloc.h` and `include/cwist/core/mem/gc.h`.
Both are console programs — they print to stdout and exit.

| Step | Shows |
|------|-------|
| [`step-1-alloc`](step-1-alloc/main.c) | `cwist_alloc`, `cwist_realloc`, `cwist_strdup`, `cwist_strndup`, and `cwist_alloc_array` |
| [`step-2-gc`](step-2-gc/main.c) | epoch-based garbage collection with `cwist_gc`, `cwist_reg_ptr`, `cwist_reg_ptr_sized`, `cwist_gc_rotate`, and `cwist_gc_shutdown` |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build each step (both ship their own Makefile):

```sh
make -C example/mem/step-1-alloc
make -C example/mem/step-2-gc
```

`make examples-check` also builds both.

## Run

### Step 1: allocation

```sh
./example/mem/step-1-alloc/step-1-alloc
```

Walks through the five allocation helpers. `cwist_alloc` returns zeroed
memory, `cwist_realloc` grows an existing block in place, `cwist_strdup`
/ `cwist_strndup` copy strings, and `cwist_alloc_array(count, size)`
allocates a zeroed array. Everything is released with `cwist_free`:

```
=== Memory Allocation Tutorial ===

[cwist_alloc]
buf: 'Hello from cwist_alloc!'

[cwist_realloc]
buf after realloc: 'Hello from cwist_alloc! (resized)'

[cwist_strdup / cwist_strndup]
strdup:  'Duplicate me!'
strndup: 'Duplicate'

[cwist_alloc_array]
arr: 10 20 30 40 50 60 70 80

=== Done ===
```

### Step 2: epoch GC

```sh
./example/mem/step-2-gc/step-2-gc
```

Initialises a GC context with manual rotation
(`cwist_gc(&gc, /*manual_rotation=*/true)`), registers three
`cwist_alloc`'d buffers with `cwist_reg_ptr` / `cwist_reg_ptr_sized`,
and reclaims retired allocations by calling `cwist_gc_rotate` between
allocations. `cwist_gc_shutdown` tears the context down at the end:

```
=== Epoch GC Tutorial ===
GC initialized (manual rotation mode)

[Register pointers]
Registered p1 (256 B) and p2 (512 B)

[Manual rotation]
Epoch rotated; retired allocations reclaimed
Registered p3 (1024 B)
Second rotation complete

[Shutdown]
GC shut down

=== Done ===
```

## Clean

```sh
make -C example/mem/step-1-alloc clean
make -C example/mem/step-2-gc clean
```
