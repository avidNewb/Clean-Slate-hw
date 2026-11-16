#include "json_min.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static size_t skip_ws(const char *p, size_t pos, size_t len)
{
    while (pos < len && (p[pos] == ' ' || p[pos] == '\t' || p[pos] == '\r' || p[pos] == '\n')) {
        pos++;
    }
    return pos;
}

static size_t skip_string(const char *p, size_t pos, size_t len)
{
    if (pos >= len || p[pos] != '"') {
        return pos;
    }
    pos++;
    while (pos < len) {
        if (p[pos] == '\\') {
            pos += 2;
            continue;
        }
        if (p[pos] == '"') {
            return pos + 1;
        }
        pos++;
    }
    return len;
}

static size_t skip_value(const char *p, size_t pos, size_t len)
{
    pos = skip_ws(p, pos, len);
    if (pos >= len) {
        return len;
    }
    char c = p[pos];
    if (c == '"') {
        return skip_string(p, pos, len);
    }
    if (c == '{' || c == '[') {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (pos < len) {
            if (p[pos] == '"') {
                pos = skip_string(p, pos, len);
                continue;
            }
            if (p[pos] == open) {
                depth++;
            } else if (p[pos] == close) {
                depth--;
                if (depth == 0) {
                    return pos + 1;
                }
            }
            pos++;
        }
        return len;
    }
    while (pos < len) {
        char ch = p[pos];
        if (ch == ',' || ch == '}' || ch == ']' ||
            ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            break;
        }
        pos++;
    }
    return pos;
}

bool json_is_null(json_span_t val)
{
    size_t pos = skip_ws(val.ptr, 0, val.len);
    return (val.len - pos >= 4) && (memcmp(val.ptr + pos, "null", 4) == 0);
}

bool json_str_inner(json_span_t val, const char **out_ptr, size_t *out_len)
{
    size_t pos = skip_ws(val.ptr, 0, val.len);
    if (pos >= val.len || val.ptr[pos] != '"') {
        return false;
    }
    size_t end = skip_string(val.ptr, pos, val.len);
    if (end <= pos + 1 || val.ptr[end - 1] != '"') {
        return false;
    }
    *out_ptr = val.ptr + pos + 1;
    *out_len = end - pos - 2;
    return true;
}

bool json_as_str(json_span_t val, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return false;
    }
    const char *raw = NULL;
    size_t raw_len = 0;
    if (!json_str_inner(val, &raw, &raw_len)) {
        return false;
    }
    size_t w = 0;
    for (size_t i = 0; i < raw_len && w + 1 < out_cap; i++) {
        if (raw[i] == '\\' && i + 1 < raw_len) {
            char esc = raw[++i];
            switch (esc) {
                case '"':  out[w++] = '"'; break;
                case '\\': out[w++] = '\\'; break;
                case '/':  out[w++] = '/'; break;
                case 'n':  out[w++] = '\n'; break;
                case 'r':  out[w++] = '\r'; break;
                case 't':  out[w++] = '\t'; break;
                default:   out[w++] = esc; break;
            }
        } else {
            out[w++] = raw[i];
        }
    }
    out[w] = '\0';
    return true;
}

bool json_as_int(json_span_t val, int64_t *out)
{
    size_t pos = skip_ws(val.ptr, 0, val.len);
    if (pos >= val.len) {
        return false;
    }
    char c = val.ptr[pos];
    if (c != '-' && !isdigit((unsigned char)c)) {
        return false;
    }
    char tmp[32];
    size_t copy_len = val.len - pos;
    if (copy_len >= sizeof(tmp)) {
        copy_len = sizeof(tmp) - 1;
    }
    memcpy(tmp, val.ptr + pos, copy_len);
    tmp[copy_len] = '\0';
    char *endptr = NULL;
    long long v = strtoll(tmp, &endptr, 10);
    if (endptr == tmp) {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

bool json_as_bool(json_span_t val, bool *out)
{
    size_t pos = skip_ws(val.ptr, 0, val.len);
    if (val.len - pos >= 4 && memcmp(val.ptr + pos, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (val.len - pos >= 5 && memcmp(val.ptr + pos, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool json_obj_get(json_span_t obj, const char *key, json_span_t *out_val)
{
    if (!obj.ptr || !key) {
        return false;
    }
    size_t pos = skip_ws(obj.ptr, 0, obj.len);
    if (pos >= obj.len || obj.ptr[pos] != '{') {
        return false;
    }
    pos++;
    size_t key_len = strlen(key);

    while (pos < obj.len) {
        pos = skip_ws(obj.ptr, pos, obj.len);
        if (pos >= obj.len || obj.ptr[pos] == '}') {
            return false;
        }
        if (obj.ptr[pos] == ',') {
            pos++;
            continue;
        }
        if (obj.ptr[pos] != '"') {
            return false;
        }
        size_t key_end = skip_string(obj.ptr, pos, obj.len);
        json_span_t kspan = { obj.ptr + pos, key_end - pos };
        pos = skip_ws(obj.ptr, key_end, obj.len);
        if (pos >= obj.len || obj.ptr[pos] != ':') {
            return false;
        }
        pos = skip_ws(obj.ptr, pos + 1, obj.len);
        size_t val_end = skip_value(obj.ptr, pos, obj.len);

        const char *kinner = NULL;
        size_t kinner_len = 0;
        if (json_str_inner(kspan, &kinner, &kinner_len) &&
            kinner_len == key_len &&
            memcmp(kinner, key, key_len) == 0) {
            out_val->ptr = obj.ptr + pos;
            out_val->len = val_end - pos;
            return true;
        }
        pos = val_end;
    }
    return false;
}

bool json_arr_iter(json_span_t arr, size_t *cursor, json_span_t *out_elem)
{
    if (!arr.ptr || !cursor || !out_elem) {
        return false;
    }
    size_t pos = *cursor;
    if (pos == 0) {
        pos = skip_ws(arr.ptr, 0, arr.len);
        if (pos >= arr.len || arr.ptr[pos] != '[') {
            return false;
        }
        pos++;
    }
    pos = skip_ws(arr.ptr, pos, arr.len);
    if (pos < arr.len && arr.ptr[pos] == ',') {
        pos = skip_ws(arr.ptr, pos + 1, arr.len);
    }
    if (pos >= arr.len || arr.ptr[pos] == ']') {
        *cursor = arr.len;
        return false;
    }
    size_t elem_end = skip_value(arr.ptr, pos, arr.len);
    out_elem->ptr = arr.ptr + pos;
    out_elem->len = elem_end - pos;
    *cursor = elem_end;
    return true;
}
