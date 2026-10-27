/* SPDX-License-Identifier: CC0-1.0
 *
 * A minimal test harness: each test is a function named for the behaviour it
 * pins, RUN() reports it, and main returns non-zero if any check failed.
 */
#ifndef SCVA_TEST_CHECK_H
#define SCVA_TEST_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int t_failed, t_tests, t_skipped;
static const char *t_name;
static int t_skip_this;

#define TEST(name) static void name(void)

#define RUN(name) do {                                                    \
    int before_ = t_failed;                                               \
    t_name = #name; t_skip_this = 0; ++t_tests;                           \
    name();                                                               \
    if (t_skip_this) { ++t_skipped; printf("skip %s\n", #name); }         \
    else printf("%s %s\n", t_failed == before_ ? "ok  " : "FAIL", #name); \
  } while (0)

/* For a test whose input is not available here, such as the user's own name
   files. It reports as skipped rather than passing silently. */
#define SKIP(why) do { t_skip_this = 1; printf("  (%s)\n", why); return; } while (0)

#define CHECK(c) do {                                                     \
    if (!(c)) {                                                           \
      ++t_failed;                                                         \
      fprintf(stderr, "  %s:%d: %s: expected %s\n",                       \
              __FILE__, __LINE__, t_name, #c);                            \
    }                                                                     \
  } while (0)

#define CHECK_INT(got, want) do {                                         \
    long long g_ = (long long)(got), w_ = (long long)(want);              \
    if (g_ != w_) {                                                       \
      ++t_failed;                                                         \
      fprintf(stderr, "  %s:%d: %s: %s is %lld, expected %lld\n",         \
              __FILE__, __LINE__, t_name, #got, g_, w_);                  \
    }                                                                     \
  } while (0)

#define CHECK_STR(got, want) do {                                         \
    const char *g_ = (got), *w_ = (want);                                 \
    if (!g_ || strcmp(g_, w_)) {                                          \
      ++t_failed;                                                         \
      fprintf(stderr, "  %s:%d: %s: %s is \"%s\", expected \"%s\"\n",     \
              __FILE__, __LINE__, t_name, #got, g_ ? g_ : "(null)", w_);  \
    }                                                                     \
  } while (0)

#define CHECK_NEAR(got, want, tol) do {                                   \
    double g_ = (got), w_ = (want);                                       \
    if (g_ < w_ - (tol) || g_ > w_ + (tol)) {                             \
      ++t_failed;                                                         \
      fprintf(stderr, "  %s:%d: %s: %s is %.9g, expected %.9g\n",         \
              __FILE__, __LINE__, t_name, #got, g_, w_);                  \
    }                                                                     \
  } while (0)

static int test_summary(void)
{
  printf("%d tests, %d failed checks, %d skipped\n", t_tests, t_failed, t_skipped);
  return t_failed ? 1 : 0;
}

#ifndef _WIN32
/* A fresh directory under $TMPDIR (or /tmp) for files a test writes. Each
   call returns a new one, which stays valid for the rest of the run. */
__attribute__((unused)) static const char *test_tmpdir(void)
{
  char dir[256];
  const char *base = getenv("TMPDIR");
  snprintf(dir, sizeof dir, "%s/scva-test-XXXXXX", base && *base ? base : "/tmp");
  if (!mkdtemp(dir)) { perror("mkdtemp"); exit(2); }
  return strdup(dir);
}

__attribute__((unused)) static void test_write(const char *dir, const char *name,
                                              const char *text)
{
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "wb");
  if (!f) { perror(path); exit(2); }
  fputs(text, f);
  fclose(f);
}
#endif
#endif
