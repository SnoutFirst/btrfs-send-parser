#include "btrfs/send/flags.hpp"

namespace btrfs::send::flags {

namespace {

struct InodeFlagName
{
    InodeFlag flag;
    const char* name;
};

constexpr InodeFlagName s_InodeFlagNames[] = {
    {InodeFlag::NoDataSum, "nodatasum"},
    {InodeFlag::NoDataCow, "nodatacow"},
    {InodeFlag::ReadOnly, "readonly"},
    {InodeFlag::NoCompress, "nocompress"},
    {InodeFlag::Prealloc, "prealloc"},
    {InodeFlag::Sync, "sync"},
    {InodeFlag::Immutable, "immutable"},
    {InodeFlag::Append, "append"},
    {InodeFlag::NoDump, "nodump"},
    {InodeFlag::NoAtime, "noatime"},
    {InodeFlag::DirSync, "dirsync"},
    {InodeFlag::Compress, "compress"},
};

struct FallocateModeName
{
    FallocateMode mode;
    const char* name;
};

constexpr FallocateModeName s_FallocateModeNames[] = {
    {FallocateMode::KeepSize, "keep_size"},
    {FallocateMode::PunchHole, "punch_hole"},
    {FallocateMode::NoHideStale, "no_hide_stale"},
    {FallocateMode::CollapseRange, "collapse_range"},
    {FallocateMode::ZeroRange, "zero_range"},
    {FallocateMode::InsertRange, "insert_range"},
    {FallocateMode::UnshareRange, "unshare_range"},
};

}

std::vector<std::string> inode_flag_names(std::uint64_t fileattr)
{
    std::vector<std::string> names;
    if (fileattr == 0)
        return names;

    std::uint64_t known = 0;
    for (const InodeFlagName& entry : s_InodeFlagNames)
    {
        const auto bit = static_cast<std::uint64_t>(entry.flag);
        known |= bit;
        if ((fileattr & bit) != 0)
            names.emplace_back(entry.name);
    }

    const std::uint64_t unknown = fileattr & ~known;
    for (unsigned int bit = 0; bit < 64; ++bit)
    {
        if ((unknown & (static_cast<std::uint64_t>(1) << bit)) != 0)
            names.push_back("unknown_bit_" + std::to_string(bit));
    }
    return names;
}

std::vector<std::string> fallocate_mode_names(std::uint32_t mode)
{
    std::vector<std::string> names;
    if (mode == static_cast<std::uint32_t>(FallocateMode::AllocateRange))
    {
        names.emplace_back("allocate_range");
        return names;
    }

    std::uint32_t known = 0;
    for (const FallocateModeName& entry : s_FallocateModeNames)
    {
        const auto bit = static_cast<std::uint32_t>(entry.mode);
        known |= bit;
        if ((mode & bit) != 0)
            names.emplace_back(entry.name);
    }

    const std::uint32_t unknown = mode & ~known;
    for (unsigned int bit = 0; bit < 32; ++bit)
    {
        if ((unknown & (static_cast<std::uint32_t>(1) << bit)) != 0)
            names.push_back("unknown_bit_" + std::to_string(bit));
    }
    return names;
}

const char* to_string(Compression compression) noexcept
{
    switch (compression)
    {
    case Compression::None:
        return "none";
    case Compression::Zlib:
        return "zlib";
    case Compression::Zstd:
        return "zstd";
    case Compression::Lzo_4K:
        return "lzo_4k";
    case Compression::Lzo_8K:
        return "lzo_8k";
    case Compression::Lzo_16K:
        return "lzo_16k";
    case Compression::Lzo_32K:
        return "lzo_32k";
    }
    return "unknown";
}

const char* to_string(Encryption encryption) noexcept
{
    switch (encryption)
    {
    case Encryption::None:
        return "none";
    }
    return "unknown";
}

}
