/* rando_json.h — minimal DOM-style JSON parser (no dependencies).
 *
 * Purpose-built for the phase-0 logic dumps (~900 KB of machine-generated,
 * ASCII, pretty-printed JSON per seed).  It is a standard recursive-descent
 * parser producing a tree of JsonValue nodes; strings are unescaped and
 * NUL-terminated; numbers keep both the double value and the raw text so
 * int fields can be read exactly.
 */
#ifndef RANDO_JSON_H
#define RANDO_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    JSON_NULL = 0, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING,
    JSON_ARRAY, JSON_OBJECT,
};

typedef struct JsonValue {
    uint8_t type;
    double num;             /* JSON_NUMBER                              */
    char *str;              /* JSON_STRING / object member key (owned)  */
    char raw[24];           /* JSON_NUMBER raw text (NUL-terminated)    */
    struct JsonValue *child;/* first element / first member value       */
    struct JsonValue *next; /* next sibling                             */
    char *key;              /* object member key (owned), NULL otherwise*/
} JsonValue;

/* Parse `text` (NUL-terminated).  Returns NULL on error and fills `err`
 * (if err != NULL) with a short message incl. line:col. */
JsonValue *Json_Parse(const char *text, char *err, size_t errsz);
void Json_Free(JsonValue *v);

/* object member lookup (NULL if absent); `v` must be JSON_OBJECT */
const JsonValue *Json_Get(const JsonValue *v, const char *key);
/* array/object length */
int Json_Size(const JsonValue *v);
/* convenience accessors: return 0/NULL when the type mismatches */
int          Json_AsInt(const JsonValue *v);
int          Json_AsBool(const JsonValue *v);
const char  *Json_AsString(const JsonValue *v);

#ifdef __cplusplus
}
#endif

#endif /* RANDO_JSON_H */
