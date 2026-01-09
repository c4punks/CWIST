#include <cwistaw/smartstring.h>
#include <cwistaw/err/cwist_err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

cwist_error_t smartstring_ltrim(smartstring *str) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SMARTSTRING_NULL_STRING;
    if (!str || !str->data) return err;

    size_t len = strlen(str->data);
    size_t start = 0;
    while (start < len && isspace((unsigned char)str->data[start])) {
        start++;
    }

    if (start > 0) {
        memmove(str->data, str->data + start, len - start + 1);
        str->size -= start; 
    }

    err.error.err_i8 = ERR_SMARTSTRING_OKAY;
    return err;                              
}

cwist_error_t smartstring_rtrim(smartstring *str) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SMARTSTRING_NULL_STRING;
    if (!str || !str->data) return err; 

    size_t len = strlen(str->data);
    
    if (len == 0) {
      err.error.err_i8 = ERR_SMARTSTRING_ZERO_LENGTH;
      return err;
    }

    size_t end = len - 1;
    while (end < len && 
        isspace((unsigned char)str->data[end])) { 
        end--;
    }
    
    str->data[end + 1] = '\0';
    
    err.error.err_i8 = ERR_SMARTSTRING_OKAY;
    return err;
}

cwist_error_t smartstring_trim(smartstring *str) {
    cwist_error_t err = smartstring_rtrim(str);
    if(err.error.err_i8 != ERR_SMARTSTRING_OKAY) return err;
    return smartstring_ltrim(str);
}

cwist_error_t smartstring_change_size(smartstring *str, size_t new_size, bool blow_data) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);

    if (!str) {
      err.error.err_i8 = ERR_SMARTSTRING_NULL_STRING;
      return err;
    }

    if (str->is_fixed) {
      err.error.err_i8 = ERR_SMARTSTRING_CONSTANT;
      return err;
    }

    size_t current_len = str->data ? strlen(str->data) : 0;

    if (new_size < current_len && !blow_data) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "err", "New size is smaller than current data length and blow_data is false.");
        return err;
    }

    char *new_data = (char *)realloc(str->data, new_size + 1); 
    if (!new_data && new_size > 0) {
        err.error.err_i8 = ERR_SMARTSTRING_RESIZE_TOO_LARGE;
        return err;                                                
    }

    str->data = new_data;
    str->size = new_size;
    
    // Ensure null termination if growing or if it was just allocated
    if (new_size >= current_len) {
        if (current_len == 0) {
             str->data[0] = '\0';
        }
    } else {
        str->data[new_size] = '\0';
    }

   err.error.err_i8 = ERR_SMARTSTRING_OKAY;
   return err;
}

cwist_error_t smartstring_assign(smartstring *str, char *data) {
    if (!str) {
      cwist_error_t err = make_error(CWIST_ERR_INT8);
      err.error.err_i8 = ERR_SMARTSTRING_NULL_STRING;
      return err;
    }
    
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();

    size_t data_len = data ? strlen(data) : 0;

    if (str->is_fixed) {
        if (data_len > str->size) {
          cJSON_AddStringToObject(err.error.err_json, "err", "string's assigned size is smaller than given data");
          return err;
        }
        if (str->data) strcpy(str->data, data ? data : "");
    } else {
        char *new_data = (char *)realloc(str->data, data_len + 1);
        if (!new_data) {
          cJSON_AddStringToObject(err.error.err_json, "err", "cannot assign string: memory is full");
          return err;
        }
        str->data = new_data;
        str->size = data_len; 
        if (data) strcpy(str->data, data);
        else str->data[0] = '\0';
    }

    cJSON_Delete(err.error.err_json); 
    err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SMARTSTRING_OKAY;

    return err;
}

cwist_error_t smartstring_append(smartstring *str, const char *data) {
    if (!str) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SMARTSTRING_NULL_STRING;
        return err;
    }
    if (!data) {
        // Appending nothing is success
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SMARTSTRING_OKAY;
        return err;
    }

    size_t current_len = str->data ? strlen(str->data) : 0;
    size_t append_len = strlen(data);
    size_t new_size = current_len + append_len;

    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();

    if (str->is_fixed) {
        if (new_size > str->size) {
            cJSON_AddStringToObject(err.error.err_json, "err", "Cannot append: would exceed fixed size");
            return err;
        }
    } else {
        char *new_data = (char *)realloc(str->data, new_size + 1);
        if (!new_data) {
             cJSON_AddStringToObject(err.error.err_json, "err", "Cannot append: memory full");
             return err;
        }
        str->data = new_data;
        str->size = new_size;
    }

    // Append logic
    if (str->data) {
        // If it was empty/null before, we need to make sure we don't strcat to garbage
        if (current_len == 0) str->data[0] = '\0';
        strcat(str->data, data);
    }

    cJSON_Delete(err.error.err_json);
    err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SMARTSTRING_OKAY;
    return err;
}

cwist_error_t smartstring_seek(smartstring *str, char *substr, int location) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);
    if (!str || !str->data || !substr) {
      err.error.err_i8 = ERR_SMARTSTRING_NULL_STRING;
      return err;
    }
    
    size_t len = strlen(str->data);
    if (location < 0 || (size_t)location >= len) {
      err.error.err_i8 = ERR_SMARTSTRING_OUTOFBOUND;
      return err;
    }

    strcpy(substr, str->data + location);
    
    err.error.err_i8 = ERR_SMARTSTRING_OKAY;
    return err;
}

cwist_error_t smartstring_copy(smartstring *origin, char *destination) {

    cwist_error_t err = make_error(CWIST_ERR_INT8);
    if (!origin || !origin->data || !destination) {
      err.error.err_i8 = ERR_SMARTSTRING_NULL_STRING;
      return err;
    }
    
    strcpy(destination, origin->data);
    
    err.error.err_i8 = ERR_SMARTSTRING_OKAY;
    return err;
}

smartstring *smartstring_create(void) {
    smartstring *str = (smartstring *)malloc(sizeof(smartstring));
    if (!str) return NULL;

    memset(str, 0, sizeof(smartstring));
    str->is_fixed = false;
    str->size = 0;
    str->data = NULL; // Initially empty

    return str;
}

void smartstring_destroy(smartstring *str) {
    if (str) {
        if (str->data) free(str->data);
        free(str);
    }
}

int smartstring_compare(smartstring *str, const char *compare_to) {
    if (!str || !str->data) {
        if (!compare_to) return 0; // Both NULL-ish (empty treated as NULL for comparison?)
        return -1; 
    }
    if (!compare_to) return 1; 
    
    return strcmp(str->data, compare_to);
}

smartstring *smartstring_substr(smartstring *str, int start, int length) {
    if (!str || !str->data || start < 0 || length < 0) return NULL;
    
    size_t current_len = strlen(str->data);
    if ((size_t)start >= current_len) return NULL;
    
    // Adjust length if it goes beyond end
    if ((size_t)(start + length) > current_len) {
        length = current_len - start;
    }
    
    smartstring *sub = smartstring_create();
    if (!sub) return NULL;
    
    sub->data = (char *)malloc(length + 1);
    if (!sub->data) {
        smartstring_destroy(sub);
        return NULL;
    }
    sub->size = length;
    
    memcpy(sub->data, str->data + start, length);
    sub->data[length] = '\0';
    
    return sub;
}
