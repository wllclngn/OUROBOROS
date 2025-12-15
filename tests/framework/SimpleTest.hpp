#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <cmath>

namespace ouroboros::test {

struct TestResult {
    std::string name;
    bool passed;
    std::string message;
};

class TestRunner {
public:
    static TestRunner& instance() {
        static TestRunner instance;
        return instance;
    }

    void register_test(const std::string& name, std::function<void()> test_func) {
        tests_.push_back({name, test_func});
    }

    int run_all() {
        int passed = 0;
        int failed = 0;

        std::cout << "\n=== OUROBOROS TEST SUITE ===\n" << std::endl;

        for (const auto& test : tests_) {
            try {
                test.func();
                std::cout << "[PASS] " << test.name << std::endl;
                passed++;
            } catch (const std::exception& e) {
                std::cout << "[FAIL] " << test.name << " - " << e.what() << std::endl;
                failed++;
            } catch (...) {
                std::cout << "[FAIL] " << test.name << " - Unknown exception" << std::endl;
                failed++;
            }
        }

        std::cout << "\nResults: " << passed << " Passed, " << failed << " Failed." << std::endl;
        return failed > 0 ? 1 : 0;
    }

private:
    struct TestEntry {
        std::string name;
        std::function<void()> func;
    };
    std::vector<TestEntry> tests_;
};

struct Registrar {
    Registrar(const std::string& name, std::function<void()> func) {
        TestRunner::instance().register_test(name, func);
    }
};

class AssertionFailure : public std::runtime_error {
public:
    AssertionFailure(const std::string& msg) : std::runtime_error(msg) {}
};

} // namespace ouroboros::test

#define TEST_CASE(name) \
    void name(); \
    static ouroboros::test::Registrar reg_##name(#name, name); \
    void name()

#define ASSERT_TRUE(condition) \
    if (!(condition)) throw ouroboros::test::AssertionFailure("Assertion failed: " #condition " at " + std::string(__FILE__) + ":" + std::to_string(__LINE__))

#define ASSERT_FALSE(condition) \
    if (condition) throw ouroboros::test::AssertionFailure("Assertion failed: " #condition " is true at " + std::string(__FILE__) + ":" + std::to_string(__LINE__))

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw ouroboros::test::AssertionFailure("Assertion failed: " #a " == " #b " at " + std::string(__FILE__) + ":" + std::to_string(__LINE__))

#define ASSERT_NEAR(a, b, epsilon) \
    if (std::abs((a) - (b)) > epsilon) throw ouroboros::test::AssertionFailure("Assertion failed: " #a " near " #b " at " + std::string(__FILE__) + ":" + std::to_string(__LINE__))
