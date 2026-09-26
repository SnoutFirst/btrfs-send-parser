#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace btrfs::send {

//16 byte Btrfs UUID. The byte order is the order used by the kernel and by the send stream itself,
//which is the same as the textual form, so no shuffling is performed anywhere.
struct Uuid
{
    static constexpr std::size_t kSize = 16;

    std::array<std::uint8_t, kSize> bytes{};

    constexpr bool is_null() const noexcept
    {
        for (std::uint8_t byte : bytes)
        {
            if (byte != 0)
                return false;
        }
        return true;
    }

    //Canonical lowercase form: 8-4-4-4-12 hex digits.
    std::string to_string() const;

    //Accepts the canonical form and bare 32 hex digits, both cases. Returns nullopt for anything else.
    static std::optional<Uuid> parse(std::string_view text);

    friend bool operator==(const Uuid& lhs, const Uuid& rhs) noexcept
    {
        return lhs.bytes == rhs.bytes;
    }

    friend bool operator!=(const Uuid& lhs, const Uuid& rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

//Timestamp as stored in the stream: 64 bit signed seconds plus 32 bit nanoseconds, both little endian.
//The kernel transmits the seconds field as an unsigned 64 bit value but the value is a signed Unix time,
//so the parser narrows it back to a signed type.
struct Timestamp
{
    static constexpr std::uint32_t kNanosecondsPerSecond = 1000000000u;

    std::int64_t seconds = 0;
    std::uint32_t nanoseconds = 0;

    bool is_valid() const noexcept
    {
        return nanoseconds < kNanosecondsPerSecond;
    }

    //ISO 8601 in UTC with nanosecond precision, for example "2020-01-02T08:04:05.000000123Z".
    //Timestamps outside the range gmtime_r can represent fall back to "@<seconds>".
    std::string to_iso8601_utc() const;

    friend bool operator==(const Timestamp& lhs, const Timestamp& rhs) noexcept
    {
        return lhs.seconds == rhs.seconds && lhs.nanoseconds == rhs.nanoseconds;
    }

    friend bool operator!=(const Timestamp& lhs, const Timestamp& rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

}
