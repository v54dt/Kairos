#ifndef KAIROS_EXEC_SCENARIO_TESTS_TEST_CHECK_H_
#define KAIROS_EXEC_SCENARIO_TESTS_TEST_CHECK_H_

#include <cstdio>

// Keep-going harness: a failed check prints FAIL and increments g_failures; main returns nonzero.
static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    auto _a = (a);                                                                       \
    auto _b = (b);                                                                       \
    if (!(_a == _b)) {                                                                   \
      std::printf("FAIL  %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                  (long long)_a, (long long)_b);                                         \
      ++g_failures;                                                                      \
    }                                                                                    \
  } while (0)

#endif  // KAIROS_EXEC_SCENARIO_TESTS_TEST_CHECK_H_
