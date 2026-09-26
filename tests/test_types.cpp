#include "framework.hpp"

using namespace btrfs::send;

TEST_CASE(uuid_text_form)
{
    Uuid uuid;
    for (std::size_t index = 0; index < uuid.bytes.size(); ++index)
        uuid.bytes[index] = static_cast<std::uint8_t>(index * 16);

    CHECK_EQ(uuid.to_string(), std::string("00102030-4050-6070-8090-a0b0c0d0e0f0"));
    CHECK(!uuid.is_null());

    const std::optional<Uuid> parsed = Uuid::parse("00102030-4050-6070-8090-a0b0c0d0e0f0");
    CHECK(parsed.has_value());
    CHECK_EQ(*parsed, uuid);

    const std::optional<Uuid> bare = Uuid::parse("00102030405060708090a0b0c0d0e0f0");
    CHECK(bare.has_value());
    CHECK_EQ(*bare, uuid);

    const std::optional<Uuid> upper = Uuid::parse("00102030-4050-6070-8090-A0B0C0D0E0F0");
    CHECK(upper.has_value());
    CHECK_EQ(*upper, uuid);
}

TEST_CASE(uuid_rejects_malformed_text)
{
    CHECK(!Uuid::parse("").has_value());
    CHECK(!Uuid::parse("00102030405060708090a0b0c0d0e0f").has_value());   //one hex digit short
    CHECK(!Uuid::parse("00102030405060708090a0b0c0d0e0f0f").has_value()); //one byte too many
    CHECK(!Uuid::parse("00102030-4050-6070-8090-a0b0c0d0e0g0").has_value()); //not hex
    CHECK(!Uuid::parse("00102030_4050_6070_8090_a0b0c0d0e0f0").has_value()); //wrong separator
    CHECK(!Uuid::parse("g0102030-4050-6070-8090-a0b0c0d0e0f0").has_value());
}

TEST_CASE(uuid_null_detection)
{
    Uuid uuid;
    CHECK(uuid.is_null());
    CHECK_EQ(uuid.to_string(), std::string("00000000-0000-0000-0000-000000000000"));
    uuid.bytes[15] = 1;
    CHECK(!uuid.is_null());
}

TEST_CASE(timestamp_formatting)
{
    Timestamp epoch;
    CHECK(epoch.is_valid());
    CHECK_EQ(epoch.to_iso8601_utc(), std::string("1970-01-01T00:00:00.000000000Z"));

    //"2020-01-02 03:04:05" in the fixture's timezone is 08:04:05 UTC.
    Timestamp timestamp;
    timestamp.seconds = 1577952245;
    CHECK_EQ(timestamp.to_iso8601_utc(), std::string("2020-01-02T08:04:05.000000000Z"));

    timestamp.nanoseconds = 382858062;
    CHECK_EQ(timestamp.to_iso8601_utc(), std::string("2020-01-02T08:04:05.382858062Z"));

    timestamp.nanoseconds = 1;
    CHECK_EQ(timestamp.to_iso8601_utc(), std::string("2020-01-02T08:04:05.000000001Z"));

    timestamp.seconds = -1;
    timestamp.nanoseconds = 0;
    CHECK_EQ(timestamp.to_iso8601_utc(), std::string("1969-12-31T23:59:59.000000000Z"));
}

TEST_CASE(timestamp_validity_and_extremes)
{
    Timestamp timestamp;
    CHECK(timestamp.is_valid());
    timestamp.nanoseconds = 999999999;
    CHECK(timestamp.is_valid());
    timestamp.nanoseconds = 1000000000;
    CHECK(!timestamp.is_valid());

    //Out of range values degrade to the raw seconds instead of failing.
    timestamp.nanoseconds = 0;
    timestamp.seconds = 9223372036854775807ll;
    CHECK_EQ(timestamp.to_iso8601_utc(), std::string("@9223372036854775807"));
    timestamp.seconds = -9223372036854775807ll;
    CHECK_EQ(timestamp.to_iso8601_utc(), std::string("@-9223372036854775807"));
}
