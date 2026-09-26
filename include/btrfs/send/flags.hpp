#pragma once

//Values that appear inside attributes, with the names the protocol documentation gives them.
//The parser does not interpret these values, it only carries them across; the names are here so callers
//(and the dump tool) do not have to reinvent them.

#include <cstdint>
#include <string>
#include <vector>

namespace btrfs::send::flags {

//BTRFS_SEND_A_FILEATTR carries the inode flags as stored in the Btrfs inode item, not the FS_*_FL set
//from the FS_IOC_SETFLAGS ioctl. BTRFS_INODE_FLAG_MASK is the set the kernel is willing to send.
enum class InodeFlag : std::uint64_t
{
    NoDataSum = 1u << 0,
    NoDataCow = 1u << 1,
    ReadOnly = 1u << 2,
    NoCompress = 1u << 3,
    Prealloc = 1u << 4,
    Sync = 1u << 5,
    Immutable = 1u << 6,
    Append = 1u << 7,
    NoDump = 1u << 8,
    NoAtime = 1u << 9,
    DirSync = 1u << 10,
    Compress = 1u << 11,
};

inline constexpr std::uint64_t kInodeFlagMask = 0xfffu;   //bits 0..11 are the flags the kernel sends

//Names of the bits set in fileattr, for example {"nodatacow", "immutable"}.
std::vector<std::string> inode_flag_names(std::uint64_t fileattr);

//BTRFS_SEND_A_FALLOCATE_MODE is passed through to fallocate(2) by the receiver, so the bits are the
//FALLOC_FL_* values from <linux/falloc.h>.
enum class FallocateMode : std::uint32_t
{
    AllocateRange = 0x00,
    KeepSize = 0x01,
    PunchHole = 0x02,
    NoHideStale = 0x04,
    CollapseRange = 0x08,
    ZeroRange = 0x10,
    InsertRange = 0x20,
    UnshareRange = 0x40,
};

std::vector<std::string> fallocate_mode_names(std::uint32_t mode);

//BTRFS_SEND_A_COMPRESSION for encoded writes.
enum class Compression : std::uint32_t
{
    None = 0,
    Zlib = 1,
    Zstd = 2,
    Lzo_4K = 3,
    Lzo_8K = 4,
    Lzo_16K = 5,
    Lzo_32K = 6,
};

//BTRFS_SEND_A_ENCRYPTION for encoded writes. Only None exists today.
enum class Encryption : std::uint32_t
{
    None = 0,
};

//Name of a compression id, "unknown" for ids this header does not know.
const char* to_string(Compression compression) noexcept;
const char* to_string(Encryption encryption) noexcept;

}
