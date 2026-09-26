#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace btrfs::send {

//Every malformed-input condition the parser can report.
//The set is deliberately explicit so callers can react to a class of failure instead of matching error text.
enum class ErrorCode
{
    InvalidStreamHeader,   //input ended before the mandatory stream header could be read
    InvalidMagic,          //stream does not start with "btrfs-stream"
    UnsupportedVersion,    //stream version is not one this parser implements
    UnexpectedEndOfStream, //input ended in the middle of a command
    SizeOverflow,          //a length in the stream exceeds the configured safety limit
    ChecksumMismatch,      //CRC32C of a command does not match the command header
    MalformedAttribute,    //attribute header or attribute body is structurally impossible or truncated
    UnknownAttributeType,  //attribute type is not defined and the caller did not allow unknown types
    InvalidAttributeLength,//attribute length is not the one the protocol prescribes for that attribute
    MissingAttribute,      //a mandatory attribute of the command is absent
    UnexpectedAttribute,   //attribute is not part of the command definition
    InvalidAttributeValue, //attribute value is out of range or internally inconsistent
    UnknownCommand,        //command id is not defined for the stream version
    InvalidCommandStructure,//command is well formed in itself but cannot legally appear here
    SourceReadFailed,      //the byte source reported an input error
};

//Stable, human readable name of an error code ("checksum_mismatch", ...).
const char* to_string(ErrorCode code) noexcept;

//Everything known about a failure: the code, a description, and where in the stream it was detected.
struct ParseFailure
{
    ErrorCode code = ErrorCode::SourceReadFailed;
    std::string message;
    std::uint64_t stream_offset = 0;   //byte offset in the stream where the problem was detected
    std::uint64_t command_index = 0;   //1-based index of the command being parsed, 0 while reading the stream header
};

//"checksum_mismatch in command 3 at offset 412: crc32c mismatch (stream 0x11223344, computed 0x55667788)"
std::string describe(const ParseFailure& failure);

//Thrown by the throwing flavour of the parser API. The non-throwing flavour reports the same ParseFailure by value.
class ParseError : public std::runtime_error
{
public:
    explicit ParseError(ParseFailure failure);

    const ParseFailure& failure() const noexcept;
    ErrorCode code() const noexcept;
    std::uint64_t stream_offset() const noexcept;
    std::uint64_t command_index() const noexcept;

private:
    ParseFailure m_Failure;
};

}
