#ifndef __CWIST_SMARTSTRING_H__
#define __CWIST_SMARTSTRING_H__

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <cwistaw/err/cwist_err.h>

typedef struct smartstring {
  char   *data;  // please access this data if raw handling is necessary
  bool   is_fixed;
  size_t size;
  size_t (*get_size)(void);
  int     (*compare )(void); // should mimic strcmp, internally use strncmp
  bool    (*copy    )(smartstring *str, smartstring from);
  bool    (*append  )(smartstring); 
                                   // returns 1 on success, returns 0 on failure
                                   // should be used in this form:
                                   // smartstring str1;
                                   // smartsring str2;
                                   // smartstring_init(&str);
                                   // smartstring_init(&str2);
                                   // bool err = str->copy(&str1, str2);
                                   // bool err = rstr->append(&str2, str1);
                                   // ...
} smartstring;

smartstring *smartstring_create(void);
void smartstring_destroy(smartstring *str);

// String manipulation API
cwist_error_r smartstring_init (smartstring *str);
cwist_error_t smartstring_ltrim(smartstring *str);
cwist_error_t smartstring_rtrim(smartstring *str);
cwist_error_t smartstring_trim(smartstring *str);
cwist_error_t smartstring_change_size(smartstring *str, size_t size, bool blow_data);
cwist_error_t smartstring_assign(smartstring *str, char *data);
cwist_error_t smartstring_append(smartstring *str, const char *data); // New function
cwist_error_t smartstring_seek(smartstring *str, char *substr, int location);
cwist_error_t smartstring_copy(smartstring *origin, char *destination);
int smartstring_compare(smartstring *str, const char *compare_to);
smartstring *smartstring_substr(smartstring *str, int start, int length);

enum smartstring_error_t {
  ERR_SMARTSTRING_OKAY,
  ERR_SMARTSTRING_ZERO_LENGTH,
  ERR_SMARTSTRING_NULL_STRING,
  ERR_SMARTSTRING_CONSTANT,
  ERR_SMARTSTRING_RESIZE_TOO_SMALL,
  ERR_SMARTSTRING_RESIZE_TOO_LARGE,
  ERR_SMARTSTRING_OUTOFBOUND,
};

#endif
