# SString API

*Header:* `<cwist/sstring.h>`

Safe, dynamic string manipulation library.

### `cwist_sstring_create`
```c
cwist_sstring *cwist_sstring_create(void);
```
Creates a dynamic string.

### `cwist_sstring_append`
```c
cwist_error_t cwist_sstring_append(cwist_sstring *str, const char *suffix);
```
Safe concatenation. Returns `cwist_error_t`.

### `cwist_sstring_destroy`
```c
void cwist_sstring_destroy(cwist_sstring *str);
```
Frees the string memory.
