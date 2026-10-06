# SString examples

Three small programs that use `cwist_sstring`, the dynamic string type declared
in `include/cwist/core/sstring/sstring.h`. See also the
[SString API page](../../docs/api/sstring.md).

| Step | Shows |
|------|-------|
| [`step-1-getting-started`](step-1-getting-started/main.c) | create, assign, trim, seek, and change the buffer size |
| [`step-2-compare-copy`](step-2-compare-copy/main.c) | compare two sstrings or an sstring and a C string, and copy to another sstring or to a char buffer |
| [`step-3-substr-append`](step-3-substr-append/main.c) | append, append with HTML escaping, substring, seek, and the trim variants |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build a step and run the program it produces:

```sh
make -C example/sstring/step-1-getting-started
./example/sstring/step-1-getting-started/getting-started

make -C example/sstring/step-2-compare-copy
./example/sstring/step-2-compare-copy/step-2-compare-copy

make -C example/sstring/step-3-substr-append
./example/sstring/step-3-substr-append/step-3-substr-append
```

`make examples-check` builds all three steps. Each step prints its results and
exits with status 0.

## Step 1: getting started

The program assigns `"   Hello, SString!   "`, trims it, copies the text from
offset 7 into a char buffer with `cwist_sstring_seek`, then shrinks the string
to 5 bytes with `cwist_sstring_change_size`. Output:

```
=== SString Getting Started ===

[Assign]
Original text: '   Hello, SString!   '

[Trim]
Trimmed text:  'Hello, SString!'

[Seek]
Seek(7):       'SString!'

[Change Size]
Current size: 15
Resized to 5:  'Hello'

=== Done ===
```

* `cwist_sstring_seek(str, buf, location)` copies the text from `location` to the
  end of the string into `buf`. `buf` must be large enough for the rest of the
  string. A `location` that is negative or at or past the end of the string
  returns `ERR_SSTRING_OUTOFBOUND`.
* `cwist_sstring_change_size(str, size, blow_data)` changes the buffer to `size`
  bytes. Making the string shorter than its current text only succeeds when
  `blow_data` is `true`; with `false` it returns an error and leaves the string
  unchanged. A fixed-size string returns `ERR_SSTRING_CONSTANT`.
* The example checks results with `err.errtype == CWIST_ERR_INT8` and
  `err.error.err_i8 == ERR_SSTRING_OKAY`.

## Step 2: compare and copy

The program creates three strings holding `Hello, World!`, `Hello, World!` and
`Hello, CWIST!`. The comparison part of the output is:

```
[Compare sstring vs sstring]
a == b? yes  (cmp=0)
a == c? no  (cmp=20)

[Compare sstring vs C-string]
a == "Hello, World!"? yes
```

* `cwist_sstring_compare_sstring(left, right)` and
  `cwist_sstring_compare(str, c_string)` return what `strcmp` returns: `0` when
  the texts are equal and a non-zero value otherwise. Here `cmp=20` is the
  difference between `W` and `C`.
* `cwist_sstring_copy_sstring(dest, src)` replaces the text of `dest` with the
  text of `src`. After the call, `c` holds `Hello, World!`.
* `cwist_sstring_copy(src, buf)` writes the text of `src` into a char buffer.
  `buf` must be large enough for the whole string.

The program prints the copied values as `c after copy from a: 'Hello, World!'`
and `buf after copy from a: 'Hello, World!'`.

## Step 3: substring and append

```
[Append]
After appends: 'Hello, CWIST!'

[Append escaped]
Escaped:       'Safe: &lt;script&gt;alert(&#39;xss&#39;)&lt;/script&gt;'

[Substr]
substr(7,5):   'CWIST'

[Seek]
seek(7):       'CWIST!'

[Trim]
ltrim:         'left-padded'
rtrim:         'right-padded'
```

* `cwist_sstring_append(str, text)` adds text to the end of the string. The
  example builds `Hello, CWIST!` from four appends.
* `cwist_sstring_append_escaped(str, text)` appends `text` with HTML escaping,
  so `<`, `>` and `'` in the example become `&lt;`, `&gt;` and `&#39;`. Use it
  when text from outside is placed into HTML.
* `cwist_sstring_substr(str, start, length)` returns a new sstring holding
  `length` bytes from `start`, which the caller releases with
  `cwist_sstring_destroy`. If `length` runs past the end it is shortened, and if
  `start` is negative or at or past the end the call returns `NULL`.
* `cwist_sstring_ltrim` and `cwist_sstring_rtrim` remove whitespace from one
  end; `cwist_sstring_trim` (step 1) removes it from both.
