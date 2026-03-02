/*
 * json.h - Minimal JSON parser for C89
 *
 * Recursive descent parser. Handles objects, arrays, strings,
 * numbers, booleans, null. Returns a tree of json_value structs.
 * No external dependencies.
 */

#ifndef SPARCCORD_JSON_H
#define SPARCCORD_JSON_H

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type_t;

typedef struct json_value json_value_t;

/* Key-value pair for objects */
typedef struct json_pair {
    char          *key;
    json_value_t  *value;
} json_pair_t;

struct json_value {
    json_type_t type;
    union {
        int          bool_val;     /* JSON_BOOL: 0 or 1 */
        double       num_val;      /* JSON_NUMBER */
        char        *str_val;      /* JSON_STRING (malloc'd) */
        struct {                   /* JSON_ARRAY */
            json_value_t **items;
            int            count;
        } arr;
        struct {                   /* JSON_OBJECT */
            json_pair_t   *pairs;
            int            count;
        } obj;
    } u;
};

/*
 * Parse a JSON string. Returns root json_value_t, or NULL on error.
 * Caller must free with json_free().
 */
json_value_t *json_parse(const char *input);

/*
 * Free a json_value_t tree (recursively).
 */
void json_free(json_value_t *v);

/*
 * Lookup helpers for objects.
 * Return NULL if key not found or wrong type.
 */
json_value_t *json_get(const json_value_t *obj, const char *key);
const char   *json_get_str(const json_value_t *obj, const char *key);
double        json_get_num(const json_value_t *obj, const char *key);
int           json_get_bool(const json_value_t *obj, const char *key);
int           json_get_int(const json_value_t *obj, const char *key);

/*
 * Array helpers.
 */
int            json_array_len(const json_value_t *arr);
json_value_t  *json_array_get(const json_value_t *arr, int index);

#endif /* SPARCCORD_JSON_H */
