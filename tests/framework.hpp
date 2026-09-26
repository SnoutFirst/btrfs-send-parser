#pragma once

//Minimal test framework: no dependency, one macro per test case, failures abort the case with a message.
//A case either passes or throws, so a broken assertion never hides later ones inside the same case.

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "btrfs/send.hpp"

namespace testfw {

//This header and the test binaries render values of the library, so the names are imported once here instead
//of being spelled out at every declaration.
using namespace btrfs::send;

class Failure : public std::exception
{
public:
    explicit Failure(std::string message)
        : m_Message(std::move(message))
    {
    }

    const char* what() const noexcept override
    {
        return m_Message.c_str();
    }

private:
    std::string m_Message;
};

struct TestCase
{
    std::string name;
    void (*function)();
};

std::vector<TestCase>& registry();

class Registrar
{
public:
    Registrar(const char* name, void (*function)());
};

//Renders values for failure messages.
std::string to_debug(const std::string& value);
std::string to_debug(std::string_view value);
std::string to_debug(const char* value);
std::string to_debug(bool value);
std::string to_debug(protocol::Command value);
std::string to_debug(protocol::Attribute value);
std::string to_debug(ErrorCode value);
std::string to_debug(NextStatus value);
std::string to_debug(const Uuid& value);
std::string to_debug(const Timestamp& value);
std::string to_debug(const BinaryData& value);
std::string to_debug(const std::vector<std::byte>& value);
std::string to_debug(const std::vector<std::string>& value);

template <typename T>
std::string to_debug(const T& value)
{
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

template <typename T>
std::string to_debug(const std::optional<T>& value)
{
    if (!value)
        return "nullopt";
    return to_debug(*value);
}

std::string hex_encode(std::string_view bytes);

[[noreturn]] void fail(const char* file, int line, const std::string& message);

int run_all(int argc, char** argv);

}

#define TEST_CASE(case_name)                                     \
    static void test_case_##case_name();                         \
    static const ::testfw::Registrar s_Registrar_##case_name(#case_name, &test_case_##case_name); \
    static void test_case_##case_name()

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition))                                                                    \
            ::testfw::fail(__FILE__, __LINE__, "expected: " #condition);                      \
    } while (false)

#define CHECK_MSG(condition, message)                                                        \
    do {                                                                                     \
        if (!(condition))                                                                    \
            ::testfw::fail(__FILE__, __LINE__, std::string("expected: " #condition " (") + (message) + ")"); \
    } while (false)

#define CHECK_EQ(actual, expected)                                                           \
    do {                                                                                     \
        const auto& check_eq_actual = (actual);                                              \
        const auto& check_eq_expected = (expected);                                          \
        if (!(check_eq_actual == check_eq_expected))                                         \
        {                                                                                    \
            ::testfw::fail(__FILE__, __LINE__,                                               \
                std::string("expected ") + #actual " == " #expected "\n    actual:   " +     \
                ::testfw::to_debug(check_eq_actual) + "\n    expected: " +                   \
                ::testfw::to_debug(check_eq_expected));                                      \
        }                                                                                    \
    } while (false)

#define CHECK_NE(actual, unexpected)                                                         \
    do {                                                                                     \
        const auto& check_ne_actual = (actual);                                              \
        const auto& check_ne_unexpected = (unexpected);                                      \
        if (check_ne_actual == check_ne_unexpected)                                          \
        {                                                                                    \
            ::testfw::fail(__FILE__, __LINE__,                                               \
                std::string("expected ") + #actual " != " #unexpected "\n    both: " +      \
                ::testfw::to_debug(check_ne_actual));                                        \
        }                                                                                    \
    } while (false)

//Runs an expression that must throw btrfs::send::ParseError with the given code.
#define CHECK_PARSE_ERROR(expression, expected_code)                                         \
    do {                                                                                     \
        bool check_threw = false;                                                            \
        try                                                                                  \
        {                                                                                    \
            (void)(expression);                                                              \
        }                                                                                    \
        catch (const ::btrfs::send::ParseError& parse_error)                                 \
        {                                                                                    \
            check_threw = true;                                                              \
            if (parse_error.code() != (expected_code))                                       \
            {                                                                                \
                ::testfw::fail(__FILE__, __LINE__,                                           \
                    std::string("expected ") + #expected_code + " from " #expression "\n    got: " + \
                    ::btrfs::send::to_string(parse_error.code()) + " (" + parse_error.what() + ")"); \
            }                                                                                \
        }                                                                                    \
        if (!check_threw)                                                                    \
            ::testfw::fail(__FILE__, __LINE__, "expected " #expression " to throw " #expected_code); \
    } while (false)

#define CHECK_NO_THROW(expression)                                                           \
    do {                                                                                     \
        try                                                                                  \
        {                                                                                    \
            (void)(expression);                                                              \
        }                                                                                    \
        catch (const std::exception& unexpected_error)                                       \
        {                                                                                    \
            ::testfw::fail(__FILE__, __LINE__,                                               \
                std::string("did not expect " #expression " to throw: ") + unexpected_error.what()); \
        }                                                                                    \
    } while (false)
