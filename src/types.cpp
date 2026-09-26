#include "btrfs/send/types.hpp"

#include <cstdio>
#include <ctime>

namespace btrfs::send {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

//Positions of the dashes in the canonical 8-4-4-4-12 form.
constexpr std::size_t kDashPositions[] = {4, 6, 8, 10};

//Widest range gmtime_r can format as a four digit year: 0001-01-01T00:00:00Z to 9999-12-31T23:59:59Z.
constexpr std::int64_t kMinFormattableSeconds = -62135596800;
constexpr std::int64_t kMaxFormattableSeconds = 253402300799;

int hex_value(char character) noexcept
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

}

std::string Uuid::to_string() const
{
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        for (std::size_t dash : kDashPositions)
        {
            if (index == dash)
                result.push_back('-');
        }
        result.push_back(kHexDigits[(bytes[index] >> 4) & 0xfu]);
        result.push_back(kHexDigits[bytes[index] & 0xfu]);
    }
    return result;
}

std::optional<Uuid> Uuid::parse(std::string_view text)
{
    Uuid result;
    std::size_t written = 0;
    std::size_t index = 0;
    while (index < text.size() && written < kSize)
    {
        if (text[index] == '-')
        {
            ++index;
            continue;
        }
        if (index + 1 >= text.size())
            return std::nullopt;
        const int high = hex_value(text[index]);
        const int low = hex_value(text[index + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        result.bytes[written] = static_cast<std::uint8_t>((high << 4) | low);
        ++written;
        index += 2;
    }
    while (index < text.size() && text[index] == '-')
        ++index;
    if (written != kSize || index != text.size())
        return std::nullopt;
    return result;
}

std::string Timestamp::to_iso8601_utc() const
{
    if (seconds < kMinFormattableSeconds || seconds > kMaxFormattableSeconds)
        return "@" + std::to_string(seconds);

    const std::time_t raw_seconds = static_cast<std::time_t>(seconds);
    if (static_cast<std::int64_t>(raw_seconds) != seconds)
        return "@" + std::to_string(seconds);

    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &raw_seconds) != 0)
        return "@" + std::to_string(seconds);
#else
    if (gmtime_r(&raw_seconds, &utc) == nullptr)
        return "@" + std::to_string(seconds);
#endif

    char buffer[64];
    const int written = std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%09uZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, nanoseconds);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer))
        return "@" + std::to_string(seconds);
    return std::string(buffer, static_cast<std::size_t>(written));
}

}
