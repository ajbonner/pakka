#ifndef PAKKA_TEST_MACROS_H
#define PAKKA_TEST_MACROS_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern int         t_failures;
extern int         t_skipped;
extern int         t_passed;
extern const char *t_current_test;

void t_test_begin(const char *name);
void t_test_end(void);
int  t_summary(void);

/* Variadic form (no `##` GNU extension) keeps the header --pedantic
 * clean. Split into two fprintf calls so callers don't have to pad
 * a dummy argument when their message has no format specifiers. */
#define FAIL(...) do {                                                      \
    fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);                  \
    fprintf(stderr, __VA_ARGS__);                                           \
    fputc('\n', stderr);                                                    \
    t_failures++;                                                           \
    return;                                                                 \
} while (0)

#define EXPECT_TRUE(cond) do {                                              \
    if (!(cond)) FAIL("EXPECT_TRUE(%s)", #cond);                            \
} while (0)

#define EXPECT_FALSE(cond) do {                                             \
    if (cond) FAIL("EXPECT_FALSE(%s)", #cond);                              \
} while (0)

#define EXPECT_EQ(actual, expected) do {                                    \
    long long _a = (long long)(actual);                                     \
    long long _e = (long long)(expected);                                   \
    if (_a != _e)                                                           \
        FAIL("EXPECT_EQ(%s, %s): got %lld, want %lld",                      \
             #actual, #expected, _a, _e);                                   \
} while (0)

#define EXPECT_NE(actual, unexpected) do {                                  \
    long long _a = (long long)(actual);                                     \
    long long _u = (long long)(unexpected);                                 \
    if (_a == _u)                                                           \
        FAIL("EXPECT_NE(%s, %s): both = %lld",                              \
             #actual, #unexpected, _a);                                     \
} while (0)

#define EXPECT_STREQ(actual, expected) do {                                 \
    const char *_a = (actual);                                              \
    const char *_e = (expected);                                            \
    if (strcmp(_a, _e) != 0)                                                \
        FAIL("EXPECT_STREQ: got \"%s\", want \"%s\"", _a, _e);              \
} while (0)

#define EXPECT_STR_CONTAINS(haystack, needle) do {                          \
    const char *_h = (haystack);                                            \
    const char *_n = (needle);                                              \
    if (strstr(_h, _n) == NULL)                                             \
        FAIL("EXPECT_STR_CONTAINS: \"%s\" not in output", _n);              \
} while (0)

#define EXPECT_STR_STARTS_WITH(s, prefix) do {                              \
    const char *_s = (s);                                                   \
    const char *_p = (prefix);                                              \
    if (strncmp(_s, _p, strlen(_p)) != 0)                                   \
        FAIL("EXPECT_STR_STARTS_WITH: \"%s\" doesn't start with \"%s\"",    \
             _s, _p);                                                       \
} while (0)

#define EXPECT_MEM_EQ(actual, expected, len) do {                           \
    if (memcmp((actual), (expected), (size_t)(len)) != 0)                   \
        FAIL("EXPECT_MEM_EQ: %zu-byte buffers differ", (size_t)(len));      \
} while (0)

#define EXPECT_NOT_NULL(ptr) do {                                           \
    if ((ptr) == NULL) FAIL("EXPECT_NOT_NULL(%s)", #ptr);                   \
} while (0)

#define EXPECT_NULL(ptr) do {                                               \
    if ((ptr) != NULL)                                                      \
        FAIL("EXPECT_NULL(%s) but got %p", #ptr, (void *)(ptr));            \
} while (0)

#define SKIP(reason) do {                                                   \
    fprintf(stdout, "  SKIP %s: %s\n", t_current_test, (reason));           \
    t_skipped++;                                                            \
    return;                                                                 \
} while (0)

#define RUN_TEST(fn) do {                                                   \
    t_test_begin(#fn);                                                      \
    fn();                                                                   \
    t_test_end();                                                           \
} while (0)

#endif
