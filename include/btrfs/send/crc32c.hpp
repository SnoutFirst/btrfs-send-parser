#pragma once

//CRC32C (Castagnoli, polynomial 0x1EDC6F41) as used by the send stream.
//
//The send stream does not use the CRC32C convention from RFC 3720: there is no initial inversion and no
//final inversion, the seed is passed straight in and the raw register value is compared against the stream.
//The implementation here is table driven, self contained and has no external dependency.

#include <cstddef>
#include <cstdint>

namespace btrfs::send {

//Continues a CRC32C computation: pass the result of the previous call as seed, or 0 to start.
std::uint32_t crc32c(std::uint32_t seed, const void* data, std::size_t size) noexcept;

//CRC32C with the no-inversion convention used by the send stream format (equivalent to crc32c(0, ...)).
std::uint32_t crc32c_send_stream(const void* data, std::size_t size) noexcept;

//The RFC 3720 / iSCSI convention (initial and final inversion applied), for tests and for callers that
//need to talk to something that expects the standard variant.
std::uint32_t crc32c_standard(const void* data, std::size_t size) noexcept;

}
