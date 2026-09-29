/* spore_json: in-place JSON reader (RFC 8259) and writer helpers.
 * SPDX-License-Identifier: MIT
 *
 * The reader decodes strings inside the caller's buffer and NUL-terminates
 * them there, so nodes point into the text and need no allocation of their
 * own. The text must stay alive and unmodified while the document is used.
 */
#ifndef SPORE_JSON_H
#define SPORE_JSON_H

#include "spore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPORE_JNULL,
    SPORE_JFALSE,
    SPORE_JTRUE,
    SPORE_JNUM,
    SPORE_JSTR,
    SPORE_JARR,
    SPORE_JOBJ
} spore_jtype;

/* Objects list their members as alternating key and value children. */
typedef struct {
    spore_jtype type;
    const char *str;  /* JSTR: decoded, NUL-terminated; JNUM: raw text */
    size_t len;       /* JSTR/JNUM: bytes; JARR: elements; JOBJ: members */
    uint32_t child;   /* index of first child, 0 if none */
    uint32_t next;    /* index of next sibling, 0 if none */
} spore_jnode;

typedef struct {
    spore_jnode *nodes; /* nodes[0] is the root */
    size_t n, cap;
} spore_json;

/* Parse text[0..len). Returns 0, or -1 on malformed input, nesting deeper
 * than 64, or allocation failure. Free with spore_json_free() either way. */
int spore_json_parse(spore_json *doc, char *text, size_t len);
void spore_json_free(spore_json *doc);

const spore_jnode *spore_json_root(const spore_json *doc);
/* Object member by key, or NULL. `obj` may be NULL or a non-object. */
const spore_jnode *spore_json_get(const spore_json *doc,
                                  const spore_jnode *obj, const char *key);
/* First child and next sibling, or NULL. */
const spore_jnode *spore_json_child(const spore_json *doc,
                                    const spore_jnode *node);
const spore_jnode *spore_json_next(const spore_json *doc,
                                   const spore_jnode *node);
/* Typed accessors: return 0 and store the value, or -1 on type mismatch. */
int spore_json_double(const spore_jnode *node, double *out);
int spore_json_int(const spore_jnode *node, long *out);  /* integral only */
int spore_json_bool(const spore_jnode *node, int *out);

/* Writer: append `s` as a quoted JSON string. Invalid UTF-8 becomes
 * U+FFFD, so the output is always valid JSON. */
void spore_json_str(spore_buf *b, const char *s, size_t len);
/* Append a number; NaN and infinities become null. */
void spore_json_num(spore_buf *b, double v);

#ifdef __cplusplus
}
#endif
#endif
