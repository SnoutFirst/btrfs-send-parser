#include "btrfs/send/error.hpp"

#include <utility>

namespace btrfs::send {

const char* to_string(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::InvalidStreamHeader:
        return "invalid_stream_header";
    case ErrorCode::InvalidMagic:
        return "invalid_magic";
    case ErrorCode::UnsupportedVersion:
        return "unsupported_version";
    case ErrorCode::UnexpectedEndOfStream:
        return "unexpected_end_of_stream";
    case ErrorCode::SizeOverflow:
        return "size_overflow";
    case ErrorCode::ChecksumMismatch:
        return "checksum_mismatch";
    case ErrorCode::MalformedAttribute:
        return "malformed_attribute";
    case ErrorCode::UnknownAttributeType:
        return "unknown_attribute_type";
    case ErrorCode::InvalidAttributeLength:
        return "invalid_attribute_length";
    case ErrorCode::MissingAttribute:
        return "missing_attribute";
    case ErrorCode::UnexpectedAttribute:
        return "unexpected_attribute";
    case ErrorCode::InvalidAttributeValue:
        return "invalid_attribute_value";
    case ErrorCode::UnknownCommand:
        return "unknown_command";
    case ErrorCode::InvalidCommandStructure:
        return "invalid_command_structure";
    case ErrorCode::SourceReadFailed:
        return "source_read_failed";
    }
    return "unknown";
}

std::string describe(const ParseFailure& failure)
{
    std::string result = to_string(failure.code);
    if (failure.command_index != 0)
    {
        result += " in command ";
        result += std::to_string(failure.command_index);
    }
    result += " at offset ";
    result += std::to_string(failure.stream_offset);
    if (!failure.message.empty())
    {
        result += ": ";
        result += failure.message;
    }
    return result;
}

ParseError::ParseError(ParseFailure failure)
    : std::runtime_error(describe(failure))
    , m_Failure(std::move(failure))
{
}

const ParseFailure& ParseError::failure() const noexcept
{
    return m_Failure;
}

ErrorCode ParseError::code() const noexcept
{
    return m_Failure.code;
}

std::uint64_t ParseError::stream_offset() const noexcept
{
    return m_Failure.stream_offset;
}

std::uint64_t ParseError::command_index() const noexcept
{
    return m_Failure.command_index;
}

}
