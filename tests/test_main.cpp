#include "framework.hpp"

#include <cstdio>
#include <cstring>

namespace testfw {

namespace {

std::string indent_lines(const std::string& text)
{
    std::string result;
    for (const char character : text)
    {
        result.push_back(character);
        if (character == '\n')
            result += "        ";
    }
    return result;
}

}

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(const char* name, void (*function)())
{
    registry().push_back(TestCase{name, function});
}

std::string to_debug(const std::string& value)
{
    return "\"" + indent_lines(value) + "\"";
}

std::string to_debug(std::string_view value)
{
    return to_debug(std::string(value));
}

std::string to_debug(const char* value)
{
    return value == nullptr ? std::string("null") : to_debug(std::string(value));
}

std::string to_debug(bool value)
{
    return value ? "true" : "false";
}

std::string to_debug(protocol::Command value)
{
    return std::string(protocol::to_string(value)) + " (" + std::to_string(static_cast<std::uint16_t>(value)) + ")";
}

std::string to_debug(protocol::Attribute value)
{
    return std::string(protocol::to_string(value)) + " (" + std::to_string(static_cast<std::uint16_t>(value)) + ")";
}

std::string to_debug(ErrorCode value)
{
    return std::string(to_string(value)) + " (" + std::to_string(static_cast<int>(value)) + ")";
}

std::string to_debug(NextStatus value)
{
    switch (value)
    {
    case NextStatus::HaveOperation:
        return "HaveOperation";
    case NextStatus::EndOfStream:
        return "EndOfStream";
    case NextStatus::Error:
        return "Error";
    }
    return "unknown status";
}

std::string to_debug(const Uuid& value)
{
    return value.to_string();
}

std::string to_debug(const Timestamp& value)
{
    return value.to_iso8601_utc() + " (" + std::to_string(value.seconds) + "s " + std::to_string(value.nanoseconds) + "ns)";
}

std::string to_debug(const BinaryData& value)
{
    return "<" + std::to_string(value.size()) + " bytes " + hex_encode(value.as_string_view()) + ">";
}

std::string to_debug(const std::vector<std::byte>& value)
{
    return "<" + std::to_string(value.size()) + " bytes " + hex_encode(std::string_view(reinterpret_cast<const char*>(value.data()), value.size())) + ">";
}

std::string to_debug(const std::vector<std::string>& value)
{
    //Lists of strings are long, so only the first few are rendered in full.
    static constexpr std::size_t kShownItems = 4;
    std::string result = "[" + std::to_string(value.size()) + " item(s)";
    const std::size_t shown = value.size() < kShownItems ? value.size() : kShownItems;
    for (std::size_t index = 0; index < shown; ++index)
        result += "\n        " + to_debug(value[index]);
    if (shown < value.size())
        result += "\n        ...";
    result += "]";
    return result;
}

std::string hex_encode(std::string_view bytes)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const char character : bytes)
    {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(kHexDigits[(byte >> 4) & 0xfu]);
        result.push_back(kHexDigits[byte & 0xfu]);
    }
    return result;
}

void fail(const char* file, int line, const std::string& message)
{
    throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

int run_all(int argc, char** argv)
{
    std::string filter;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument.rfind("--filter=", 0) == 0)
            filter = argument.substr(std::strlen("--filter="));
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::vector<std::string> failures;

    for (const TestCase& test : registry())
    {
        if (!filter.empty() && test.name.find(filter) == std::string::npos)
            continue;
        try
        {
            test.function();
            ++passed;
            std::cout << "ok    " << test.name << "\n";
        }
        catch (const Failure& failure)
        {
            ++failed;
            failures.push_back(test.name + "\n    " + failure.what());
            std::cout << "FAIL  " << test.name << "\n      " << indent_lines(failure.what()) << "\n";
        }
        catch (const std::exception& error)
        {
            ++failed;
            failures.push_back(test.name + "\n    unexpected exception: " + error.what());
            std::cout << "FAIL  " << test.name << "\n      unexpected exception: " << error.what() << "\n";
        }
        catch (...)
        {
            ++failed;
            failures.push_back(test.name + "\n    unknown exception");
            std::cout << "FAIL  " << test.name << "\n      unknown exception\n";
        }
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    for (const std::string& failure : failures)
        std::cout << "\nfailed: " << failure << "\n";
    return failed == 0 ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    return testfw::run_all(argc, argv);
}
