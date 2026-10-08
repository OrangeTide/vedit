/* test.h : unit-test API for test programs
 * SPDX-License-Identifier: 0BSD OR CC0-1.0
 * Copyright © 2026 Jon Mayo
 *
 * A unit test is a test_NAME.c file that defines a null-terminated
 * tap_cases[] table and no main. The main that walks the table lives in
 * testmain.c and is linked into every test program, so there is one copy of
 * the reporting code. Each case reports through the macros below, and the
 * program prints its results as TAP for the taptest driver to read. See
 * README.md.
 */

#ifndef TAP_TEST_H
#define TAP_TEST_H

/* The tests include vedit.c after this header, so the feature macro it
 * wants must already be set when the first system header comes in. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif


/* Per-case context, opaque to the case. */
typedef struct Test Test;

typedef struct Case Case;
struct Case {
    const char *name;
    void (*run)(Test *t);
};

/* Every test program defines this table, NULL-terminated. */
extern const Case tap_cases[];

/* Record a failure at file:line with a printf-style message. */
void tap_failf(Test *t, const char *file, int line,
    const char *fmt, ...);

/* Abandon the current case but let the rest of the file run. */
void tap_bail(Test *t);

/* Record the failure and continue. The one to reach for. */
#define TAP_CHECK(t, x) \
    do { \
        if (!(x)) \
            tap_failf((t), __FILE__, __LINE__, "%s", #x); \
    } while (0)

/* Record with a message and continue. */
#define TAP_CHECKF(t, x, ...) \
    do { \
        if (!(x)) \
            tap_failf((t), __FILE__, __LINE__, \
                #x ": " __VA_ARGS__); \
    } while (0)

/* Record and bail out of the case: for when continuing would crash. */
#define TAP_ASSERT(t, x) \
    do { \
        if (!(x)) { \
            tap_failf((t), __FILE__, __LINE__, "%s", #x); \
            tap_bail((t)); \
        } \
    } while (0)

#endif /* TAP_TEST_H */

#include <stddef.h>

/* A text line (which need not be NUL-terminated: it may borrow from the
 * file map) equals the C string want. No string.h here: this header is
 * included before vedit.c sets the feature macros. */
static inline int
lineq(const char *s, size_t len, const char *want)
{
	size_t i;

	if (!s)
		return 0;
	for (i = 0; i < len; i++)
		if (want[i] == 0 || s[i] != want[i])
			return 0;
	return want[len] == 0;
}
