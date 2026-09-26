#pragma once

//Builds send streams byte by byte, so the parser can be tested against commands, attributes and
//malformations that a real kernel will not produce on demand.
//
//The builder is deliberately independent of the library: the checksum comes from a bitwise CRC32C written
//for the tests, and the byte layout is assembled here rather than through any parser helper.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "btrfs/send/protocol.hpp"
#include "btrfs/send/types.hpp"

namespace testfw {

//Straightforward bit-at-a-time CRC32C with the send stream convention (seed passed in, no inversion).
inline std::uint32_t reference_crc32c(std::uint32_t seed, std::string_view data)
{
    std::uint32_t crc = seed;
    for (const char character : data)
    {
        crc ^= static_cast<unsigned char>(character);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
    }
    return crc;
}

inline void append_le16(std::string& out, std::uint16_t value)
{
    out.push_back(static_cast<char>(value & 0xffu));
    out.push_back(static_cast<char>((value >> 8) & 0xffu));
}

inline void append_le32(std::string& out, std::uint32_t value)
{
    for (int index = 0; index < 4; ++index)
        out.push_back(static_cast<char>((value >> (index * 8)) & 0xffu));
}

inline void append_le64(std::string& out, std::uint64_t value)
{
    for (int index = 0; index < 8; ++index)
        out.push_back(static_cast<char>((value >> (index * 8)) & 0xffu));
}

class StreamBuilder
{
public:
    explicit StreamBuilder(std::uint32_t version = btrfs::send::protocol::kMinStreamVersion)
    {
        append_stream_header(version);
    }

    //A command sequence with no "btrfs-stream" header in front of it.
    static StreamBuilder headerless(std::uint32_t version = btrfs::send::protocol::kMinStreamVersion)
    {
        StreamBuilder builder;
        builder.m_Bytes.clear();
        builder.m_CommandOffsets.clear();
        builder.m_Version = version;
        builder.m_WithHeader = false;
        return builder;
    }

    //Appends another complete stream (header included) to the bytes built so far.
    void append_stream_header(std::uint32_t version)
    {
        m_Bytes.append(btrfs::send::protocol::kStreamMagic, btrfs::send::protocol::kStreamMagicFieldSize);
        append_le32(m_Bytes, version);
        m_Version = version;
        m_WithHeader = true;
    }

    std::uint32_t version() const
    {
        return m_Version;
    }

    void begin_command(btrfs::send::protocol::Command command)
    {
        m_Command = command;
        m_Payload.clear();
    }

    void end_command()
    {
        finish_command(static_cast<std::uint32_t>(m_Payload.size()), m_Command, m_Payload, true);
    }

    //Finishes a command with a length field that need not match the payload actually written.
    void end_command_with_declared_length(std::uint32_t declared_length)
    {
        finish_command(declared_length, m_Command, m_Payload, true);
    }

    void put_u8(btrfs::send::protocol::Attribute attribute, std::uint8_t value)
    {
        put_raw_attribute(static_cast<std::uint16_t>(attribute), std::string(1, static_cast<char>(value)));
    }

    void put_u16(btrfs::send::protocol::Attribute attribute, std::uint16_t value)
    {
        std::string value_bytes;
        append_le16(value_bytes, value);
        put_raw_attribute(static_cast<std::uint16_t>(attribute), value_bytes);
    }

    void put_u32(btrfs::send::protocol::Attribute attribute, std::uint32_t value)
    {
        std::string value_bytes;
        append_le32(value_bytes, value);
        put_raw_attribute(static_cast<std::uint16_t>(attribute), value_bytes);
    }

    void put_u64(btrfs::send::protocol::Attribute attribute, std::uint64_t value)
    {
        std::string value_bytes;
        append_le64(value_bytes, value);
        put_raw_attribute(static_cast<std::uint16_t>(attribute), value_bytes);
    }

    void put_u64_raw(btrfs::send::protocol::Attribute attribute, std::uint64_t value, std::uint16_t declared_length)
    {
        std::string value_bytes;
        append_le64(value_bytes, value);
        put_malformed_attribute(static_cast<std::uint16_t>(attribute), declared_length, value_bytes);
    }

    void put_uuid(btrfs::send::protocol::Attribute attribute, const btrfs::send::Uuid& uuid)
    {
        std::string value_bytes(uuid.bytes.begin(), uuid.bytes.end());
        put_raw_attribute(static_cast<std::uint16_t>(attribute), value_bytes);
    }

    void put_string(btrfs::send::protocol::Attribute attribute, std::string_view value)
    {
        put_raw_attribute(static_cast<std::uint16_t>(attribute), value);
    }

    void put_bytes(btrfs::send::protocol::Attribute attribute, std::string_view value)
    {
        put_raw_attribute(static_cast<std::uint16_t>(attribute), value);
    }

    void put_timespec(btrfs::send::protocol::Attribute attribute, std::int64_t seconds, std::uint32_t nanoseconds)
    {
        std::string value_bytes;
        append_le64(value_bytes, static_cast<std::uint64_t>(seconds));
        append_le32(value_bytes, nanoseconds);
        put_raw_attribute(static_cast<std::uint16_t>(attribute), value_bytes);
    }

    //TLV with an explicit 16 bit length, which is what every attribute uses in stream version 1.
    void put_raw_attribute(std::uint16_t type, std::string_view value)
    {
        put_malformed_attribute(type, static_cast<std::uint16_t>(value.size()), value);
    }

    //TLV whose declared length need not match the bytes that follow.
    void put_malformed_attribute(std::uint16_t type, std::uint16_t declared_length, std::string_view value)
    {
        append_le16(m_Payload, type);
        append_le16(m_Payload, declared_length);
        m_Payload.append(value);
    }

    //TLV header without any value bytes.
    void put_attribute_header(std::uint16_t type, std::uint16_t declared_length)
    {
        append_le16(m_Payload, type);
        append_le16(m_Payload, declared_length);
    }

    //Two byte TLV header: attribute type but no length field.
    void put_truncated_attribute_header(std::uint16_t type)
    {
        append_le16(m_Payload, type);
    }

    //Stream version 2 and later: DATA carries no length, it runs to the end of the command payload.
    void put_lengthless_data(std::string_view data)
    {
        append_le16(m_Payload, static_cast<std::uint16_t>(btrfs::send::protocol::Attribute::Data));
        m_Payload.append(data);
    }

    void put_raw_payload(std::string_view payload)
    {
        m_Payload.append(payload);
    }

    //A complete command in one call, with control over the declared length and the checksum.
    void put_raw_command(std::uint32_t declared_length, btrfs::send::protocol::Command command, std::string_view payload, bool valid_checksum)
    {
        finish_command(declared_length, command, std::string(payload), valid_checksum);
    }

    const std::string& bytes() const
    {
        return m_Bytes;
    }

    std::vector<std::byte> byte_vector() const
    {
        std::vector<std::byte> result(m_Bytes.size());
        for (std::size_t index = 0; index < m_Bytes.size(); ++index)
            result[index] = static_cast<std::byte>(static_cast<unsigned char>(m_Bytes[index]));
        return result;
    }

    void truncate(std::size_t size)
    {
        if (size < m_Bytes.size())
            m_Bytes.resize(size);
    }

    //Flips a bit in the checksum field of the n-th command (0 based).
    void corrupt_checksum_of_command(std::size_t command_index)
    {
        const std::size_t offset = m_CommandOffsets.at(command_index) + btrfs::send::protocol::kCommandCrcFieldOffset;
        m_Bytes[offset] = static_cast<char>(static_cast<unsigned char>(m_Bytes[offset]) ^ 0x01u);
    }

    //Flips a bit in a payload byte of the n-th command (0 based).
    void corrupt_payload_of_command(std::size_t command_index)
    {
        const std::size_t offset = m_CommandOffsets.at(command_index) + btrfs::send::protocol::kCommandHeaderSize;
        m_Bytes[offset] = static_cast<char>(static_cast<unsigned char>(m_Bytes[offset]) ^ 0x01u);
    }

    std::size_t command_count() const
    {
        return m_CommandOffsets.size();
    }

    bool has_stream_header() const
    {
        return m_WithHeader;
    }

    void clear()
    {
        m_Bytes.clear();
        m_CommandOffsets.clear();
    }

private:
    void finish_command(std::uint32_t declared_length, btrfs::send::protocol::Command command, const std::string& payload, bool valid_checksum)
    {
        m_CommandOffsets.push_back(m_Bytes.size());

        std::string header;
        append_le32(header, declared_length);
        append_le16(header, static_cast<std::uint16_t>(command));
        append_le32(header, 0);
        //The checksum covers the header with a zero crc field, followed by the payload.
        const std::uint32_t checksum = valid_checksum ? reference_crc32c(0, header + payload) : 0xdeadbeefu;
        header.resize(btrfs::send::protocol::kCommandCrcFieldOffset);
        append_le32(header, checksum);

        m_Bytes.append(header);
        m_Bytes.append(payload);
        m_Payload.clear();
    }

    std::string m_Bytes;
    std::string m_Payload;
    std::vector<std::size_t> m_CommandOffsets;
    btrfs::send::protocol::Command m_Command = btrfs::send::protocol::Command::Unspec;
    std::uint32_t m_Version = btrfs::send::protocol::kMinStreamVersion;
    bool m_WithHeader = true;
};

//A UUID made of a repeated byte, handy for stream builders.
inline btrfs::send::Uuid make_uuid(std::uint8_t fill)
{
    btrfs::send::Uuid uuid;
    uuid.bytes.fill(fill);
    return uuid;
}

}
