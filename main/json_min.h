#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ptr;
    size_t len;
} json_span_t;

static inline json_span_t json_root(const char *buf, size_t len)
{
    json_span_t s = { buf, len };
    return s;
}

bool json_is_null(json_span_t val);
bool json_obj_get(json_span_t obj, const char *key, json_span_t *out_val);
bool json_arr_iter(json_span_t arr, size_t *cursor, json_span_t *out_elem);
bool json_as_str(json_span_t val, char *out, size_t out_cap);
bool json_as_int(json_span_t val, int64_t *out);
bool json_as_bool(json_span_t val, bool *out);

/**
 * Return raw inner content of a JSON string token (excluding surrounding quotes).
 * Useful for base64 fields where no JSON escape sequences appear.
 */
bool json_str_inner(json_span_t val, const char **out_ptr, size_t *out_len);

#ifdef __cplusplus
}
#endif
