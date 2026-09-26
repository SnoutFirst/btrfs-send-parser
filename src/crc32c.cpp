#include "btrfs/send/crc32c.hpp"

#include <array>

namespace btrfs::send {

namespace {

//Reflected form of 0x1EDC6F41.
constexpr std::uint32_t kCastagnoliPolynomial = 0x82F63B78u;

using Table = std::array<std::uint32_t, 256>;

constexpr Table make_table()
{
    Table table{};
    for (std::uint32_t index = 0; index < table.size(); ++index)
    {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1u) != 0 ? (value >> 1) ^ kCastagnoliPolynomial : value >> 1;
        table[index] = value;
    }
    return table;
}

constexpr Table s_Table = make_table();

}

std::uint32_t crc32c(std::uint32_t seed, const void* data, std::size_t size) noexcept
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = seed;
    for (std::size_t index = 0; index < size; ++index)
        crc = (crc >> 8) ^ s_Table[(crc ^ bytes[index]) & 0xffu];
    return crc;
}

std::uint32_t crc32c_send_stream(const void* data, std::size_t size) noexcept
{
    return crc32c(0, data, size);
}

std::uint32_t crc32c_standard(const void* data, std::size_t size) noexcept
{
    return crc32c(0xffffffffu, data, size) ^ 0xffffffffu;
}

}
