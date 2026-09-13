/* rando_json.c — minimal DOM-style JSON parser.  See rando_json.h. */
#include "rando_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 128

typedef struct {
    const char *p;
    const char *start;
    char *err; size_t errsz;
    int failed;
    int depth;
} JParser;

static void jfail(JParser *jp, const char *msg)
{
    if (!jp->failed && jp->err && jp->errsz > 0) {
        int line = 1, col = 1;
        const char *q;
        for (q = jp->start; q < jp->p && *q; q++) {
            if (*q == '\n') { line++; col = 1; } else col++;
        }
        snprintf(jp->err, jp->errsz, "json: %s at line %d col %d", msg, line, col);
    }
    jp->failed = 1;
}

static JsonValue *jnew(uint8_t type)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (!v) return NULL;
    v->type = type;
    return v;
}

void Json_Free(JsonValue *v)
{
    while (v) {
        JsonValue *next = v->next;
        Json_Free(v->child);
        free(v->str);
        free(v->key);
        free(v);
        v = next;
    }
}

static void jskip_ws(JParser *jp)
{
    while (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')
        jp->p++;
}

static JsonValue *jparse_value(JParser *jp);

static char *jparse_string_raw(JParser *jp)
{
    if (*jp->p != '"') { jfail(jp, "expected string"); return NULL; }
    jp->p++;
    /* measure with escape expansion */
    size_t cap = 32, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) { jfail(jp, "oom"); return NULL; }
    while (*jp->p && *jp->p != '"') {
        unsigned int ch;
        if (len + 8 > cap) {
            cap *= 2;
            char *no = (char *)realloc(out, cap);
            if (!no) { free(out); jfail(jp, "oom"); return NULL; }
            out = no;
        }
        if (*jp->p == '\\') {
            jp->p++;
            switch (*jp->p) {
            case '"': ch = '"';  jp->p++; break;
            case '\\':ch = '\\'; jp->p++; break;
            case '/': ch = '/';  jp->p++; break;
            case 'b': ch = '\b'; jp->p++; break;
            case 'f': ch = '\f'; jp->p++; break;
            case 'n': ch = '\n'; jp->p++; break;
            case 'r': ch = '\r'; jp->p++; break;
            case 't': ch = '\t'; jp->p++; break;
            case 'u': {
                int i; unsigned int cp = 0;
                jp->p++;
                for (i = 0; i < 4; i++) {
                    char c = jp->p[i];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                    else { free(out); jfail(jp, "bad \\u escape"); return NULL; }
                }
                jp->p += 4;
                ch = cp;   /* dumps are ASCII; no surrogate handling needed */
                break;
            }
            default:
                free(out); jfail(jp, "bad escape"); return NULL;
            }
        } else {
            ch = (unsigned char)*jp->p;
            jp->p++;
        }
        if (ch < 0x80) {
            out[len++] = (char)ch;
        } else if (ch < 0x800) {
            out[len++] = (char)(0xC0 | (ch >> 6));
            out[len++] = (char)(0x80 | (ch & 0x3F));
        } else {
            out[len++] = (char)(0xE0 | (ch >> 12));
            out[len++] = (char)(0x80 | ((ch >> 6) & 0x3F));
            out[len++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    if (*jp->p != '"') { free(out); jfail(jp, "unterminated string"); return NULL; }
    jp->p++;
    out[len] = '\0';
    return out;
}

static JsonValue *jparse_value_inner(JParser *jp)
{
    if (jp->failed) return NULL;
    jskip_ws(jp);
    switch (*jp->p) {
    case '\0': jfail(jp, "unexpected end"); return NULL;
    case 'n':
        if (strncmp(jp->p, "null", 4) == 0) { jp->p += 4; return jnew(JSON_NULL); }
        jfail(jp, "bad literal"); return NULL;
    case 't':
        if (strncmp(jp->p, "true", 4) == 0) { jp->p += 4; return jnew(JSON_TRUE); }
        jfail(jp, "bad literal"); return NULL;
    case 'f':
        if (strncmp(jp->p, "false", 5) == 0) { jp->p += 5; return jnew(JSON_FALSE); }
        jfail(jp, "bad literal"); return NULL;
    case '"': {
        JsonValue *v = jnew(JSON_STRING);
        if (!v) { jfail(jp, "oom"); return NULL; }
        v->str = jparse_string_raw(jp);
        if (!v->str) { free(v); return NULL; }
        return v;
    }
    case '[': {
        JsonValue *arr = jnew(JSON_ARRAY);
        if (!arr) { jfail(jp, "oom"); return NULL; }
        jp->p++;
        jskip_ws(jp);
        if (*jp->p == ']') { jp->p++; return arr; }
        for (;;) {
            JsonValue *elem = jparse_value(jp);
            if (!elem) { Json_Free(arr); return NULL; }
            /* append */
            if (!arr->child) arr->child = elem;
            else {
                JsonValue *t = arr->child;
                while (t->next) t = t->next;
                t->next = elem;
            }
            jskip_ws(jp);
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == ']') { jp->p++; break; }
            Json_Free(arr); jfail(jp, "expected ',' or ']'"); return NULL;
        }
        return arr;
    }
    case '{': {
        JsonValue *obj = jnew(JSON_OBJECT);
        if (!obj) { jfail(jp, "oom"); return NULL; }
        jp->p++;
        jskip_ws(jp);
        if (*jp->p == '}') { jp->p++; return obj; }
        for (;;) {
            char *key;
            JsonValue *val, *t;
            jskip_ws(jp);
            key = jparse_string_raw(jp);
            if (!key) { Json_Free(obj); return NULL; }
            jskip_ws(jp);
            if (*jp->p != ':') { free(key); Json_Free(obj); jfail(jp, "expected ':'"); return NULL; }
            jp->p++;
            val = jparse_value(jp);
            if (!val) { free(key); Json_Free(obj); return NULL; }
            val->key = key;
            if (!obj->child) obj->child = val;
            else { t = obj->child; while (t->next) t = t->next; t->next = val; }
            jskip_ws(jp);
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == '}') { jp->p++; break; }
            Json_Free(obj); jfail(jp, "expected ',' or '}'"); return NULL;
        }
        return obj;
    }
    default: {   /* number */
        JsonValue *v = jnew(JSON_NUMBER);
        char *endp;
        size_t n;
        if (!v) { jfail(jp, "oom"); return NULL; }
        endp = NULL;
        v->num = strtod(jp->p, &endp);
        if (endp == jp->p) { free(v); jfail(jp, "bad number"); return NULL; }
        n = (size_t)(endp - jp->p);
        if (n >= sizeof(v->raw)) n = sizeof(v->raw) - 1;
        memcpy(v->raw, jp->p, n);
        v->raw[n] = '\0';
        jp->p = endp;
        return v;
    }
    }
}

static JsonValue *jparse_value(JParser *jp)
{
    JsonValue *v;
    if (++jp->depth > JSON_MAX_DEPTH) { jfail(jp, "nesting too deep"); return NULL; }
    v = jparse_value_inner(jp);
    jp->depth--;
    return v;
}

JsonValue *Json_Parse(const char *text, char *err, size_t errsz)
{
    JParser jp;
    JsonValue *v;
    jp.p = text; jp.start = text;
    jp.err = err; jp.errsz = errsz;
    jp.failed = 0; jp.depth = 0;
    if (err && errsz) err[0] = '\0';
    v = jparse_value(&jp);
    if (!v) return NULL;
    jskip_ws(&jp);
    if (*jp.p != '\0') {
        Json_Free(v);
        jfail(&jp, "trailing garbage");
        return NULL;
    }
    return v;
}

const JsonValue *Json_Get(const JsonValue *v, const char *key)
{
    const JsonValue *c;
    if (!v || v->type != JSON_OBJECT) return NULL;
    for (c = v->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0) return c;
    return NULL;
}

int Json_Size(const JsonValue *v)
{
    const JsonValue *c;
    int n = 0;
    if (!v) return 0;
    for (c = v->child; c; c = c->next) n++;
    return n;
}

int Json_AsInt(const JsonValue *v)
{
    if (!v) return 0;
    if (v->type == JSON_TRUE) return 1;
    if (v->type == JSON_FALSE || v->type == JSON_NULL) return 0;
    if (v->type == JSON_NUMBER) return (int)(v->num + (v->num < 0 ? -0.5 : 0.5));
    return 0;
}

int Json_AsBool(const JsonValue *v)
{
    if (!v) return 0;
    return v->type == JSON_TRUE;
}

const char *Json_AsString(const JsonValue *v)
{
    if (!v || v->type != JSON_STRING) return NULL;
    return v->str;
}
