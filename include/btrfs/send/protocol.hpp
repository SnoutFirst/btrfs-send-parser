#pragma once

//Wire level description of the Btrfs send stream format.
//
//Layout (all integers little endian):
//
//  stream header : "btrfs-stream" + NUL, then u32 version
//  command       : u32 length (payload only), u16 command, u32 crc32c
//  payload       : sequence of attributes
//  attribute     : u16 type, u16 length, then length bytes of value
//
//The only exception to the last rule is the DATA attribute (type 19) in stream version 2 and later:
//it carries no length field and instead extends to the end of the command payload, so it must be the last attribute.
//
//The CRC32C covers the 10 command header bytes with the crc field replaced by zero, followed by the payload.
//The checksum uses a seed of 0 and no final inversion.

#include <cstddef>
#include <cstdint>

namespace btrfs::send::protocol {

//"btrfs-stream" plus the terminating NUL byte, which is part of the stream.
inline constexpr const char kStreamMagic[] = "btrfs-stream";
inline constexpr std::size_t kStreamMagicFieldSize = 13;
inline constexpr std::size_t kVersionFieldSize = 4;
inline constexpr std::size_t kStreamHeaderSize = kStreamMagicFieldSize + kVersionFieldSize;

inline constexpr std::size_t kCommandHeaderSize = 10;   //u32 length + u16 command + u32 crc
inline constexpr std::size_t kCommandLengthFieldOffset = 0;
inline constexpr std::size_t kCommandTypeFieldOffset = 4;
inline constexpr std::size_t kCommandCrcFieldOffset = 6;
inline constexpr std::size_t kAttributeHeaderSize = 4;  //u16 type + u16 length

//Stream versions.
//  1: original format, attribute lengths are 16 bit
//  2: DATA becomes a length-less trailing attribute, adds fallocate/fileattr/encoded_write
//  3: adds enable_verity (still marked experimental upstream)
inline constexpr std::uint32_t kMinStreamVersion = 1;
inline constexpr std::uint32_t kMaxStreamVersion = 3;
inline constexpr std::uint32_t kVersionEncodedIo = 2;
inline constexpr std::uint32_t kVersionVerity = 3;

//Highest attribute type defined by protocol version 3.
inline constexpr std::uint16_t kMaxAttributeType = 35;

//Safety limit for a single command payload. Stream version 2 allows lengths up to 4GiB in theory,
//but the kernel never emits more than a send buffer (16KiB + the compressed extent size, well under 1MiB).
//Callers that know their producer can raise or lower this through Parser::Options.
inline constexpr std::uint32_t kDefaultMaxCommandSize = 64u * 1024u * 1024u;

//Command ids as they appear in the stream.
enum class Command : std::uint16_t
{
    Unspec = 0,
    Subvol = 1,
    Snapshot = 2,
    Mkfile = 3,
    Mkdir = 4,
    Mknod = 5,
    Mkfifo = 6,
    Mksock = 7,
    Symlink = 8,
    Rename = 9,
    Link = 10,
    Unlink = 11,
    Rmdir = 12,
    SetXattr = 13,
    RemoveXattr = 14,
    Write = 15,
    Clone = 16,
    Truncate = 17,
    Chmod = 18,
    Chown = 19,
    Utimes = 20,
    End = 21,
    UpdateExtent = 22,
    Fallocate = 23,
    Fileattr = 24,
    EncodedWrite = 25,
    EnableVerity = 26,
};

inline constexpr std::uint16_t kMaxCommandId = 26;

//Attribute (TLV) ids as they appear in a command payload.
enum class Attribute : std::uint16_t
{
    Unspec = 0,
    Uuid = 1,
    CtransId = 2,
    Ino = 3,
    Size = 4,
    Mode = 5,
    Uid = 6,
    Gid = 7,
    Rdev = 8,
    Ctime = 9,
    Mtime = 10,
    Atime = 11,
    Otime = 12,
    XattrName = 13,
    XattrData = 14,
    Path = 15,
    PathTo = 16,
    PathLink = 17,
    FileOffset = 18,
    Data = 19,
    CloneUuid = 20,
    CloneCtransId = 21,
    ClonePath = 22,
    CloneOffset = 23,
    CloneLen = 24,
    FallocateMode = 25,
    Fileattr = 26,
    UnencodedFileLen = 27,
    UnencodedLen = 28,
    UnencodedOffset = 29,
    Compression = 30,
    Encryption = 31,
    VerityAlgorithm = 32,
    VerityBlockSize = 33,
    VeritySaltData = 34,
    VeritySigData = 35,
};

//Lowest stream version that may contain the command.
constexpr std::uint32_t minimum_version(Command command) noexcept
{
    switch (command)
    {
    case Command::Fallocate:
    case Command::Fileattr:
    case Command::EncodedWrite:
        return kVersionEncodedIo;
    case Command::EnableVerity:
        return kVersionVerity;
    default:
        return kMinStreamVersion;
    }
}

constexpr bool is_known_command(Command command) noexcept
{
    return command >= Command::Subvol && command <= static_cast<Command>(kMaxCommandId);
}

constexpr bool is_known_attribute(Attribute attribute) noexcept
{
    return attribute >= Attribute::Uuid && static_cast<std::uint16_t>(attribute) <= kMaxAttributeType;
}

//Name of the command as used by the dump tool ("update_extent", "encoded_write", ...). Unknown ids yield "unknown".
const char* to_string(Command command) noexcept;

//Name of the attribute in the spelling of the protocol documentation ("ctransid", "xattr_data", ...).
const char* to_string(Attribute attribute) noexcept;

//Frame of the stream: magic plus version.
struct StreamHeader
{
    std::uint32_t version = 0;
};

//Frame of a command.
struct CommandHeader
{
    std::uint32_t payload_size = 0;
    Command command = Command::Unspec;
    std::uint32_t crc = 0;
};

//One attribute after the TLV header was decoded, before it is interpreted as a value.
//payload_offset is relative to the start of the command payload and is only used for diagnostics.
struct RawAttribute
{
    Attribute type = Attribute::Unspec;
    const std::byte* data = nullptr;
    std::size_t size = 0;
    std::uint32_t payload_offset = 0;
};

//Little endian decoding helpers. They take raw bytes so no alignment or aliasing assumptions are needed.
constexpr std::uint16_t read_le16(const std::byte* data) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8));
}

constexpr std::uint32_t read_le32(const std::byte* data) noexcept
{
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) | (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

constexpr std::uint64_t read_le64(const std::byte* data) noexcept
{
    return static_cast<std::uint64_t>(read_le32(data)) | (static_cast<std::uint64_t>(read_le32(data + 4)) << 32);
}

}
