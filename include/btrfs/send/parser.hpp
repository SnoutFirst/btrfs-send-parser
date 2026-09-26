#pragma once

//Incremental parser for a Btrfs send stream.
//
//  ByteSource& source = ...;
//  btrfs::send::Parser parser(source);
//  while (std::optional<btrfs::send::SendOperation> operation = parser.next())
//      consume(*operation);
//
//Nothing is buffered beyond one command payload, so arbitrarily long streams are fine, and the parser
//never assumes that a read returns the whole request.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "btrfs/send/byte_source.hpp"
#include "btrfs/send/error.hpp"
#include "btrfs/send/operations.hpp"
#include "btrfs/send/protocol.hpp"

namespace btrfs::send {

enum class NextStatus
{
    HaveOperation, //operation was filled in
    EndOfStream, //clean end of the stream, no operation
    Error,       //failure was filled in, the parser is finished
};

class Parser
{
public:
    struct Options
    {
        //A stream produced by "btrfs send" always starts with the magic and the version. Turning this off
        //parses a headerless command sequence, for example the body of a stream sent with
        //BTRFS_SEND_FLAG_OMIT_STREAM_HEADER, using assumed_stream_version.
        bool expect_stream_header = true;
        std::uint32_t assumed_stream_version = protocol::kMinStreamVersion;

        //Verify the CRC32C of every command. Only worth turning off when the producer is trusted and the
        //cost of checksumming dominates.
        bool verify_checksums = true;

        //Stop after the BTRFS_SEND_C_END command instead of parsing the rest of the input.
        //Off by default because one file may hold several subvolume streams, each ended by END.
        bool stop_after_end_command = false;

        //Require the first command of a stream to be SUBVOL or SNAPSHOT, as the kernel always emits.
        bool require_subvol_or_snapshot_first = true;

        //Accept a second stream header in the middle of the input. "btrfs send" with several subvolumes and
        //without -e concatenates complete streams, so a file can hold several of them. Turning this off
        //turns such a header into ErrorCode::UnknownCommand.
        bool allow_concatenated_streams = true;

        //Turn an undefined command id into UnknownCommandOperation instead of ErrorCode::UnknownCommand.
        bool allow_unknown_commands = false;

        //Skip attribute types above protocol::kMaxAttributeType instead of failing with
        //ErrorCode::UnknownAttributeType. Skipped attributes are still listed in the operation, with a
        //16 bit length header as stream version 1 defines.
        bool allow_unknown_attribute_types = false;

        //Accept attributes that are not part of a command's definition instead of failing with
        //ErrorCode::UnexpectedAttribute. The docs allow a receiver to ignore unknown TLVs, so set this when
        //reading streams from a newer kernel than this parser knows about.
        bool allow_unexpected_attributes = false;

        //Reject commands whose declared payload is larger than this. Keeps a corrupt length field from
        //asking for a huge allocation before the data even arrives.
        std::uint32_t max_command_size = protocol::kDefaultMaxCommandSize;
    };

    //Defaults to a strict parse of a well formed stream: header required, checksums verified.
    explicit Parser(ByteSource& source);
    Parser(ByteSource& source, Options options);

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;
    Parser(Parser&&) = delete;
    Parser& operator=(Parser&&) = delete;

    //Throwing flavour: returns nullopt at the end of the stream, throws ParseError for malformed input.
    //The returned operation, including its binary payload views, is valid until the next call.
    std::optional<SendOperation> next();

    //Non-throwing flavour, safe to call from code that does not use exceptions.
    //After NextStatus::Error the parser is finished and later calls return EndOfStream.
    NextStatus next(SendOperation& operation, ParseFailure& failure);

    bool header_parsed() const noexcept;
    bool finished() const noexcept;

    //Stream version announced by the header, 0 before the header was read.
    std::uint32_t stream_version() const noexcept;

    //Bytes read from the source so far, a useful offset when reporting a failure of your own.
    std::uint64_t bytes_consumed() const noexcept;

    std::uint64_t commands_parsed() const noexcept;

    const Options& options() const noexcept;

private:
    enum class State
    {
        StreamHeader,
        Commands,
        Finished,
    };

    NextStatus step(SendOperation& operation);

    void read_stream_header();
    bool read_command(SendOperation& operation);
    void parse_attributes();

    //True when a freshly read command header is really the beginning of another stream header, which
    //happens when several complete streams follow one another in one input.
    static bool is_embedded_stream_header(const std::array<std::byte, protocol::kCommandHeaderSize>& header) noexcept;

    std::size_t read_some(std::byte* out, std::size_t size);
    void read_payload(std::uint32_t size, std::uint32_t* crc);

    [[noreturn]] void fail(ErrorCode code, std::string message, std::uint64_t offset) const;
    [[noreturn]] void fail_for_command(ErrorCode code, std::string message) const;

    ByteSource* m_Source;
    Options m_Options;
    State m_State = State::StreamHeader;
    std::uint32_t m_Version = 0;
    std::uint64_t m_BytesConsumed = 0;
    std::uint64_t m_CommandsParsed = 0;
    std::uint64_t m_CommandsInStream = 0;
    std::uint64_t m_CurrentCommandIndex = 0;
    std::uint64_t m_CommandOffset = 0;
    std::vector<std::byte> m_Payload;
    std::vector<protocol::RawAttribute> m_Attributes;
};

}
