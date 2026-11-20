// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/unique_function.h"

namespace {
struct Noisy {
    Noisy() : state{"Default constructed"} {}
    Noisy(Noisy&& rhs) noexcept : state{"Move constructed"} {
        rhs.state = "Moved away";
    }
    Noisy& operator=(Noisy&& rhs) noexcept {
        state = "Move assigned";
        rhs.state = "Moved away";
        return *this;
    }
    Noisy(const Noisy&) : state{"Copied constructed"} {}
    Noisy& operator=(const Noisy&) {
        state = "Copied assigned";
        return *this;
    }

    std::string state;
};

int Add(int a, int b) {
    return a + b;
}
} // Anonymous namespace

TEST_CASE("UniqueFunction", "[common]") {
    SECTION("Capture reference") {
        int value = 0;
        Common::UniqueFunction<void> func = [&value] { value = 5; };
        func();
        REQUIRE(value == 5);
    }
    SECTION("Capture pointer") {
        int value = 0;
        int* pointer = &value;
        Common::UniqueFunction<void> func = [pointer] { *pointer = 5; };
        func();
        REQUIRE(value == 5);
    }
    SECTION("Move object") {
        Noisy noisy;
        REQUIRE(noisy.state == "Default constructed");

        Common::UniqueFunction<void> func = [noisy_inner = std::move(noisy)] {
            REQUIRE(noisy_inner.state == "Move constructed");
        };
        REQUIRE(noisy.state == "Moved away");
        func();
    }
    SECTION("Move construct function") {
        int value = 0;
        Common::UniqueFunction<void> func = [&value] { value = 5; };
        Common::UniqueFunction<void> new_func = std::move(func);
        new_func();
        REQUIRE(value == 5);
    }
    SECTION("Move assign function") {
        int value = 0;
        Common::UniqueFunction<void> func = [&value] { value = 5; };
        Common::UniqueFunction<void> new_func;
        new_func = std::move(func);
        new_func();
        REQUIRE(value == 5);
    }
    SECTION("Default construct then assign function") {
        int value = 0;
        Common::UniqueFunction<void> func;
        func = [&value] { value = 5; };
        func();
        REQUIRE(value == 5);
    }
    SECTION("Pass arguments") {
        int result = 0;
        Common::UniqueFunction<void, int, int> func = [&result](int a, int b) { result = a + b; };
        func(5, 4);
        REQUIRE(result == 9);
    }
    SECTION("Pass arguments and return value") {
        Common::UniqueFunction<int, int, int> func = [](int a, int b) { return a + b; };
        REQUIRE(func(5, 4) == 9);
    }
    SECTION("Destructor") {
        int num_destroyed = 0;
        struct Foo {
            Foo(int* num_) : num{num_} {}
            Foo(Foo&& rhs) : num{std::exchange(rhs.num, nullptr)} {}
            Foo(const Foo&) = delete;

            ~Foo() {
                if (num) {
                    ++*num;
                }
            }

            int* num = nullptr;
        };
        Foo object{&num_destroyed};
        {
            Common::UniqueFunction<void> func = [object_inner = std::move(object)] {};
            REQUIRE(num_destroyed == 0);
        }
        REQUIRE(num_destroyed == 1);
    }
    SECTION("Owns a callable given as a const lvalue") {
        // A worker thread was handed a const lambda by name and ran it after the function that
        // held it had returned: the wrapper kept a reference, and the lambda's captures were gone.
        struct Setter {
            void operator()() const {
                *out = value;
            }
            mutable int value;
            int* out;
        };
        int result = 0;
        const Setter setter{5, &result};
        Common::UniqueFunction<void> func{setter};
        // The wrapper holds its own copy: what happens to the original no longer reaches it.
        setter.value = 6;
        func();
        REQUIRE(result == 5);
    }
    SECTION("Copies a callable given as a non-const lvalue") {
        Noisy noisy;
        auto named = [noisy] { return noisy.state; };
        Common::UniqueFunction<std::string> func{named};
        REQUIRE(named() == "Copied constructed");
        REQUIRE(func() == "Copied constructed");
    }
    SECTION("Takes a function by name") {
        Common::UniqueFunction<int, int, int> func{Add};
        REQUIRE(func(5, 4) == 9);
    }
}
