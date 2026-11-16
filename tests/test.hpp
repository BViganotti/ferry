#pragma once

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <ferry/ferry.hpp>

struct TestFailure : std::runtime_error {
  explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

std::vector<TestCase>& test_registry();

struct TestRegistration {
  TestRegistration(const char* name, std::function<void()> fn);
};

#define TEST(name)                                                    \
  static void ferry_test_##name();                                    \
  static TestRegistration ferry_registration_##name(#name, ferry_test_##name); \
  static void ferry_test_##name()

#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!(condition)) {                                                           \
      throw TestFailure(std::string("CHECK failed: ") + #condition + " (" +      \
                        __FILE__ + ":" + std::to_string(__LINE__) + ")");         \
    }                                                                             \
  } while (0)

#define CHECK_EQ(a, b)                                                            \
  do {                                                                            \
    auto&& ferry_a = (a);                                                         \
    auto&& ferry_b = (b);                                                         \
    if (!(ferry_a == ferry_b)) {                                                  \
      std::ostringstream ferry_stream;                                            \
      ferry_stream << "CHECK_EQ failed: " #a " != " #b " (" << ferry_a << " vs " \
                   << ferry_b << ") at " << __FILE__ << ":" << __LINE__;          \
      throw TestFailure(ferry_stream.str());                                      \
    }                                                                             \
  } while (0)

inline void run_async(std::function<ferry::task<void>()> fn) {
  ferry::run([&]() -> ferry::task<void> { co_await fn(); });
}
