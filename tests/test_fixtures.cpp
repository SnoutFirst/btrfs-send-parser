#include "framework.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "btrfs/send.hpp"

//The streams under tests/fixtures were produced by the installed kernel and btrfs-progs, not by this project:
//run tests/integration/gen-fixtures.sh on a btrfs filesystem to regenerate them. Everything asserted here is
//either a property of the file that the generator script proves, or a count that is cross checked against an
//independent walk over the raw framing in this file.
#ifndef BTRFS_SEND_PARSER_FIXTURE_DIR
#define BTRFS_SEND_PARSER_FIXTURE_DIR "."
#endif

using namespace btrfs::send;
using protocol::Attribute;
using protocol::Command;

namespace {

std::string read_fixture(const std::string& name)
{
    std::ifstream file(std::string(BTRFS_SEND_PARSER_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

//Shape of every fixture. The counts are corroborated by walk_framing() below, which reads the length fields
//directly and never touches the parser, so these numbers are not the parser grading its own homework.
struct FixtureShape
{
    const char* name;
    std::uint32_t version;
    std::size_t commands;
    std::size_t stream_headers;
    std::size_t subvolumes;   //SUBVOL plus SNAPSHOT
    std::size_t end_commands;
};

const FixtureShape kFixtureShapes[] = {
    {"clone-v2.bin", 2, 59, 1, 1, 1},
    {"compressed-v2.bin", 2, 6, 1, 1, 1},
    {"fallocate-fileattr-v2.bin", 2, 6, 1, 1, 1},
    {"full-v1.bin", 1, 118, 1, 1, 1},
    {"full-v2.bin", 2, 118, 1, 1, 1},
    {"incremental-v1.bin", 1, 58, 1, 1, 1},
    {"incremental-v2.bin", 2, 59, 1, 1, 1},
    {"minimal-v1.bin", 1, 5, 1, 1, 1},
    {"minimal-v2.bin", 2, 5, 1, 1, 1},
    {"multi-subvol-omit-end-v1.bin", 1, 122, 1, 2, 1},
    {"multi-subvol-v1.bin", 1, 123, 2, 2, 2},
    {"nodata-v1.bin", 1, 58, 1, 1, 1},
    {"nodata-v2.bin", 2, 59, 1, 1, 1},
};

//A second, independent view of the framing: walk the length fields by hand, so the parser's view can be
//compared against something that shares no code with it.
struct RawFraming
{
    std::vector<std::uint32_t> versions;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint32_t> payload_sizes;
    std::vector<std::uint16_t> command_ids;
};

bool walk_framing(const std::string& bytes, RawFraming& framing)
{
    const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
    std::size_t offset = 0;

    while (offset < bytes.size())
    {
        if (offset + protocol::kStreamHeaderSize <= bytes.size() && std::memcmp(bytes.data() + offset, protocol::kStreamMagic, protocol::kStreamMagicFieldSize) == 0)
        {
            framing.versions.push_back(protocol::read_le32(data + offset + protocol::kStreamMagicFieldSize));
            offset += protocol::kStreamHeaderSize;
            continue;
        }
        if (offset + protocol::kCommandHeaderSize > bytes.size())
            return false;

        const std::uint32_t payload_size = protocol::read_le32(data + offset + protocol::kCommandLengthFieldOffset);
        if (payload_size > bytes.size() - offset - protocol::kCommandHeaderSize)
            return false;

        framing.offsets.push_back(offset);
        framing.payload_sizes.push_back(payload_size);
        framing.command_ids.push_back(protocol::read_le16(data + offset + protocol::kCommandTypeFieldOffset));
        offset += protocol::kCommandHeaderSize + payload_size;
    }
    return offset == bytes.size();
}

struct ParsedFixture
{
    std::string bytes;
    SendStream stream;
    std::uint64_t bytes_consumed = 0;
};

//Strictest settings: the stream header is required and every checksum is verified.
ParsedFixture parse_fixture(const std::string& name)
{
    ParsedFixture parsed;
    parsed.bytes = read_fixture(name);

    MemorySource source(parsed.bytes);
    Parser parser(source);
    while (std::optional<SendOperation> operation = parser.next())
    {
        operation->materialize();
        parsed.stream.operations.push_back(std::move(*operation));
    }
    parsed.stream.version = parser.stream_version();
    parsed.bytes_consumed = parser.bytes_consumed();
    return parsed;
}

ParsedFixture parse_fixture_bytes(const std::string& bytes, Parser::Options options = {})
{
    ParsedFixture parsed;
    parsed.bytes = bytes;

    MemorySource source(parsed.bytes);
    Parser parser(source, options);
    while (std::optional<SendOperation> operation = parser.next())
    {
        operation->materialize();
        parsed.stream.operations.push_back(std::move(*operation));
    }
    parsed.stream.version = parser.stream_version();
    parsed.bytes_consumed = parser.bytes_consumed();
    return parsed;
}

template <typename T, typename Predicate>
const T* find_typed(const SendStream& stream, Predicate predicate)
{
    for (const SendOperation& operation : stream.operations)
    {
        if (const T* typed = operation.as<T>())
        {
            if (predicate(*typed))
                return typed;
        }
    }
    return nullptr;
}

const WriteOperation* find_write(const SendStream& stream, std::string_view path, std::uint64_t file_offset)
{
    return find_typed<WriteOperation>(stream, [path, file_offset](const WriteOperation& write) {
        return write.path == path && write.file_offset == file_offset;
    });
}

const SetXattrOperation* find_xattr(const SendStream& stream, std::string_view path, std::string_view name)
{
    return find_typed<SetXattrOperation>(stream, [path, name](const SetXattrOperation& xattr) {
        return xattr.path == path && xattr.name == name;
    });
}

std::size_t count_command(const SendStream& stream, Command command)
{
    return stream.count(command);
}

std::uint64_t total_payload_size(const SendStream& stream)
{
    std::uint64_t total = 0;
    for (const SendOperation& operation : stream.operations)
        total += operation.payload_size;
    return total;
}

//The names the generator script creates through make-weird-tree.py, in the order that script writes them.
const char* const kWeirdPaths[] = {
    "w e i r d/spaces in name.txt",
    "w e i r d/tab\tname.txt",
    "w e i r d/newline\nname.txt",
    "w e i r d/quote\"name.txt",
    "w e i r d/backslash\\name.txt",
    "w e i r d/non-utf8-\xff-name.txt",
    "w e i r d/utf8-\xc3\xa9\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt",
    "w e i r d/-leading-dash.txt",
    "w e i r d/single'quote.txt",
};

constexpr std::uint64_t kFileTypeMask = 0170000;
constexpr std::uint64_t kCharacterDevice = 0020000;
constexpr std::uint64_t kFifo = 0010000;
constexpr std::uint64_t kSocket = 0140000;

//Content the generator script puts into the src subvolume.
constexpr const char* kF1Content = "hello world\n";
constexpr std::uint64_t kSparseOffset = 16384 * 4096;   //dd bs=4096 seek=16384
constexpr std::uint64_t kSparseLength = 4096;
constexpr std::uint64_t kBigBinLength = 16384;          //head -c 16384
constexpr std::uint64_t kF1Mode = 0640;                 //chmod 640
constexpr std::uint64_t kF1Uid = 2;
constexpr std::uint64_t kF1Gid = 3;

//The four extents the generator script changes between snap1 and snap2.
struct ExpectedExtent
{
    const char* path;
    std::uint64_t offset;
    std::uint64_t length;
};

const ExpectedExtent kChangedExtents[] = {
    {"f1.txt", 0, 27},              //hello world (12) + more data here (15)
    {"sparse.bin", kSparseOffset, kSparseLength},
    {"d1/new.txt", 0, 100},         //truncate -s 100
    {"d2/holes.bin", 0, 32768},     //'x' * 32768
};

}

TEST_CASE(fixtures_agree_with_an_independent_walk_of_the_framing)
{
    for (const FixtureShape& shape : kFixtureShapes)
    {
        const std::string bytes = read_fixture(shape.name);
        CHECK_MSG(!bytes.empty(), shape.name);

        RawFraming raw;
        CHECK_MSG(walk_framing(bytes, raw), shape.name);
        CHECK_MSG(raw.command_ids.size() == shape.commands, shape.name);
        CHECK_MSG(raw.versions.size() == shape.stream_headers, shape.name);
        CHECK_MSG(raw.versions.back() == shape.version, shape.name);

        const ParsedFixture parsed = parse_fixture(shape.name);
        CHECK_MSG(parsed.stream.operations.size() == shape.commands, shape.name);
        CHECK_MSG(parsed.stream.version == shape.version, shape.name);
        //Every byte of the file has to be accounted for: no trailing garbage, no missed command.
        CHECK_MSG(parsed.bytes_consumed == bytes.size(), shape.name);

        std::size_t subvolumes = 0;
        std::size_t ends = 0;
        for (std::size_t index = 0; index < parsed.stream.operations.size(); ++index)
        {
            const SendOperation& operation = parsed.stream.operations[index];
            CHECK_MSG(operation.index == index + 1, shape.name);
            CHECK_MSG(static_cast<std::uint16_t>(operation.command) == raw.command_ids[index], shape.name);
            CHECK_MSG(operation.stream_offset == raw.offsets[index], shape.name);
            CHECK_MSG(operation.payload_size == raw.payload_sizes[index], shape.name);
            //Nothing in these streams is a command this parser has to guess about.
            CHECK_MSG(!operation.is<UnknownCommandOperation>(), shape.name);
            if (operation.command == Command::End)
                ++ends;
            if (operation.command == Command::Subvol || operation.command == Command::Snapshot)
                ++subvolumes;
        }
        CHECK_MSG(subvolumes == shape.subvolumes, shape.name);
        CHECK_MSG(ends == shape.end_commands, shape.name);

        //Every stream starts with subvol or snapshot and ends with END, as the kernel always writes it.
        CHECK_MSG(parsed.stream.operations.front().command == Command::Subvol || parsed.stream.operations.front().command == Command::Snapshot, shape.name);
        CHECK_MSG(parsed.stream.operations.back().command == Command::End, shape.name);
        CHECK_MSG(parsed.stream.operations.back().index == parsed.stream.operations.size(), shape.name);
    }
}

TEST_CASE(version_one_and_version_two_fixtures_carry_the_same_tree)
{
    const ParsedFixture version_one = parse_fixture("full-v1.bin");
    const ParsedFixture version_two = parse_fixture("full-v2.bin");

    CHECK_EQ(version_one.stream.operations.size(), version_two.stream.operations.size());
    for (std::size_t index = 0; index < version_one.stream.operations.size(); ++index)
    {
        const SendOperation& first = version_one.stream.operations[index];
        const SendOperation& second = version_two.stream.operations[index];
        CHECK_EQ(static_cast<std::uint16_t>(first.command), static_cast<std::uint16_t>(second.command));
        CHECK_EQ(operation_path(first), operation_path(second));
    }

    //Version 2 adds otime to UTIMES, which makes those payloads bigger and nothing else smaller.
    CHECK(total_payload_size(version_two.stream) > total_payload_size(version_one.stream));
    CHECK_EQ(count_command(version_one.stream, Command::Utimes), count_command(version_two.stream, Command::Utimes));
    CHECK_EQ(count_command(version_one.stream, Command::Utimes), std::size_t(20));
}

TEST_CASE(stream_version_decides_whether_utimes_carries_otime)
{
    for (const FixtureShape& shape : kFixtureShapes)
    {
        const ParsedFixture parsed = parse_fixture(shape.name);
        std::size_t utimes = 0;
        for (const SendOperation& operation : parsed.stream.operations)
        {
            const UtimesOperation* times = operation.as<UtimesOperation>();
            if (times == nullptr)
                continue;
            ++utimes;
            if (shape.version >= protocol::kVersionEncodedIo)
                CHECK_MSG(times->otime.has_value(), shape.name);
            else
                CHECK_MSG(!times->otime.has_value(), shape.name);
        }
        CHECK_MSG(utimes > 0, shape.name);
    }
}

TEST_CASE(minimal_streams_describe_an_empty_subvolume)
{
    for (const char* name : {"minimal-v1.bin", "minimal-v2.bin"})
    {
        const ParsedFixture parsed = parse_fixture(name);
        const SubvolOperation* subvol = find_typed<SubvolOperation>(parsed.stream, [](const SubvolOperation&) { return true; });
        CHECK(subvol != nullptr);
        CHECK_EQ(subvol->path, std::string("snap-minimal"));
        CHECK_EQ(subvol->ctrans_id, std::uint64_t(9));
        CHECK(!subvol->uuid.is_null());

        CHECK_EQ(count_command(parsed.stream, Command::Write), std::size_t(0));
        CHECK_EQ(count_command(parsed.stream, Command::Mkfile), std::size_t(0));
        CHECK_EQ(count_command(parsed.stream, Command::Mkdir), std::size_t(0));
        //The empty subvolume itself still gets its ownership, mode and times.
        CHECK_EQ(count_command(parsed.stream, Command::Chown), std::size_t(1));
        CHECK_EQ(count_command(parsed.stream, Command::Chmod), std::size_t(1));
        CHECK_EQ(count_command(parsed.stream, Command::Utimes), std::size_t(1));
    }
}

TEST_CASE(full_v1_data_and_metadata_match_the_generator_script)
{
    const ParsedFixture parsed = parse_fixture("full-v1.bin");
    const SendStream& stream = parsed.stream;

    const WriteOperation* f1 = find_write(stream, "f1.txt", 0);
    CHECK(f1 != nullptr);
    CHECK_EQ(f1->data.to_string(), std::string(kF1Content));

    const WriteOperation* sparse = find_write(stream, "sparse.bin", kSparseOffset);
    CHECK(sparse != nullptr);
    CHECK_EQ(sparse->data.size(), std::size_t(kSparseLength));

    const WriteOperation* big = find_write(stream, "big.bin", 0);
    CHECK(big != nullptr);
    CHECK_EQ(big->data.size(), std::size_t(kBigBinLength));

    const ChmodOperation* chmod = find_typed<ChmodOperation>(stream, [](const ChmodOperation& operation) { return operation.path == "f1.txt"; });
    CHECK(chmod != nullptr);
    CHECK_EQ(chmod->mode, kF1Mode);

    const ChownOperation* chown = find_typed<ChownOperation>(stream, [](const ChownOperation& operation) { return operation.path == "f1.txt"; });
    CHECK(chown != nullptr);
    CHECK_EQ(chown->uid, kF1Uid);
    CHECK_EQ(chown->gid, kF1Gid);

    //touch -d '2020-01-02 03:04:05' sets atime and mtime to the same instant, in the local time of the
    //machine that generated the fixture, so only the relationship and the year are asserted here.
    const UtimesOperation* utimes = find_typed<UtimesOperation>(stream, [](const UtimesOperation& operation) { return operation.path == "f1.txt"; });
    CHECK(utimes != nullptr);
    CHECK_EQ(utimes->atime, utimes->mtime);
    CHECK(utimes->atime.is_valid());
    CHECK_EQ(utimes->atime.to_iso8601_utc().substr(0, 4), std::string("2020"));
}

TEST_CASE(full_v1_hard_links_and_symlinks_match_the_generator_script)
{
    const SendStream& stream = parse_fixture("full-v1.bin").stream;

    //ln f1.txt hard.txt
    const LinkOperation* hard_link = find_typed<LinkOperation>(stream, [](const LinkOperation& operation) { return operation.path == "hard.txt"; });
    CHECK(hard_link != nullptr);
    CHECK_EQ(hard_link->link_target, std::string("f1.txt"));

    //ln -s f1.txt link.txt: the kernel creates the symlink under a temporary name and renames it, so the
    //final name shows up as a RENAME and not as the path of the SYMLINK command.
    const SymlinkOperation* symlink = find_typed<SymlinkOperation>(stream, [](const SymlinkOperation& operation) { return operation.link_target == "f1.txt"; });
    CHECK(symlink != nullptr);
    CHECK(!symlink->path.empty());

    const RenameOperation* rename = find_typed<RenameOperation>(stream, [](const RenameOperation& operation) { return operation.path_to == "link.txt"; });
    CHECK(rename != nullptr);
    CHECK_EQ(rename->path, symlink->path);
}

TEST_CASE(full_v1_special_files_carry_their_mode_and_device)
{
    const SendStream& stream = parse_fixture("full-v1.bin").stream;

    const MkfifoOperation* fifo = find_typed<MkfifoOperation>(stream, [](const MkfifoOperation& operation) { return operation.mode.has_value() && (*operation.mode & kFileTypeMask) == kFifo; });
    CHECK(fifo != nullptr);
    CHECK(fifo->rdev.has_value());
    CHECK_EQ(*fifo->rdev, std::uint64_t(0));

    //The socket and the character device are created after snap1, so they only appear in the incremental
    //streams. mknod d2/zero c 1 5 arrives with the device number the kernel encodes for it.
    const SendStream& incremental = parse_fixture("incremental-v2.bin").stream;

    const MksockOperation* socket = find_typed<MksockOperation>(incremental, [](const MksockOperation& operation) { return operation.mode.has_value() && (*operation.mode & kFileTypeMask) == kSocket; });
    CHECK(socket != nullptr);

    const MknodOperation* device = find_typed<MknodOperation>(incremental, [](const MknodOperation& operation) { return (operation.mode & kFileTypeMask) == kCharacterDevice; });
    CHECK(device != nullptr);
    CHECK_EQ(device->rdev, std::uint64_t(261));   //makedev(1, 5) as the kernel encodes it
    CHECK_EQ(device->mode & kFileTypeMask, kCharacterDevice);
}

TEST_CASE(full_v1_xattr_values_survive_byte_for_byte)
{
    const SendStream& stream = parse_fixture("full-v1.bin").stream;

    //setfattr -n user.text -v 'plain value' and setfattr -n user.binary -v 0x00010203ff7f
    const SetXattrOperation* text = find_xattr(stream, "f1.txt", "user.text");
    CHECK(text != nullptr);
    CHECK_EQ(text->value.to_string(), std::string("plain value"));

    const SetXattrOperation* binary = find_xattr(stream, "f1.txt", "user.binary");
    CHECK(binary != nullptr);
    CHECK_EQ(binary->value.to_string(), std::string("\x00\x01\x02\x03\xff\x7f", 6));

    //make-weird-tree.py sets an xattr that contains a NUL and a byte above 0x7f on a file with a space in
    //its name, so the value has to come back as bytes and not as a NUL terminated string.
    const std::string weird_path = "w e i r d/spaces in name.txt";
    const SetXattrOperation* weird_binary = find_xattr(stream, weird_path, "user.binary");
    CHECK(weird_binary != nullptr);
    CHECK_EQ(weird_binary->value.to_string(), std::string("\x00\x01\x02\xff\x7f\x80" "binary" "\x00" "value", 18));

    const SetXattrOperation* weird_text = find_xattr(stream, weird_path, "user.text");
    CHECK(weird_text != nullptr);
    CHECK_EQ(weird_text->value.to_string(), std::string("a value with a \xff byte", 21));
}

TEST_CASE(full_v1_paths_that_need_escaping_round_trip)
{
    const SendStream& stream = parse_fixture("full-v1.bin").stream;
    const std::size_t weird_count = sizeof(kWeirdPaths) / sizeof(kWeirdPaths[0]);

    for (std::size_t index = 0; index < weird_count; ++index)
    {
        const std::string path = kWeirdPaths[index];
        const WriteOperation* write = find_write(stream, path, 0);
        CHECK_MSG(write != nullptr, path);
        //make-weird-tree.py writes "content of file <index>\n" into every one of them, in this order.
        CHECK_MSG(write->data.to_string() == "content of file " + std::to_string(index) + "\n", path);

        const SendOperation* owner = nullptr;
        for (const SendOperation& operation : stream.operations)
        {
            if (operation_path(operation) == path)
            {
                owner = &operation;
                break;
            }
        }
        CHECK_MSG(owner != nullptr, path);
    }

    //A path with a NUL in it cannot exist on disk, and the parser must not invent one.
    for (const SendOperation& operation : stream.operations)
    {
        const std::string_view path = operation_path(operation);
        CHECK(path.find('\0') == std::string_view::npos);
    }
}

TEST_CASE(incremental_v2_clone_points_at_the_parent_snapshot)
{
    const SendStream& stream = parse_fixture("incremental-v2.bin").stream;

    const SnapshotOperation* snapshot = find_typed<SnapshotOperation>(stream, [](const SnapshotOperation&) { return true; });
    CHECK(snapshot != nullptr);

    //cp --reflink=always big.bin big-copy.bin turns into a CLONE against the parent snapshot.
    const CloneOperation* clone = find_typed<CloneOperation>(stream, [](const CloneOperation& operation) { return operation.path == "big-copy.bin"; });
    CHECK(clone != nullptr);
    CHECK_EQ(clone->clone_path, std::string("big.bin"));
    CHECK_EQ(clone->file_offset, std::uint64_t(0));
    CHECK_EQ(clone->clone_offset, std::uint64_t(0));
    CHECK_EQ(clone->length, std::uint64_t(kBigBinLength));
    CHECK_EQ(clone->clone_uuid, snapshot->clone_uuid);
    CHECK_EQ(clone->clone_ctrans_id, snapshot->clone_ctrans_id);
}

TEST_CASE(incremental_v2_fileattr_records_the_noatime_flag)
{
    const SendStream& stream = parse_fixture("incremental-v2.bin").stream;

    //chattr +A on d1/y is what the generator script does; +A means "do not update atime".
    const FileattrOperation* fileattr = find_typed<FileattrOperation>(stream, [](const FileattrOperation& operation) { return operation.path == "d1/y"; });
    CHECK(fileattr != nullptr);

    const std::vector<std::string> names = flags::inode_flag_names(fileattr->fileattr);
    bool found = false;
    for (const std::string& name : names)
    {
        if (name == "noatime")
            found = true;
    }
    CHECK(found);
    CHECK((fileattr->fileattr & static_cast<std::uint64_t>(flags::InodeFlag::NoAtime)) != 0);

    //FILEATTR does not exist in stream version 1, and the sender knows it.
    const SendStream& version_one = parse_fixture("incremental-v1.bin").stream;
    CHECK_EQ(count_command(version_one, Command::Fileattr), std::size_t(0));
    CHECK_EQ(count_command(version_one, Command::Fallocate), std::size_t(0));
    CHECK_EQ(count_command(version_one, Command::EncodedWrite), std::size_t(0));
}

TEST_CASE(nodata_streams_report_changed_extents_and_no_file_data)
{
    for (const char* name : {"nodata-v1.bin", "nodata-v2.bin"})
    {
        const SendStream& stream = parse_fixture(name).stream;

        CHECK_EQ(count_command(stream, Command::Write), std::size_t(0));
        CHECK_EQ(count_command(stream, Command::EncodedWrite), std::size_t(0));
        CHECK_EQ(count_command(stream, Command::UpdateExtent), std::size_t(4));

        const std::vector<ChangedExtent> extents = changed_extents(stream);
        CHECK_EQ(extents.size(), std::size_t(4));
        for (const ExpectedExtent& expected : kChangedExtents)
        {
            bool found = false;
            for (const ChangedExtent& extent : extents)
            {
                if (extent.path == expected.path && extent.offset == expected.offset && extent.length == expected.length)
                    found = true;
            }
            CHECK_MSG(found, std::string(name) + ": " + expected.path);
        }
    }

    //The same run with data carries the same changes as WRITE commands instead.
    const SendStream& with_data = parse_fixture("incremental-v2.bin").stream;
    CHECK_EQ(count_command(with_data, Command::UpdateExtent), std::size_t(0));
    CHECK_EQ(count_command(with_data, Command::Write), std::size_t(4));
}

TEST_CASE(compressed_stream_carries_encoded_writes)
{
    const SendStream& stream = parse_fixture("compressed-v2.bin").stream;

    CHECK_EQ(count_command(stream, Command::Write), std::size_t(0));
    CHECK_EQ(count_command(stream, Command::EncodedWrite), std::size_t(2));

    std::uint64_t unencoded_file_bytes = 0;
    for (const SendOperation& operation : stream.operations)
    {
        const EncodedWriteOperation* encoded = operation.as<EncodedWriteOperation>();
        if (encoded == nullptr)
            continue;
        CHECK_EQ(encoded->path, std::string("comp/repeat.txt"));
        //btrfs send --compressed-data compresses with the filesystem default, zstd on this filesystem.
        CHECK_EQ(encoded->compression, static_cast<std::uint32_t>(flags::Compression::Zstd));
        CHECK_EQ(encoded->encryption, static_cast<std::uint32_t>(flags::Encryption::None));
        CHECK(!encoded->data.empty());
        //The compressed payload is never longer than what it decodes to.
        CHECK(encoded->data.size() < encoded->unencoded_len);
        CHECK_EQ(encoded->file_offset, encoded->unencoded_offset);
        unencoded_file_bytes += encoded->unencoded_file_len;
    }
    //4096 and 40960 bytes of the 49152 byte file are transferred, the rest is already in the parent.
    CHECK_EQ(unencoded_file_bytes, std::uint64_t(45056));
}

TEST_CASE(fallocate_fileattr_stream_carries_a_punch_hole)
{
    const SendStream& stream = parse_fixture("fallocate-fileattr-v2.bin").stream;

    const FallocateOperation* fallocate = find_typed<FallocateOperation>(stream, [](const FallocateOperation&) { return true; });
    CHECK(fallocate != nullptr);
    CHECK_EQ(fallocate->path, std::string("comp/repeat.txt"));
    CHECK_EQ(fallocate->file_offset, std::uint64_t(4096));
    CHECK_EQ(fallocate->length, std::uint64_t(4096));
    CHECK_EQ(fallocate->mode, static_cast<std::uint32_t>(flags::FallocateMode::PunchHole) | static_cast<std::uint32_t>(flags::FallocateMode::KeepSize));

    const std::vector<std::string> mode_names = flags::fallocate_mode_names(fallocate->mode);
    CHECK(std::find(mode_names.begin(), mode_names.end(), std::string("punch_hole")) != mode_names.end());

    CHECK_EQ(count_command(stream, Command::Write), std::size_t(2));
    CHECK_EQ(count_command(stream, Command::EncodedWrite), std::size_t(0));
}

TEST_CASE(multi_subvolume_streams_are_read_to_the_end)
{
    //btrfs send without -e writes two complete streams, headers included.
    const ParsedFixture concatenated = parse_fixture("multi-subvol-v1.bin");
    CHECK_EQ(count_command(concatenated.stream, Command::Subvol), std::size_t(2));
    CHECK_EQ(count_command(concatenated.stream, Command::End), std::size_t(2));
    CHECK_EQ(operation_path(concatenated.stream.operations[0]), std::string_view("snap-minimal"));
    CHECK_EQ(operation_path(concatenated.stream.operations[5]), std::string_view("snap1"));

    //btrfs send -e omits the END of the last stream, so the second subvolume starts without one in between.
    const ParsedFixture omitted = parse_fixture("multi-subvol-omit-end-v1.bin");
    CHECK_EQ(count_command(omitted.stream, Command::Subvol), std::size_t(2));
    CHECK_EQ(count_command(omitted.stream, Command::End), std::size_t(1));
    CHECK_EQ(operation_path(omitted.stream.operations[0]), std::string_view("snap-minimal"));
    CHECK_EQ(operation_path(omitted.stream.operations[4]), std::string_view("snap1"));

    //With --single-stream the second header is refused instead of being followed.
    Parser::Options single;
    single.allow_concatenated_streams = false;
    CHECK_PARSE_ERROR(parse_fixture_bytes(concatenated.bytes, single), ErrorCode::UnknownCommand);

    //The omitted END case is legal exactly because the second header stands in for the missing END.
    Parser::Options strict;
    strict.require_subvol_or_snapshot_first = true;
    CHECK_EQ(parse_fixture_bytes(omitted.bytes, strict).stream.count(Command::Subvol), std::size_t(2));
}

TEST_CASE(truncated_real_streams_are_rejected)
{
    const std::string bytes = read_fixture("full-v1.bin");
    CHECK(!bytes.empty());

    //Cutting one byte off truncates the header of the END command, which has an empty payload. The failure
    //is at the end of the stream, so the whole stream has to be walked to reach it.
    const std::string cut = bytes.substr(0, bytes.size() - 1);
    MemorySource cut_source(cut);
    Parser cut_parser(cut_source);
    bool truncated = false;
    try
    {
        while (cut_parser.next().has_value())
        {
        }
    }
    catch (const ParseError& error)
    {
        truncated = error.code() == ErrorCode::UnexpectedEndOfStream;
    }
    CHECK(truncated);

    //Cutting a whole command off instead ends the stream cleanly one operation early, which is legal: the
    //receiver cannot tell a short stream from a stream that ended.
    RawFraming raw;
    CHECK(walk_framing(bytes, raw));
    const std::size_t last_offset = static_cast<std::size_t>(raw.offsets.back());
    CHECK_EQ(parse_fixture_bytes(bytes.substr(0, last_offset)).stream.operations.size(), raw.command_ids.size() - 1);
}

TEST_CASE(corrupted_real_streams_fail_the_checksum_and_the_data_check)
{
    const std::string bytes = read_fixture("full-v1.bin");
    const ParsedFixture parsed = parse_fixture("full-v1.bin");

    //Flip the last byte of a WRITE payload: that byte is file data, so it can only be caught by the checksum.
    const WriteOperation* write = find_write(parsed.stream, "big.bin", 0);
    CHECK(write != nullptr);
    const SendOperation* write_operation = nullptr;
    for (const SendOperation& operation : parsed.stream.operations)
    {
        if (operation.as<WriteOperation>() == write)
            write_operation = &operation;
    }
    CHECK(write_operation != nullptr);

    std::string corrupted = bytes;
    const std::size_t data_offset = static_cast<std::size_t>(write_operation->stream_offset) + protocol::kCommandHeaderSize + write_operation->payload_size - 1;
    corrupted[data_offset] = static_cast<char>(static_cast<unsigned char>(corrupted[data_offset]) ^ 0x01u);

    //The corrupted command is not the first one in the stream, so the whole stream is walked.
    bool mismatched = false;
    MemorySource source(corrupted);
    Parser parser(source);
    try
    {
        while (parser.next().has_value())
        {
        }
    }
    catch (const ParseError& error)
    {
        mismatched = error.code() == ErrorCode::ChecksumMismatch;
    }
    CHECK(mismatched);

    //With verification off the same bytes parse, which shows the failure came from the checksum alone.
    Parser::Options unchecked;
    unchecked.verify_checksums = false;
    const ParsedFixture unchecked_stream = parse_fixture_bytes(corrupted, unchecked);
    CHECK_EQ(unchecked_stream.stream.operations.size(), parsed.stream.operations.size());

    const WriteOperation* corrupted_write = find_write(unchecked_stream.stream, "big.bin", 0);
    CHECK(corrupted_write != nullptr);
    CHECK_EQ(corrupted_write->data.size(), write->data.size());
    CHECK(corrupted_write->data.to_string() != write->data.to_string());
}

TEST_CASE(fixtures_parse_through_every_source_helper)
{
    const std::string path = std::string(BTRFS_SEND_PARSER_FIXTURE_DIR) + "/minimal-v2.bin";

    //The same fixture through the istream and the fd flavour of the convenience layer.
    const SendStream from_file = parse_file(path);
    CHECK_EQ(from_file.operations.size(), std::size_t(5));
    CHECK_EQ(from_file.version, std::uint32_t(2));

    const SendStream from_bytes = parse_fixture("minimal-v2.bin").stream;
    CHECK_EQ(from_file.operations.size(), from_bytes.operations.size());
    for (std::size_t index = 0; index < from_bytes.operations.size(); ++index)
    {
        CHECK_EQ(summary(from_file.operations[index]), summary(from_bytes.operations[index]));
        CHECK_EQ(from_file.operations[index].stream_offset, from_bytes.operations[index].stream_offset);
    }
}
