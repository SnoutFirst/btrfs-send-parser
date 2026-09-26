#include "framework.hpp"

#include <fstream>
#include <string>
#include <vector>

#include "btrfs/send/crc32c.hpp"
#include "stream_builder.hpp"

using namespace btrfs::send;

namespace {

std::string read_file(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}

TEST_CASE(crc32c_standard_test_vector)
{
    //The check value every CRC catalogue lists for CRC-32C.
    CHECK_EQ(crc32c_standard("123456789", 9), 0xe3069283u);
    CHECK_EQ(crc32c_standard("", 0), 0u);
}

TEST_CASE(crc32c_send_stream_convention)
{
    //No initial inversion and no final inversion, which is what the send stream uses.
    CHECK_EQ(crc32c_send_stream("123456789", 9), 0x58e3fa20u);
    CHECK_EQ(crc32c_send_stream("", 0), 0u);
    CHECK_EQ(crc32c(0xffffffffu, "123456789", 9), 0x1cf96d7cu);
    CHECK_NE(crc32c_send_stream("123456789", 9), crc32c_standard("123456789", 9));
}

TEST_CASE(crc32c_is_incremental)
{
    const std::string text = "0123456789";
    const std::uint32_t whole = crc32c(0, text.data(), text.size());
    const std::uint32_t first = crc32c(0, text.data(), 4);
    const std::uint32_t continued = crc32c(first, text.data() + 4, text.size() - 4);
    CHECK_EQ(continued, whole);
    CHECK_EQ(crc32c(first, text.data() + 4, 6), whole);

    //Byte at a time must agree with the whole buffer as well.
    std::uint32_t crc = 0;
    for (const char character : text)
        crc = crc32c(crc, &character, 1);
    CHECK_EQ(crc, whole);
}

TEST_CASE(crc32c_matches_the_builder)
{
    //The tests' own bitwise implementation and the library's table driven one have to agree, otherwise
    //synthetic streams would test the parser against the wrong checksums.
    for (const std::string& sample : {std::string(""), std::string("a"), std::string("123456789"), std::string(300, 'x')})
    {
        CHECK_EQ(crc32c(0, sample.data(), sample.size()), testfw::reference_crc32c(0, sample));
    }
}

TEST_CASE(crc32c_matches_real_btrfs_commands)
{
    //Walks a stream produced by the installed btrfs-progs and recomputes every command checksum.
    for (const char* name : {"full-v1.bin", "incremental-v2.bin", "nodata-v2.bin", "compressed-v2.bin"})
    {
        const std::string data = read_file(std::string(BTRFS_SEND_PARSER_FIXTURE_DIR) + "/" + name);
        CHECK(!data.empty());
        std::size_t offset = protocol::kStreamHeaderSize;
        std::size_t checked = 0;
        while (offset + protocol::kCommandHeaderSize <= data.size())
        {
            const auto* header = reinterpret_cast<const std::byte*>(data.data() + offset);
            const std::uint32_t payload_size = protocol::read_le32(header + protocol::kCommandLengthFieldOffset);
            const std::uint32_t stored = protocol::read_le32(header + protocol::kCommandCrcFieldOffset);
            CHECK(offset + protocol::kCommandHeaderSize + payload_size <= data.size());

            std::string header_without_crc(data, offset, protocol::kCommandHeaderSize);
            header_without_crc.replace(protocol::kCommandCrcFieldOffset, 4, std::string(4, '\0'));
            const std::uint32_t computed = crc32c(0, header_without_crc.data(), header_without_crc.size());
            const std::uint32_t continued = crc32c(computed, data.data() + offset + protocol::kCommandHeaderSize, payload_size);
            CHECK_MSG(continued == stored, std::string("command ") + std::to_string(checked) + " of " + name);

            offset += protocol::kCommandHeaderSize + payload_size;
            ++checked;
        }
        CHECK(checked > 0);
    }
}
