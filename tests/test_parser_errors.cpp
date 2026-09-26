#include "framework.hpp"
#include "stream_builder.hpp"

#include <stdexcept>
#include <string>

#include "btrfs/send.hpp"

using namespace btrfs::send;
using testfw::StreamBuilder;
using protocol::Attribute;
using protocol::Command;

namespace {

void put_subvol(StreamBuilder& builder)
{
    builder.begin_command(Command::Subvol);
    builder.put_string(Attribute::Path, "subvol");
    builder.put_uuid(Attribute::Uuid, testfw::make_uuid(0xaa));
    builder.put_u64(Attribute::CtransId, 7);
    builder.end_command();
}

void put_end(StreamBuilder& builder)
{
    builder.begin_command(Command::End);
    builder.end_command();
}

//Parses the whole input and expects it to fail with the given code.
void expect_first_failure(const std::string& bytes, ErrorCode code, Parser::Options options = {})
{
    MemorySource source(bytes);
    Parser parser(source, options);
    try
    {
        while (parser.next().has_value())
        {
        }
    }
    catch (const ParseError& error)
    {
        if (error.code() != code)
            testfw::fail(__FILE__, __LINE__, std::string("expected ") + to_string(code) + ", got " + to_string(error.code()) + " (" + error.what() + ")");
        return;
    }
    testfw::fail(__FILE__, __LINE__, std::string("expected ") + to_string(code) + ", the stream parsed cleanly");
}

//A byte source that always fails, to check that source errors are reported and not swallowed.
class FailingSource final : public ByteSource
{
public:
    std::size_t read(std::byte* out, std::size_t size) override
    {
        (void)out;
        (void)size;
        throw std::runtime_error("the disk went away");
    }
};

}

TEST_CASE(empty_input_is_rejected)
{
    expect_first_failure(std::string(), ErrorCode::InvalidStreamHeader);
}

TEST_CASE(valid_stream_does_not_fail)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    put_end(builder);
    MemorySource source(builder.bytes());
    Parser parser(source);
    CHECK(parser.next().has_value());
    CHECK(parser.next().has_value());
    CHECK(!parser.next().has_value());
    CHECK(parser.commands_parsed() == 2);
}

TEST_CASE(truncated_stream_header)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    put_end(builder);

    for (std::size_t size = 1; size < protocol::kStreamHeaderSize; ++size)
    {
        expect_first_failure(builder.bytes().substr(0, size), ErrorCode::InvalidStreamHeader);
    }
}

TEST_CASE(wrong_magic)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    put_end(builder);

    std::string bytes = builder.bytes();
    bytes[0] = 'B';
    expect_first_failure(bytes, ErrorCode::InvalidMagic);

    bytes = builder.bytes();
    bytes[protocol::kStreamMagicFieldSize - 1] = 'X';   //the NUL terminator of the magic field
    expect_first_failure(bytes, ErrorCode::InvalidMagic);
}

TEST_CASE(unsupported_versions)
{
    {
        StreamBuilder builder;
        put_subvol(builder);
        std::string bytes = builder.bytes();
        bytes.replace(protocol::kStreamMagicFieldSize, 4, std::string("\0\0\0\0", 4));
        expect_first_failure(bytes, ErrorCode::UnsupportedVersion);
    }
    {
        StreamBuilder builder;
        put_subvol(builder);
        std::string bytes = builder.bytes();
        bytes.replace(protocol::kStreamMagicFieldSize, 4, std::string("\x04\0\0\0", 4));
        expect_first_failure(bytes, ErrorCode::UnsupportedVersion);
    }
    {
        StreamBuilder builder;
        put_subvol(builder);
        std::string bytes = builder.bytes();
        bytes.replace(protocol::kStreamMagicFieldSize, 4, std::string("\xff\xff\xff\xff", 4));
        expect_first_failure(bytes, ErrorCode::UnsupportedVersion);
    }
}

TEST_CASE(truncated_command_header)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    put_end(builder);

    //Cutting the input exactly at a command boundary is a clean end of stream, so only a partial header is
    //an error.
    for (std::size_t size = 1; size < protocol::kCommandHeaderSize; ++size)
    {
        const std::string bytes = builder.bytes().substr(0, protocol::kStreamHeaderSize + size);
        expect_first_failure(bytes, ErrorCode::UnexpectedEndOfStream);
    }
}

TEST_CASE(truncated_command_payload)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    put_end(builder);

    const std::size_t payload_start = protocol::kStreamHeaderSize + protocol::kCommandHeaderSize;
    for (std::size_t size = 1; size < 42; ++size)
    {
        const std::string bytes = builder.bytes().substr(0, payload_start + size);
        expect_first_failure(bytes, ErrorCode::UnexpectedEndOfStream);
    }
}

TEST_CASE(truncated_after_a_complete_command)
{
    //The first command is complete, the second one is cut in half.
    StreamBuilder builder(1);
    put_subvol(builder);
    put_end(builder);

    const std::size_t after_subvol = protocol::kStreamHeaderSize + protocol::kCommandHeaderSize + 42;
    const std::string bytes = builder.bytes().substr(0, after_subvol + 4);
    MemorySource source(bytes);
    Parser parser(source);
    CHECK(parser.next().has_value());
    CHECK_PARSE_ERROR(parser.next(), ErrorCode::UnexpectedEndOfStream);
}

TEST_CASE(checksum_mismatch_is_detected)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Unlink);
    builder.put_string(Attribute::Path, "gone.txt");
    builder.end_command();
    put_end(builder);

    builder.corrupt_checksum_of_command(1);
    expect_first_failure(builder.bytes(), ErrorCode::ChecksumMismatch);
}

TEST_CASE(corrupted_payload_is_detected)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Unlink);
    builder.put_string(Attribute::Path, "gone.txt");
    builder.end_command();
    put_end(builder);

    //The payload of END is empty, so command 1 is the one with bytes to flip.
    builder.corrupt_payload_of_command(1);
    expect_first_failure(builder.bytes(), ErrorCode::ChecksumMismatch);
}

TEST_CASE(oversized_command_length)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    put_end(builder);

    Parser::Options options;
    //The SUBVOL payload of this stream is 42 bytes, so a limit below that refuses the very first command
    //before its payload is read.
    options.max_command_size = 32;
    MemorySource source(builder.bytes());
    Parser parser(source, options);
    CHECK_PARSE_ERROR(parser.next(), ErrorCode::SizeOverflow);

    //A command that declares far more than the configured limit must not cause an allocation attempt.
    std::string bytes = builder.bytes().substr(0, protocol::kStreamHeaderSize);
    std::string header;
    testfw::append_le32(header, 0xffffffffu);
    testfw::append_le16(header, static_cast<std::uint16_t>(Command::End));
    testfw::append_le32(header, 0);
    bytes += header;
    expect_first_failure(bytes, ErrorCode::SizeOverflow, options);
}

TEST_CASE(zero_length_end_command_is_valid)
{
    //END has an empty payload, so a declared length of zero must not be treated as an error.
    StreamBuilder builder(1);
    put_subvol(builder);
    put_end(builder);

    MemorySource source(builder.bytes());
    Parser parser(source);
    CHECK(parser.next().has_value());
    const std::optional<SendOperation> end = parser.next();
    CHECK(end.has_value());
    CHECK(end->is<EndOperation>());
    CHECK_EQ(end->payload_size, std::uint32_t(0));
}

TEST_CASE(truncated_attribute_header)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Unlink);
    builder.put_string(Attribute::Path, "gone.txt");
    builder.put_truncated_attribute_header(static_cast<std::uint16_t>(Attribute::Path));
    builder.put_raw_payload("xy");
    builder.end_command();
    put_end(builder);

    expect_first_failure(builder.bytes(), ErrorCode::MalformedAttribute);
}

TEST_CASE(attribute_type_zero_is_invalid)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Unlink);
    builder.put_raw_attribute(0, "value");
    builder.end_command();
    put_end(builder);

    expect_first_failure(builder.bytes(), ErrorCode::MalformedAttribute);
}

TEST_CASE(attribute_length_beyond_the_payload)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Unlink);
    builder.put_malformed_attribute(static_cast<std::uint16_t>(Attribute::Path), 100, "short");
    builder.end_command();
    put_end(builder);

    expect_first_failure(builder.bytes(), ErrorCode::MalformedAttribute);
}

TEST_CASE(missing_mandatory_attributes)
{
    {
        StreamBuilder builder(1);
        put_subvol(builder);
        builder.begin_command(Command::Mkfile);
        builder.put_string(Attribute::Path, "f1.txt");
        builder.end_command();
        put_end(builder);
        expect_first_failure(builder.bytes(), ErrorCode::MissingAttribute);
    }
    {
        StreamBuilder builder(1);
        put_subvol(builder);
        builder.begin_command(Command::SetXattr);
        builder.put_string(Attribute::Path, "f1.txt");
        builder.put_string(Attribute::XattrName, "user.text");
        builder.end_command();
        put_end(builder);
        expect_first_failure(builder.bytes(), ErrorCode::MissingAttribute);
    }
    {
        StreamBuilder builder(1);
        put_subvol(builder);
        builder.begin_command(Command::Utimes);
        builder.put_string(Attribute::Path, "f1.txt");
        builder.put_timespec(Attribute::Atime, 1, 0);
        builder.put_timespec(Attribute::Mtime, 2, 0);
        builder.end_command();
        put_end(builder);
        expect_first_failure(builder.bytes(), ErrorCode::MissingAttribute);
    }
    {
        StreamBuilder builder(1);
        builder.begin_command(Command::Subvol);
        builder.put_string(Attribute::Path, "subvol");
        builder.put_uuid(Attribute::Uuid, testfw::make_uuid(0xaa));
        builder.end_command();
        expect_first_failure(builder.bytes(), ErrorCode::MissingAttribute);
    }
    {
        StreamBuilder builder(2);
        put_subvol(builder);
        builder.begin_command(Command::EncodedWrite);
        builder.put_string(Attribute::Path, "compressed.txt");
        builder.put_u64(Attribute::FileOffset, 0);
        builder.put_u64(Attribute::UnencodedFileLen, 10);
        builder.put_u64(Attribute::UnencodedLen, 10);
        builder.put_u64(Attribute::UnencodedOffset, 0);
        builder.end_command();
        put_end(builder);
        expect_first_failure(builder.bytes(), ErrorCode::MissingAttribute);
    }
    {
        StreamBuilder builder(3);
        put_subvol(builder);
        builder.begin_command(Command::EnableVerity);
        builder.put_string(Attribute::Path, "signed.bin");
        builder.put_u8(Attribute::VerityAlgorithm, 1);
        builder.put_u32(Attribute::VerityBlockSize, 4096);
        builder.end_command();
        put_end(builder);
        expect_first_failure(builder.bytes(), ErrorCode::MissingAttribute);
    }
}

TEST_CASE(attribute_with_the_wrong_length)
{
    //Declared length 4 with 8 bytes of value: the leftover bytes do not tile, so the payload itself is
    //malformed and the parser never gets as far as looking at the value.
    StreamBuilder loose(1);
    put_subvol(loose);
    loose.begin_command(Command::Mkfile);
    loose.put_string(Attribute::Path, "f1.txt");
    loose.put_u64_raw(Attribute::Ino, 257, 4);
    loose.end_command();
    put_end(loose);
    expect_first_failure(loose.bytes(), ErrorCode::MalformedAttribute);

    //Declared length 4 with exactly 4 bytes: the attribute stream is well formed, the value is simply too
    //short for an inode number.
    StreamBuilder short_value(1);
    put_subvol(short_value);
    short_value.begin_command(Command::Mkfile);
    short_value.put_string(Attribute::Path, "f1.txt");
    short_value.put_u32(Attribute::Ino, 257);
    short_value.end_command();
    put_end(short_value);
    expect_first_failure(short_value.bytes(), ErrorCode::InvalidAttributeLength);
}

TEST_CASE(uuid_with_the_wrong_length)
{
    StreamBuilder builder(1);
    builder.begin_command(Command::Subvol);
    builder.put_string(Attribute::Path, "subvol");
    builder.put_bytes(Attribute::Uuid, std::string(15, '\xaa'));
    builder.put_u64(Attribute::CtransId, 7);
    builder.end_command();

    expect_first_failure(builder.bytes(), ErrorCode::InvalidAttributeLength);
}

TEST_CASE(timestamp_with_an_impossible_fraction)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Utimes);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_timespec(Attribute::Atime, 1, 1000000000);
    builder.put_timespec(Attribute::Mtime, 2, 0);
    builder.put_timespec(Attribute::Ctime, 3, 0);
    builder.end_command();
    put_end(builder);

    expect_first_failure(builder.bytes(), ErrorCode::InvalidAttributeValue);
}

TEST_CASE(command_from_a_newer_protocol_version)
{
    //FALLOCATE only exists from stream version 2 on.
    StreamBuilder version1(1);
    put_subvol(version1);
    version1.begin_command(Command::Fallocate);
    version1.put_string(Attribute::Path, "f1.txt");
    version1.put_u32(Attribute::FallocateMode, 0x2);
    version1.put_u64(Attribute::FileOffset, 0);
    version1.put_u64(Attribute::Size, 4096);
    version1.end_command();
    put_end(version1);
    expect_first_failure(version1.bytes(), ErrorCode::InvalidCommandStructure);

    //ENABLE_VERITY only exists from stream version 3 on.
    StreamBuilder version2(2);
    put_subvol(version2);
    version2.begin_command(Command::EnableVerity);
    version2.put_string(Attribute::Path, "signed.bin");
    version2.put_u8(Attribute::VerityAlgorithm, 1);
    version2.put_u32(Attribute::VerityBlockSize, 4096);
    version2.put_bytes(Attribute::VeritySaltData, "salt");
    version2.put_bytes(Attribute::VeritySigData, "sig");
    version2.end_command();
    put_end(version2);
    expect_first_failure(version2.bytes(), ErrorCode::InvalidCommandStructure);
}

TEST_CASE(source_errors_are_reported)
{
    FailingSource source;
    Parser parser(source);
    CHECK_PARSE_ERROR(parser.next(), ErrorCode::SourceReadFailed);

    //And through the non-throwing API as well.
    FailingSource second_source;
    Parser second_parser(second_source);
    SendOperation operation;
    ParseFailure failure;
    CHECK_EQ(second_parser.next(operation, failure), NextStatus::Error);
    CHECK_EQ(failure.code, ErrorCode::SourceReadFailed);
    CHECK(failure.message.find("the disk went away") != std::string::npos);
}

TEST_CASE(error_location_is_reported)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Mkfile);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.end_command();   //no ino
    put_end(builder);

    MemorySource source(builder.bytes());
    Parser parser(source);
    CHECK(parser.next().has_value());
    try
    {
        parser.next();
        CHECK_MSG(false, "expected a failure");
    }
    catch (const ParseError& error)
    {
        CHECK_EQ(error.code(), ErrorCode::MissingAttribute);
        CHECK_EQ(error.command_index(), std::uint64_t(2));
        CHECK(error.stream_offset() >= protocol::kStreamHeaderSize + protocol::kCommandHeaderSize);
        CHECK(error.what() == describe(error.failure()));
    }
}
