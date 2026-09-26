#include "btrfs/send/parser.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <iterator>
#include <string>
#include <utility>

#include "btrfs/send/crc32c.hpp"

namespace btrfs::send {

namespace {

//How many payload bytes are requested per read() call. Small enough that a corrupt length field cannot
//force a large allocation before the data actually arrives, large enough to keep the syscall count sane.
constexpr std::size_t kReadChunkSize = 64u * 1024u;

//Extra slack when the payload buffer grows, so appending does not turn quadratically expensive.
constexpr std::size_t kPayloadGrowthSlack = 4u * 1024u;

constexpr std::size_t kU8Size = 1;
constexpr std::size_t kU32Size = 4;
constexpr std::size_t kU64Size = 8;
constexpr std::size_t kUuidSize = 16;
constexpr std::size_t kTimespecSize = 12;

//Attributes each command may carry. Anything outside the list is rejected unless
//Parser::Options::allow_unexpected_attributes is set.
struct AttributeTable
{
    const protocol::Attribute* attributes;
    std::size_t size;
};

constexpr protocol::Attribute kSubvolAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Uuid, protocol::Attribute::CtransId};
constexpr protocol::Attribute kSnapshotAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Uuid, protocol::Attribute::CtransId, protocol::Attribute::CloneUuid, protocol::Attribute::CloneCtransId};
constexpr protocol::Attribute kMkfileAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Ino};
constexpr protocol::Attribute kMkdirAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Ino};
constexpr protocol::Attribute kMknodAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Ino, protocol::Attribute::Mode, protocol::Attribute::Rdev};
constexpr protocol::Attribute kMkfifoAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Ino, protocol::Attribute::Rdev, protocol::Attribute::Mode};
constexpr protocol::Attribute kMksockAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Ino, protocol::Attribute::Rdev, protocol::Attribute::Mode};
constexpr protocol::Attribute kSymlinkAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Ino, protocol::Attribute::PathLink};
constexpr protocol::Attribute kRenameAttributes[] = {protocol::Attribute::Path, protocol::Attribute::PathTo};
constexpr protocol::Attribute kLinkAttributes[] = {protocol::Attribute::Path, protocol::Attribute::PathLink};
constexpr protocol::Attribute kPathOnlyAttributes[] = {protocol::Attribute::Path};
constexpr protocol::Attribute kSetXattrAttributes[] = {protocol::Attribute::Path, protocol::Attribute::XattrName, protocol::Attribute::XattrData};
constexpr protocol::Attribute kRemoveXattrAttributes[] = {protocol::Attribute::Path, protocol::Attribute::XattrName};
constexpr protocol::Attribute kWriteAttributes[] = {protocol::Attribute::Path, protocol::Attribute::FileOffset, protocol::Attribute::Data};
constexpr protocol::Attribute kCloneAttributes[] = {protocol::Attribute::Path, protocol::Attribute::FileOffset, protocol::Attribute::CloneLen, protocol::Attribute::CloneUuid, protocol::Attribute::CloneCtransId, protocol::Attribute::ClonePath, protocol::Attribute::CloneOffset};
constexpr protocol::Attribute kTruncateAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Size};
constexpr protocol::Attribute kChmodAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Mode};
constexpr protocol::Attribute kChownAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Uid, protocol::Attribute::Gid};
constexpr protocol::Attribute kUtimesAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Atime, protocol::Attribute::Mtime, protocol::Attribute::Ctime, protocol::Attribute::Otime};
constexpr protocol::Attribute kUpdateExtentAttributes[] = {protocol::Attribute::Path, protocol::Attribute::FileOffset, protocol::Attribute::Size};
constexpr protocol::Attribute kFallocateAttributes[] = {protocol::Attribute::Path, protocol::Attribute::FallocateMode, protocol::Attribute::FileOffset, protocol::Attribute::Size};
constexpr protocol::Attribute kFileattrAttributes[] = {protocol::Attribute::Path, protocol::Attribute::Fileattr};
constexpr protocol::Attribute kEncodedWriteAttributes[] = {protocol::Attribute::Path, protocol::Attribute::FileOffset, protocol::Attribute::UnencodedFileLen, protocol::Attribute::UnencodedLen, protocol::Attribute::UnencodedOffset, protocol::Attribute::Compression, protocol::Attribute::Encryption, protocol::Attribute::Data};
constexpr protocol::Attribute kEnableVerityAttributes[] = {protocol::Attribute::Path, protocol::Attribute::VerityAlgorithm, protocol::Attribute::VerityBlockSize, protocol::Attribute::VeritySaltData, protocol::Attribute::VeritySigData};

AttributeTable allowed_attributes(protocol::Command command) noexcept
{
    switch (command)
    {
    case protocol::Command::Subvol:
        return {kSubvolAttributes, std::size(kSubvolAttributes)};
    case protocol::Command::Snapshot:
        return {kSnapshotAttributes, std::size(kSnapshotAttributes)};
    case protocol::Command::Mkfile:
        return {kMkfileAttributes, std::size(kMkfileAttributes)};
    case protocol::Command::Mkdir:
        return {kMkdirAttributes, std::size(kMkdirAttributes)};
    case protocol::Command::Mknod:
        return {kMknodAttributes, std::size(kMknodAttributes)};
    case protocol::Command::Mkfifo:
        return {kMkfifoAttributes, std::size(kMkfifoAttributes)};
    case protocol::Command::Mksock:
        return {kMksockAttributes, std::size(kMksockAttributes)};
    case protocol::Command::Symlink:
        return {kSymlinkAttributes, std::size(kSymlinkAttributes)};
    case protocol::Command::Rename:
        return {kRenameAttributes, std::size(kRenameAttributes)};
    case protocol::Command::Link:
        return {kLinkAttributes, std::size(kLinkAttributes)};
    case protocol::Command::Unlink:
    case protocol::Command::Rmdir:
        return {kPathOnlyAttributes, std::size(kPathOnlyAttributes)};
    case protocol::Command::SetXattr:
        return {kSetXattrAttributes, std::size(kSetXattrAttributes)};
    case protocol::Command::RemoveXattr:
        return {kRemoveXattrAttributes, std::size(kRemoveXattrAttributes)};
    case protocol::Command::Write:
        return {kWriteAttributes, std::size(kWriteAttributes)};
    case protocol::Command::Clone:
        return {kCloneAttributes, std::size(kCloneAttributes)};
    case protocol::Command::Truncate:
        return {kTruncateAttributes, std::size(kTruncateAttributes)};
    case protocol::Command::Chmod:
        return {kChmodAttributes, std::size(kChmodAttributes)};
    case protocol::Command::Chown:
        return {kChownAttributes, std::size(kChownAttributes)};
    case protocol::Command::Utimes:
        return {kUtimesAttributes, std::size(kUtimesAttributes)};
    case protocol::Command::End:
    case protocol::Command::Unspec:
        return {nullptr, 0};
    case protocol::Command::UpdateExtent:
        return {kUpdateExtentAttributes, std::size(kUpdateExtentAttributes)};
    case protocol::Command::Fallocate:
        return {kFallocateAttributes, std::size(kFallocateAttributes)};
    case protocol::Command::Fileattr:
        return {kFileattrAttributes, std::size(kFileattrAttributes)};
    case protocol::Command::EncodedWrite:
        return {kEncodedWriteAttributes, std::size(kEncodedWriteAttributes)};
    case protocol::Command::EnableVerity:
        return {kEnableVerityAttributes, std::size(kEnableVerityAttributes)};
    }
    return {nullptr, 0};
}

std::string hex32(std::uint32_t value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result = "00000000";
    for (std::size_t index = 0; index < 8; ++index)
        result[7 - index] = kHexDigits[(value >> (index * 4)) & 0xfu];
    return result;
}

//Turns the raw attributes of one command into the typed operation.
class Decoder
{
public:
    Decoder(protocol::Command command, std::uint32_t version, const Parser::Options& options, const std::vector<protocol::RawAttribute>& attributes, std::uint64_t command_index, std::uint64_t payload_offset)
        : m_Command(command)
        , m_Version(version)
        , m_Options(options)
        , m_Attributes(attributes)
        , m_CommandIndex(command_index)
        , m_PayloadOffset(payload_offset)
    {
    }

    Operation run();

private:
    const protocol::RawAttribute* find(protocol::Attribute type) const noexcept;
    const protocol::RawAttribute& require(protocol::Attribute type) const;

    std::string_view string_value(protocol::Attribute type) const;
    std::uint8_t u8_value(protocol::Attribute type) const;
    std::uint32_t u32_value(protocol::Attribute type) const;
    std::uint64_t u64_value(protocol::Attribute type) const;
    Uuid uuid_value(protocol::Attribute type) const;
    Timestamp timestamp_value(protocol::Attribute type) const;
    BinaryData binary_value(protocol::Attribute type) const;

    std::optional<std::uint32_t> optional_u32(protocol::Attribute type) const;
    std::optional<std::uint64_t> optional_u64(protocol::Attribute type) const;
    std::optional<Timestamp> optional_timestamp(protocol::Attribute type) const;

    std::size_t check_size(protocol::Attribute type, const protocol::RawAttribute& attribute, std::size_t expected) const;
    void check_attributes() const;

    [[noreturn]] void fail(ErrorCode code, std::string message, const protocol::RawAttribute* attribute = nullptr) const;

    protocol::Command m_Command;
    std::uint32_t m_Version;
    const Parser::Options& m_Options;
    const std::vector<protocol::RawAttribute>& m_Attributes;
    std::uint64_t m_CommandIndex;
    std::uint64_t m_PayloadOffset;
};

void Decoder::fail(ErrorCode code, std::string message, const protocol::RawAttribute* attribute) const
{
    std::uint64_t offset = m_PayloadOffset;
    if (attribute != nullptr)
        offset += attribute->payload_offset;
    else if (!m_Attributes.empty())
        offset += m_Attributes.back().payload_offset;
    throw ParseFailure{code, std::move(message), offset, m_CommandIndex};
}

const protocol::RawAttribute* Decoder::find(protocol::Attribute type) const noexcept
{
    const protocol::RawAttribute* found = nullptr;
    for (const protocol::RawAttribute& attribute : m_Attributes)
    {
        if (attribute.type == type)
            found = &attribute;
    }
    return found;
}

const protocol::RawAttribute& Decoder::require(protocol::Attribute type) const
{
    const protocol::RawAttribute* attribute = find(type);
    if (attribute == nullptr)
        fail(ErrorCode::MissingAttribute, std::string("command ") + protocol::to_string(m_Command) + " requires the attribute " + protocol::to_string(type));
    return *attribute;
}

std::size_t Decoder::check_size(protocol::Attribute type, const protocol::RawAttribute& attribute, std::size_t expected) const
{
    if (attribute.size != expected)
    {
        fail(ErrorCode::InvalidAttributeLength, std::string("attribute ") + protocol::to_string(type) + " must be " + std::to_string(expected) + " bytes, got " + std::to_string(attribute.size), &attribute);
    }
    return attribute.size;
}

std::string_view Decoder::string_value(protocol::Attribute type) const
{
    const protocol::RawAttribute& attribute = require(type);
    return std::string_view(reinterpret_cast<const char*>(attribute.data), attribute.size);
}

std::uint8_t Decoder::u8_value(protocol::Attribute type) const
{
    const protocol::RawAttribute& attribute = require(type);
    check_size(type, attribute, kU8Size);
    return static_cast<std::uint8_t>(attribute.data[0]);
}

std::uint32_t Decoder::u32_value(protocol::Attribute type) const
{
    const protocol::RawAttribute& attribute = require(type);
    check_size(type, attribute, kU32Size);
    return protocol::read_le32(attribute.data);
}

std::uint64_t Decoder::u64_value(protocol::Attribute type) const
{
    const protocol::RawAttribute& attribute = require(type);
    check_size(type, attribute, kU64Size);
    return protocol::read_le64(attribute.data);
}

Uuid Decoder::uuid_value(protocol::Attribute type) const
{
    const protocol::RawAttribute& attribute = require(type);
    check_size(type, attribute, kUuidSize);
    Uuid uuid;
    std::memcpy(uuid.bytes.data(), attribute.data, Uuid::kSize);
    return uuid;
}

Timestamp Decoder::timestamp_value(protocol::Attribute type) const
{
    const protocol::RawAttribute& attribute = require(type);
    check_size(type, attribute, kTimespecSize);
    Timestamp timestamp;
    timestamp.seconds = static_cast<std::int64_t>(protocol::read_le64(attribute.data));
    timestamp.nanoseconds = protocol::read_le32(attribute.data + 8);
    if (!timestamp.is_valid())
    {
        fail(ErrorCode::InvalidAttributeValue, std::string("attribute ") + protocol::to_string(type) + " has " + std::to_string(timestamp.nanoseconds) + " nanoseconds, which is not a valid fraction of a second", &attribute);
    }
    return timestamp;
}

BinaryData Decoder::binary_value(protocol::Attribute type) const
{
    const protocol::RawAttribute& attribute = require(type);
    return BinaryData::view(attribute.data, attribute.size);
}

std::optional<std::uint32_t> Decoder::optional_u32(protocol::Attribute type) const
{
    const protocol::RawAttribute* attribute = find(type);
    if (attribute == nullptr)
        return std::nullopt;
    check_size(type, *attribute, kU32Size);
    return protocol::read_le32(attribute->data);
}

std::optional<std::uint64_t> Decoder::optional_u64(protocol::Attribute type) const
{
    const protocol::RawAttribute* attribute = find(type);
    if (attribute == nullptr)
        return std::nullopt;
    check_size(type, *attribute, kU64Size);
    return protocol::read_le64(attribute->data);
}

std::optional<Timestamp> Decoder::optional_timestamp(protocol::Attribute type) const
{
    if (find(type) == nullptr)
        return std::nullopt;
    return timestamp_value(type);
}

void Decoder::check_attributes() const
{
    if (m_Options.allow_unexpected_attributes)
        return;

    const AttributeTable table = allowed_attributes(m_Command);
    for (const protocol::RawAttribute& attribute : m_Attributes)
    {
        if (!protocol::is_known_attribute(attribute.type))
            continue;   //types above kMaxAttributeType were already accepted or rejected while parsing the TLVs

        bool allowed = false;
        for (std::size_t index = 0; index < table.size; ++index)
        {
            if (table.attributes[index] == attribute.type)
            {
                allowed = true;
                break;
            }
        }
        if (!allowed)
        {
            fail(ErrorCode::UnexpectedAttribute, std::string("command ") + protocol::to_string(m_Command) + " does not take the attribute " + protocol::to_string(attribute.type) + " (pass Options::allow_unexpected_attributes to accept it)", &attribute);
        }
    }
}

Operation Decoder::run()
{
    const std::uint32_t required_version = protocol::minimum_version(m_Command);
    if (required_version > m_Version)
    {
        fail(ErrorCode::InvalidCommandStructure, std::string("command ") + protocol::to_string(m_Command) + " needs stream version " + std::to_string(required_version) + ", this stream is version " + std::to_string(m_Version));
    }
    check_attributes();

    switch (m_Command)
    {
    case protocol::Command::Subvol:
    {
        SubvolOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.uuid = uuid_value(protocol::Attribute::Uuid);
        operation.ctrans_id = u64_value(protocol::Attribute::CtransId);
        return operation;
    }
    case protocol::Command::Snapshot:
    {
        SnapshotOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.uuid = uuid_value(protocol::Attribute::Uuid);
        operation.ctrans_id = u64_value(protocol::Attribute::CtransId);
        operation.clone_uuid = uuid_value(protocol::Attribute::CloneUuid);
        operation.clone_ctrans_id = u64_value(protocol::Attribute::CloneCtransId);
        return operation;
    }
    case protocol::Command::Mkfile:
    {
        MkfileOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.ino = u64_value(protocol::Attribute::Ino);
        return operation;
    }
    case protocol::Command::Mkdir:
    {
        MkdirOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.ino = u64_value(protocol::Attribute::Ino);
        return operation;
    }
    case protocol::Command::Mknod:
    {
        MknodOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        //The documented format omits INO for MKNOD and the kernel has always sent it, so it is optional here.
        if (const std::optional<std::uint64_t> ino = optional_u64(protocol::Attribute::Ino))
            operation.ino = *ino;
        operation.mode = u64_value(protocol::Attribute::Mode);
        operation.rdev = u64_value(protocol::Attribute::Rdev);
        return operation;
    }
    case protocol::Command::Mkfifo:
    {
        MkfifoOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.ino = u64_value(protocol::Attribute::Ino);
        operation.rdev = optional_u64(protocol::Attribute::Rdev);
        operation.mode = optional_u64(protocol::Attribute::Mode);
        return operation;
    }
    case protocol::Command::Mksock:
    {
        MksockOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.ino = u64_value(protocol::Attribute::Ino);
        operation.rdev = optional_u64(protocol::Attribute::Rdev);
        operation.mode = optional_u64(protocol::Attribute::Mode);
        return operation;
    }
    case protocol::Command::Symlink:
    {
        SymlinkOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.ino = u64_value(protocol::Attribute::Ino);
        operation.link_target = std::string(string_value(protocol::Attribute::PathLink));
        return operation;
    }
    case protocol::Command::Rename:
    {
        RenameOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.path_to = std::string(string_value(protocol::Attribute::PathTo));
        return operation;
    }
    case protocol::Command::Link:
    {
        LinkOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.link_target = std::string(string_value(protocol::Attribute::PathLink));
        return operation;
    }
    case protocol::Command::Unlink:
    {
        UnlinkOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        return operation;
    }
    case protocol::Command::Rmdir:
    {
        RmdirOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        return operation;
    }
    case protocol::Command::SetXattr:
    {
        SetXattrOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.name = std::string(string_value(protocol::Attribute::XattrName));
        operation.value = binary_value(protocol::Attribute::XattrData);
        return operation;
    }
    case protocol::Command::RemoveXattr:
    {
        RemoveXattrOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.name = std::string(string_value(protocol::Attribute::XattrName));
        return operation;
    }
    case protocol::Command::Write:
    {
        WriteOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.file_offset = u64_value(protocol::Attribute::FileOffset);
        operation.data = binary_value(protocol::Attribute::Data);
        return operation;
    }
    case protocol::Command::Clone:
    {
        CloneOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.file_offset = u64_value(protocol::Attribute::FileOffset);
        operation.length = u64_value(protocol::Attribute::CloneLen);
        operation.clone_uuid = uuid_value(protocol::Attribute::CloneUuid);
        operation.clone_ctrans_id = u64_value(protocol::Attribute::CloneCtransId);
        operation.clone_path = std::string(string_value(protocol::Attribute::ClonePath));
        operation.clone_offset = u64_value(protocol::Attribute::CloneOffset);
        return operation;
    }
    case protocol::Command::Truncate:
    {
        TruncateOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.size = u64_value(protocol::Attribute::Size);
        return operation;
    }
    case protocol::Command::Chmod:
    {
        ChmodOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.mode = u64_value(protocol::Attribute::Mode);
        return operation;
    }
    case protocol::Command::Chown:
    {
        ChownOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.uid = u64_value(protocol::Attribute::Uid);
        operation.gid = u64_value(protocol::Attribute::Gid);
        return operation;
    }
    case protocol::Command::Utimes:
    {
        UtimesOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.atime = timestamp_value(protocol::Attribute::Atime);
        operation.mtime = timestamp_value(protocol::Attribute::Mtime);
        operation.ctime = timestamp_value(protocol::Attribute::Ctime);
        operation.otime = optional_timestamp(protocol::Attribute::Otime);
        return operation;
    }
    case protocol::Command::End:
        return EndOperation{};
    case protocol::Command::UpdateExtent:
    {
        UpdateExtentOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.file_offset = u64_value(protocol::Attribute::FileOffset);
        operation.length = u64_value(protocol::Attribute::Size);
        return operation;
    }
    case protocol::Command::Fallocate:
    {
        FallocateOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.mode = u32_value(protocol::Attribute::FallocateMode);
        operation.file_offset = u64_value(protocol::Attribute::FileOffset);
        operation.length = u64_value(protocol::Attribute::Size);
        return operation;
    }
    case protocol::Command::Fileattr:
    {
        FileattrOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.fileattr = u64_value(protocol::Attribute::Fileattr);
        return operation;
    }
    case protocol::Command::EncodedWrite:
    {
        EncodedWriteOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.file_offset = u64_value(protocol::Attribute::FileOffset);
        operation.unencoded_file_len = u64_value(protocol::Attribute::UnencodedFileLen);
        operation.unencoded_len = u64_value(protocol::Attribute::UnencodedLen);
        operation.unencoded_offset = u64_value(protocol::Attribute::UnencodedOffset);
        //Both default to NONE when the sender leaves them out.
        if (const std::optional<std::uint32_t> compression = optional_u32(protocol::Attribute::Compression))
            operation.compression = *compression;
        if (const std::optional<std::uint32_t> encryption = optional_u32(protocol::Attribute::Encryption))
            operation.encryption = *encryption;
        operation.data = binary_value(protocol::Attribute::Data);
        return operation;
    }
    case protocol::Command::EnableVerity:
    {
        EnableVerityOperation operation;
        operation.path = std::string(string_value(protocol::Attribute::Path));
        operation.algorithm = u8_value(protocol::Attribute::VerityAlgorithm);
        operation.block_size = u32_value(protocol::Attribute::VerityBlockSize);
        operation.salt = binary_value(protocol::Attribute::VeritySaltData);
        operation.signature = binary_value(protocol::Attribute::VeritySigData);
        return operation;
    }
    case protocol::Command::Unspec:
        break;
    }

    fail(ErrorCode::UnknownCommand, std::string("command id ") + std::to_string(static_cast<std::uint16_t>(m_Command)) + " is not defined");
}

//Decodes every TLV of the command payload into m_Attributes equivalents for the current command.
Operation decode_operation(protocol::Command command, std::uint32_t version, const Parser::Options& options, const std::vector<protocol::RawAttribute>& attributes, std::uint64_t command_index, std::uint64_t payload_offset)
{
    Decoder decoder(command, version, options, attributes, command_index, payload_offset);
    return decoder.run();
}

}

Parser::Parser(ByteSource& source)
    : Parser(source, Options{})
{
}

Parser::Parser(ByteSource& source, Options options)
    : m_Source(&source)
    , m_Options(options)
{
}

const Parser::Options& Parser::options() const noexcept
{
    return m_Options;
}

bool Parser::header_parsed() const noexcept
{
    return m_State != State::StreamHeader;
}

bool Parser::finished() const noexcept
{
    return m_State == State::Finished;
}

std::uint32_t Parser::stream_version() const noexcept
{
    return m_Version;
}

std::uint64_t Parser::bytes_consumed() const noexcept
{
    return m_BytesConsumed;
}

std::uint64_t Parser::commands_parsed() const noexcept
{
    return m_CommandsParsed;
}

void Parser::fail(ErrorCode code, std::string message, std::uint64_t offset) const
{
    throw ParseFailure{code, std::move(message), offset, m_CurrentCommandIndex};
}

void Parser::fail_for_command(ErrorCode code, std::string message) const
{
    fail(code, std::move(message), m_BytesConsumed);
}

std::size_t Parser::read_some(std::byte* out, std::size_t size)
{
    std::size_t got = 0;
    try
    {
        got = m_Source->read(out, size);
    }
    catch (const ParseError& error)
    {
        throw ParseFailure{ErrorCode::SourceReadFailed, std::string("byte source failed: ") + error.what(), m_BytesConsumed, m_CurrentCommandIndex};
    }
    catch (const std::exception& error)
    {
        throw ParseFailure{ErrorCode::SourceReadFailed, std::string("byte source failed: ") + error.what(), m_BytesConsumed, m_CurrentCommandIndex};
    }
    if (got > size)
        fail_for_command(ErrorCode::SourceReadFailed, "byte source returned more bytes than requested");
    m_BytesConsumed += got;
    return got;
}

void Parser::read_payload(std::uint32_t size, std::uint32_t* crc)
{
    m_Payload.clear();
    std::uint32_t have = 0;
    while (have < size)
    {
        const auto request = static_cast<std::size_t>(std::min<std::uint32_t>(size - have, static_cast<std::uint32_t>(kReadChunkSize)));
        const std::size_t needed = have + request;
        if (m_Payload.capacity() < needed)
            m_Payload.reserve(std::max(needed, m_Payload.capacity() * 2 + kPayloadGrowthSlack));
        m_Payload.resize(needed);

        const std::size_t got = read_some(m_Payload.data() + have, request);
        if (got == 0)
        {
            throw ParseFailure{ErrorCode::UnexpectedEndOfStream, "input ended inside a command payload: got " + std::to_string(have) + " of " + std::to_string(size) + " bytes", m_BytesConsumed, m_CurrentCommandIndex};
        }
        have += static_cast<std::uint32_t>(got);
        if (crc != nullptr)
            *crc = crc32c(*crc, m_Payload.data() + have - got, got);
    }
    m_Payload.resize(have);
}

void Parser::read_stream_header()
{
    if (!m_Options.expect_stream_header)
    {
        m_Version = m_Options.assumed_stream_version;
        if (m_Version < protocol::kMinStreamVersion || m_Version > protocol::kMaxStreamVersion)
            fail(ErrorCode::UnsupportedVersion, "assumed stream version " + std::to_string(m_Version) + " is not supported, this parser implements versions 1 to " + std::to_string(protocol::kMaxStreamVersion), 0);
        m_CommandsInStream = 0;
        m_State = State::Commands;
        return;
    }

    std::array<std::byte, protocol::kStreamHeaderSize> header{};

    //No bytes at all is an empty input, not a truncated header.
    std::size_t have = read_some(header.data(), header.size());
    if (have == 0)
        fail(ErrorCode::InvalidStreamHeader, "input is empty, a send stream starts with the magic and the version", 0);
    while (have < header.size())
    {
        const std::size_t got = read_some(header.data() + have, header.size() - have);
        if (got == 0)
            fail(ErrorCode::InvalidStreamHeader, "input ended inside the stream header", m_BytesConsumed);
        have += got;
    }

    if (std::memcmp(header.data(), protocol::kStreamMagic, protocol::kStreamMagicFieldSize) != 0)
    {
        fail(ErrorCode::InvalidMagic, "stream does not start with \"btrfs-stream\"", 0);
    }

    const std::uint32_t version = protocol::read_le32(header.data() + protocol::kStreamMagicFieldSize);
    if (version < protocol::kMinStreamVersion || version > protocol::kMaxStreamVersion)
    {
        fail(ErrorCode::UnsupportedVersion, "stream version " + std::to_string(version) + " is not supported, this parser implements versions " + std::to_string(protocol::kMinStreamVersion) + " to " + std::to_string(protocol::kMaxStreamVersion), 0);
    }

    m_Version = version;
    m_CommandsInStream = 0;
    m_State = State::Commands;
}

bool Parser::is_embedded_stream_header(const std::array<std::byte, protocol::kCommandHeaderSize>& header) noexcept
{
    //"btrfs-str" cannot start a real command: it would mean a payload length of 0x66727462 together with
    //the command id 0x7372, which no stream version defines.
    return std::memcmp(header.data(), protocol::kStreamMagic, protocol::kCommandHeaderSize) == 0 && protocol::read_le16(header.data() + protocol::kCommandTypeFieldOffset) > protocol::kMaxCommandId;
}

bool Parser::read_command(SendOperation& operation)
{
    std::array<std::byte, protocol::kCommandHeaderSize> header{};
    std::size_t have = 0;

    for (;;)
    {
        //A clean end of input is only legal on a command boundary.
        have = read_some(header.data(), header.size());
        if (have == 0)
            return false;
        m_CommandOffset = m_BytesConsumed - have;
        m_CurrentCommandIndex = m_CommandsParsed + 1;

        while (have < header.size())
        {
            const std::size_t got = read_some(header.data() + have, header.size() - have);
            if (got == 0)
                fail(ErrorCode::UnexpectedEndOfStream, "input ended inside a command header: got " + std::to_string(have) + " of " + std::to_string(header.size()) + " bytes", m_CommandOffset);
            have += got;
        }

        if (!is_embedded_stream_header(header))
            break;

        if (!m_Options.allow_concatenated_streams)
        {
            fail(ErrorCode::UnknownCommand, "found a second stream header at offset " + std::to_string(m_CommandOffset) + ", concatenated streams are disabled (pass Options::allow_concatenated_streams to accept them)", m_CommandOffset);
        }

        //Read the rest of the stream header and continue with the commands of the next stream.
        std::array<std::byte, protocol::kStreamHeaderSize> stream_header{};
        std::copy(header.begin(), header.end(), stream_header.begin());
        std::size_t stream_have = header.size();
        while (stream_have < stream_header.size())
        {
            const std::size_t got = read_some(stream_header.data() + stream_have, stream_header.size() - stream_have);
            if (got == 0)
                fail(ErrorCode::InvalidStreamHeader, "input ended inside a stream header", m_BytesConsumed);
            stream_have += got;
        }
        if (std::memcmp(stream_header.data(), protocol::kStreamMagic, protocol::kStreamMagicFieldSize) != 0)
            fail(ErrorCode::InvalidMagic, "concatenated stream header does not start with \"btrfs-stream\"", m_CommandOffset);

        const std::uint32_t version = protocol::read_le32(stream_header.data() + protocol::kStreamMagicFieldSize);
        if (version < protocol::kMinStreamVersion || version > protocol::kMaxStreamVersion)
        {
            fail(ErrorCode::UnsupportedVersion, "stream version " + std::to_string(version) + " is not supported, this parser implements versions " + std::to_string(protocol::kMinStreamVersion) + " to " + std::to_string(protocol::kMaxStreamVersion), m_CommandOffset);
        }
        m_Version = version;
        m_CommandsInStream = 0;
    }

    const std::uint32_t payload_size = protocol::read_le32(header.data() + protocol::kCommandLengthFieldOffset);
    const auto command_id = static_cast<protocol::Command>(protocol::read_le16(header.data() + protocol::kCommandTypeFieldOffset));
    const std::uint32_t checksum = protocol::read_le32(header.data() + protocol::kCommandCrcFieldOffset);

    if (payload_size > m_Options.max_command_size)
    {
        fail(ErrorCode::SizeOverflow, "command " + std::to_string(static_cast<std::uint16_t>(command_id)) + " declares a payload of " + std::to_string(payload_size) + " bytes, the limit is " + std::to_string(m_Options.max_command_size), m_CommandOffset);
    }

    //The checksum covers the header with the crc field zeroed, followed by the payload.
    std::uint32_t computed_crc = 0;
    if (m_Options.verify_checksums)
    {
        std::array<std::byte, protocol::kCommandHeaderSize> checksum_header = header;
        std::fill(checksum_header.begin() + protocol::kCommandCrcFieldOffset, checksum_header.begin() + protocol::kCommandCrcFieldOffset + 4, std::byte{0});
        computed_crc = crc32c(0, checksum_header.data(), checksum_header.size());
    }
    read_payload(payload_size, m_Options.verify_checksums ? &computed_crc : nullptr);

    if (m_Options.verify_checksums && computed_crc != checksum)
    {
        fail(ErrorCode::ChecksumMismatch, "crc32c mismatch: stream says 0x" + hex32(checksum) + ", the payload computes to 0x" + hex32(computed_crc), m_CommandOffset);
    }

    operation.command = command_id;
    operation.index = m_CurrentCommandIndex;
    operation.stream_offset = m_CommandOffset;
    operation.payload_size = payload_size;
    operation.attributes.clear();
    operation.operation = Operation{EndOperation{}};

    if (!protocol::is_known_command(command_id))
    {
        if (!m_Options.allow_unknown_commands)
        {
            fail(ErrorCode::UnknownCommand, "command id " + std::to_string(static_cast<std::uint16_t>(command_id)) + " is not defined for stream version " + std::to_string(m_Version), m_CommandOffset);
        }
        UnknownCommandOperation unknown;
        unknown.command = command_id;
        unknown.payload = BinaryData::view(m_Payload.data(), m_Payload.size());
        operation.operation = std::move(unknown);
        m_CommandsParsed += 1;
        m_CommandsInStream += 1;
        return true;
    }

    if (m_Options.require_subvol_or_snapshot_first && m_CommandsInStream == 0 && command_id != protocol::Command::Subvol && command_id != protocol::Command::Snapshot)
    {
        fail(ErrorCode::InvalidCommandStructure, std::string("the first command of a stream must be subvol or snapshot, got ") + protocol::to_string(command_id), m_CommandOffset);
    }

    parse_attributes();

    for (const protocol::RawAttribute& attribute : m_Attributes)
        operation.attributes.push_back(AttributeValue{attribute.type, BinaryData::view(attribute.data, attribute.size)});

    const std::uint64_t payload_offset = m_CommandOffset + protocol::kCommandHeaderSize;
    operation.operation = decode_operation(command_id, m_Version, m_Options, m_Attributes, m_CurrentCommandIndex, payload_offset);

    m_CommandsParsed += 1;
    m_CommandsInStream += 1;
    return true;
}

void Parser::parse_attributes()
{
    m_Attributes.clear();
    const auto payload_size = static_cast<std::uint32_t>(m_Payload.size());
    std::uint32_t position = 0;

    while (position < payload_size)
    {
        const std::uint32_t remaining = payload_size - position;
        if (remaining < protocol::kAttributeHeaderSize)
        {
            fail_for_command(ErrorCode::MalformedAttribute, "truncated attribute header: " + std::to_string(remaining) + " byte(s) left in the payload");
        }

        const std::uint16_t type_id = protocol::read_le16(m_Payload.data() + position);
        const std::uint32_t type_offset = position;
        position += 2;

        if (type_id == static_cast<std::uint16_t>(protocol::Attribute::Unspec))
        {
            fail(ErrorCode::MalformedAttribute, "attribute type 0 is not valid", m_CommandOffset + protocol::kCommandHeaderSize + type_offset);
        }

        const auto type = static_cast<protocol::Attribute>(type_id);
        std::uint32_t value_size = 0;

        if (m_Version >= protocol::kVersionEncodedIo && type == protocol::Attribute::Data)
        {
            //Stream version 2 and later: DATA has no length field, it extends to the end of the payload.
            value_size = payload_size - position;
        }
        else
        {
            if (type_id > protocol::kMaxAttributeType && !m_Options.allow_unknown_attribute_types)
            {
                fail(ErrorCode::UnknownAttributeType, "attribute type " + std::to_string(type_id) + " is not defined (pass Options::allow_unknown_attribute_types to skip it)", m_CommandOffset + protocol::kCommandHeaderSize + type_offset);
            }
            value_size = protocol::read_le16(m_Payload.data() + position);
            position += 2;
        }

        if (value_size > payload_size - position)
        {
            fail(ErrorCode::MalformedAttribute, std::string("attribute ") + protocol::to_string(type) + " claims " + std::to_string(value_size) + " bytes but only " + std::to_string(payload_size - position) + " byte(s) are left in the payload", m_CommandOffset + protocol::kCommandHeaderSize + position);
        }

        protocol::RawAttribute attribute;
        attribute.type = type;
        attribute.data = m_Payload.data() + position;
        attribute.size = value_size;
        attribute.payload_offset = position;
        m_Attributes.push_back(attribute);

        position += value_size;
    }
}

NextStatus Parser::step(SendOperation& operation)
{
    if (m_State == State::StreamHeader)
        read_stream_header();
    if (m_State == State::Finished)
        return NextStatus::EndOfStream;

    if (!read_command(operation))
    {
        m_State = State::Finished;
        return NextStatus::EndOfStream;
    }

    if (m_Options.stop_after_end_command && operation.command == protocol::Command::End)
        m_State = State::Finished;

    return NextStatus::HaveOperation;
}

NextStatus Parser::next(SendOperation& operation, ParseFailure& failure)
{
    if (m_State == State::Finished)
        return NextStatus::EndOfStream;

    try
    {
        return step(operation);
    }
    catch (const ParseFailure& parse_failure)
    {
        failure = parse_failure;
        m_State = State::Finished;
        return NextStatus::Error;
    }
}

std::optional<SendOperation> Parser::next()
{
    SendOperation operation;
    ParseFailure failure;
    switch (next(operation, failure))
    {
    case NextStatus::HaveOperation:
        return std::optional<SendOperation>(std::move(operation));
    case NextStatus::EndOfStream:
        return std::nullopt;
    case NextStatus::Error:
        break;
    }
    throw ParseError(std::move(failure));
}

}
