/* Internal declarations shared between spore modules and unit tests.
 * SPDX-License-Identifier: MIT */
#ifndef SPORE_INTERNAL_H
#define SPORE_INTERNAL_H

#include "spore.h"

/* Parse a request head from buf[0..len). `scan` carries the search offset
 * for the blank line across calls; start it at 0. Returns the head length
 * including the blank line, 0 if more bytes are needed, or -status. */
long spore__parse_head(const char *buf, size_t len, size_t max_header,
                       size_t *scan, spore_req *req);

int spore__ieq(spore_str s, const char *cstr);          /* ASCII case-fold */
int spore__has_token(spore_str list, const char *tok);  /* "a, b, c" list */
int spore__host_allowed(spore_str host);
int spore__origin_allowed(spore_str origin, const char *const *extra);

#endif
