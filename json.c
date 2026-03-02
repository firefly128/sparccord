/*
 * json.c - Minimal JSON parser for C89
 *
 * Recursive descent parser. No external dependencies.
 * Handles: objects, arrays, strings (with escape sequences),
 * numbers (int and float), booleans, null.
 */

#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ================================================================
 * PARSER STATE
 * ================================================================ */

typedef struct {
    const char *input;
    int         pos;
    int         len;
} parser_t;

static void skip_ws(parser_t *p)
{
    while (p->pos < p->len && isspace((unsigned char)p->input[p->pos]))
        p->pos++;
}

static char peek(parser_t *p)
{
    skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->input[p->pos];
}

static char next(parser_t *p)
{
    skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->input[p->pos++];
}

/* Forward declarations */
static json_value_t *parse_value(parser_t *p);

/* ================================================================
 * ALLOCATORS
 * ================================================================ */

static json_value_t *alloc_value(json_type_t type)
{
    json_value_t *v = (json_value_t *)calloc(1, sizeof(json_value_t));
    if (v) v->type = type;
    return v;
}

/* ================================================================
 * STRING PARSER (handles escape sequences)
 * ================================================================ */

static char *parse_string_raw(parser_t *p)
{
    char *buf = NULL;
    int alloc = 0, len = 0;
    char c;

    /* Expect opening quote (already consumed by caller or skip it) */
    if (p->input[p->pos] == '"') p->pos++;

    while (p->pos < p->len) {
        c = p->input[p->pos++];
        if (c == '"') break;  /* End of string */

        if (c == '\\' && p->pos < p->len) {
            c = p->input[p->pos++];
            switch (c) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u':
                    /* Skip 4 hex digits (simplified — just pass through) */
                    if (p->pos + 4 <= p->len) {
                        /* Simple: output '?' for unicode escapes */
                        p->pos += 4;
                        c = '?';
                    }
                    break;
                default: break;
            }
        }

        /* Grow buffer */
        if (len + 1 >= alloc) {
            alloc = alloc ? alloc * 2 : 64;
            buf = (char *)realloc(buf, alloc);
            if (!buf) return NULL;
        }
        buf[len++] = c;
    }

    if (!buf) {
        buf = (char *)malloc(1);
        if (!buf) return NULL;
    }
    buf[len] = '\0';
    return buf;
}

/* ================================================================
 * VALUE PARSERS
 * ================================================================ */

static json_value_t *parse_string(parser_t *p)
{
    json_value_t *v = alloc_value(JSON_STRING);
    if (!v) return NULL;
    v->u.str_val = parse_string_raw(p);
    if (!v->u.str_val) { free(v); return NULL; }
    return v;
}

static json_value_t *parse_number(parser_t *p)
{
    json_value_t *v;
    const char *start = p->input + p->pos;
    char *end;
    double num;

    num = strtod(start, &end);
    if (end == start) return NULL;
    p->pos += (end - start);

    v = alloc_value(JSON_NUMBER);
    if (!v) return NULL;
    v->u.num_val = num;
    return v;
}

static json_value_t *parse_array(parser_t *p)
{
    json_value_t *v;
    json_value_t **items = NULL;
    int alloc = 0, count = 0;
    json_value_t *elem;

    p->pos++;  /* skip '[' */

    v = alloc_value(JSON_ARRAY);
    if (!v) return NULL;

    if (peek(p) == ']') {
        p->pos++;
        v->u.arr.items = NULL;
        v->u.arr.count = 0;
        return v;
    }

    for (;;) {
        elem = parse_value(p);
        if (!elem) break;

        /* Grow array */
        if (count >= alloc) {
            alloc = alloc ? alloc * 2 : 8;
            items = (json_value_t **)realloc(items, alloc * sizeof(json_value_t *));
            if (!items) { json_free(elem); break; }
        }
        items[count++] = elem;

        if (peek(p) == ',') {
            p->pos++;
        } else {
            break;
        }
    }

    if (peek(p) == ']') p->pos++;

    v->u.arr.items = items;
    v->u.arr.count = count;
    return v;
}

static json_value_t *parse_object(parser_t *p)
{
    json_value_t *v;
    json_pair_t *pairs = NULL;
    int alloc = 0, count = 0;

    p->pos++;  /* skip '{' */

    v = alloc_value(JSON_OBJECT);
    if (!v) return NULL;

    if (peek(p) == '}') {
        p->pos++;
        v->u.obj.pairs = NULL;
        v->u.obj.count = 0;
        return v;
    }

    for (;;) {
        char *key;
        json_value_t *val;

        skip_ws(p);
        if (p->pos >= p->len || p->input[p->pos] != '"') break;

        key = parse_string_raw(p);
        if (!key) break;

        skip_ws(p);
        if (p->pos < p->len && p->input[p->pos] == ':') p->pos++;

        val = parse_value(p);
        if (!val) { free(key); break; }

        /* Grow pairs */
        if (count >= alloc) {
            alloc = alloc ? alloc * 2 : 8;
            pairs = (json_pair_t *)realloc(pairs, alloc * sizeof(json_pair_t));
            if (!pairs) { free(key); json_free(val); break; }
        }
        pairs[count].key = key;
        pairs[count].value = val;
        count++;

        if (peek(p) == ',') {
            p->pos++;
        } else {
            break;
        }
    }

    if (peek(p) == '}') p->pos++;

    v->u.obj.pairs = pairs;
    v->u.obj.count = count;
    return v;
}

static json_value_t *parse_literal(parser_t *p, const char *lit, int litlen,
                                    json_type_t type, int bval)
{
    json_value_t *v;
    if (p->pos + litlen > p->len) return NULL;
    if (strncmp(p->input + p->pos, lit, litlen) != 0) return NULL;
    p->pos += litlen;

    v = alloc_value(type);
    if (!v) return NULL;
    if (type == JSON_BOOL) v->u.bool_val = bval;
    return v;
}

static json_value_t *parse_value(parser_t *p)
{
    char c = peek(p);
    if (c == '\0') return NULL;

    switch (c) {
        case '"': return parse_string(p);
        case '{': return parse_object(p);
        case '[': return parse_array(p);
        case 't': return parse_literal(p, "true", 4, JSON_BOOL, 1);
        case 'f': return parse_literal(p, "false", 5, JSON_BOOL, 0);
        case 'n': return parse_literal(p, "null", 4, JSON_NULL, 0);
        default:
            if (c == '-' || (c >= '0' && c <= '9'))
                return parse_number(p);
            return NULL;
    }
}

/* ================================================================
 * PUBLIC API
 * ================================================================ */

json_value_t *json_parse(const char *input)
{
    parser_t p;
    if (!input) return NULL;
    p.input = input;
    p.pos = 0;
    p.len = strlen(input);
    return parse_value(&p);
}

void json_free(json_value_t *v)
{
    int i;
    if (!v) return;

    switch (v->type) {
        case JSON_STRING:
            free(v->u.str_val);
            break;
        case JSON_ARRAY:
            for (i = 0; i < v->u.arr.count; i++)
                json_free(v->u.arr.items[i]);
            free(v->u.arr.items);
            break;
        case JSON_OBJECT:
            for (i = 0; i < v->u.obj.count; i++) {
                free(v->u.obj.pairs[i].key);
                json_free(v->u.obj.pairs[i].value);
            }
            free(v->u.obj.pairs);
            break;
        default:
            break;
    }
    free(v);
}

json_value_t *json_get(const json_value_t *obj, const char *key)
{
    int i;
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    for (i = 0; i < obj->u.obj.count; i++) {
        if (strcmp(obj->u.obj.pairs[i].key, key) == 0)
            return obj->u.obj.pairs[i].value;
    }
    return NULL;
}

const char *json_get_str(const json_value_t *obj, const char *key)
{
    json_value_t *v = json_get(obj, key);
    if (v && v->type == JSON_STRING) return v->u.str_val;
    return NULL;
}

double json_get_num(const json_value_t *obj, const char *key)
{
    json_value_t *v = json_get(obj, key);
    if (v && v->type == JSON_NUMBER) return v->u.num_val;
    return 0.0;
}

int json_get_bool(const json_value_t *obj, const char *key)
{
    json_value_t *v = json_get(obj, key);
    if (v && v->type == JSON_BOOL) return v->u.bool_val;
    return 0;
}

int json_get_int(const json_value_t *obj, const char *key)
{
    return (int)json_get_num(obj, key);
}

int json_array_len(const json_value_t *arr)
{
    if (!arr || arr->type != JSON_ARRAY) return 0;
    return arr->u.arr.count;
}

json_value_t *json_array_get(const json_value_t *arr, int index)
{
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    if (index < 0 || index >= arr->u.arr.count) return NULL;
    return arr->u.arr.items[index];
}
