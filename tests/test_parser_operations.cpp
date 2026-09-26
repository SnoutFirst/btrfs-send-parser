#include "framework.hpp"
#include "stream_builder.hpp"

#include <string>
#include <vector>

#include "btrfs/send.hpp"

using namespace btrfs::send;
using testfw::StreamBuilder;
using protocol::Attribute;
using protocol::Command;

namespace {

const Uuid kUuidA = testfw::make_uuid(0xaa);
const Uuid kUuidB = testfw::make_uuid(0xbb);

void put_subvol(StreamBuilder& builder, std::string_view path = "subvol", const Uuid& uuid = kUuidA, std::uint64_t ctrans_id = 7)
{
    builder.begin_command(Command::Subvol);
    builder.put_string(Attribute::Path, path);
    builder.put_uuid(Attribute::Uuid, uuid);
    builder.put_u64(Attribute::CtransId, ctrans_id);
    builder.end_command();
}

void put_snapshot(StreamBuilder& builder, std::string_view path = "subvol")
{
    builder.begin_command(Command::Snapshot);
    builder.put_string(Attribute::Path, path);
    builder.put_uuid(Attribute::Uuid, kUuidA);
    builder.put_u64(Attribute::CtransId, 9);
    builder.put_uuid(Attribute::CloneUuid, kUuidB);
    builder.put_u64(Attribute::CloneCtransId, 8);
    builder.end_command();
}

void put_end(StreamBuilder& builder)
{
    builder.begin_command(Command::End);
    builder.end_command();
}

std::vector<SendOperation> parse_bytes(const std::string& bytes, Parser::Options options = {})
{
    std::vector<SendOperation> operations;
    MemorySource source(bytes);
    Parser parser(source, options);
    while (std::optional<SendOperation> operation = parser.next())
    {
        operation->materialize();
        operations.push_back(std::move(*operation));
    }
    return operations;
}

}

TEST_CASE(subvol_and_snapshot_are_typed)
{
    StreamBuilder builder(1);
    put_subvol(builder, "snap1", kUuidA, 9);
    put_snapshot(builder, "snap2");
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operations.size(), std::size_t(3));

    const SubvolOperation* subvol = operations[0].as<SubvolOperation>();
    CHECK(subvol != nullptr);
    CHECK_EQ(subvol->path, std::string("snap1"));
    CHECK_EQ(subvol->uuid, kUuidA);
    CHECK_EQ(subvol->ctrans_id, std::uint64_t(9));
    CHECK_EQ(operations[0].command, Command::Subvol);
    CHECK_EQ(operations[0].index, std::uint64_t(1));
    CHECK_EQ(operations[0].stream_offset, std::uint64_t(protocol::kStreamHeaderSize));
    CHECK_EQ(operations[0].attributes.size(), std::size_t(3));
    CHECK_EQ(operations[0].attribute(Attribute::Path)->value.as_string_view(), std::string_view("snap1"));

    const SnapshotOperation* snapshot = operations[1].as<SnapshotOperation>();
    CHECK(snapshot != nullptr);
    CHECK_EQ(snapshot->path, std::string("snap2"));
    CHECK_EQ(snapshot->uuid, kUuidA);
    CHECK_EQ(snapshot->ctrans_id, std::uint64_t(9));
    CHECK_EQ(snapshot->clone_uuid, kUuidB);
    CHECK_EQ(snapshot->clone_ctrans_id, std::uint64_t(8));

    CHECK(operations[2].is<EndOperation>());
    CHECK_EQ(operations[2].payload_size, std::uint32_t(0));
}

TEST_CASE(file_creation_commands_are_typed)
{
    StreamBuilder builder(1);
    put_subvol(builder);

    builder.begin_command(Command::Mkfile);
    builder.put_string(Attribute::Path, "o257-7-0");
    builder.put_u64(Attribute::Ino, 257);
    builder.end_command();

    builder.begin_command(Command::Mkdir);
    builder.put_string(Attribute::Path, "d1");
    builder.put_u64(Attribute::Ino, 258);
    builder.end_command();

    builder.begin_command(Command::Mknod);
    builder.put_string(Attribute::Path, "d1/zero");
    builder.put_u64(Attribute::Ino, 259);
    builder.put_u64(Attribute::Mode, 020644);
    builder.put_u64(Attribute::Rdev, 0x105);
    builder.end_command();

    builder.begin_command(Command::Mkfifo);
    builder.put_string(Attribute::Path, "d1/pipe");
    builder.put_u64(Attribute::Ino, 260);
    builder.put_u64(Attribute::Rdev, 0);
    builder.put_u64(Attribute::Mode, 010644);
    builder.end_command();

    builder.begin_command(Command::Mksock);
    builder.put_string(Attribute::Path, "sock");
    builder.put_u64(Attribute::Ino, 261);
    builder.put_u64(Attribute::Rdev, 0);
    builder.put_u64(Attribute::Mode, 0140644);
    builder.end_command();

    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operations.size(), std::size_t(7));

    const MkfileOperation* mkfile = operations[1].as<MkfileOperation>();
    CHECK(mkfile != nullptr);
    CHECK_EQ(mkfile->path, std::string("o257-7-0"));
    CHECK_EQ(mkfile->ino, std::uint64_t(257));

    const MkdirOperation* mkdir = operations[2].as<MkdirOperation>();
    CHECK(mkdir != nullptr);
    CHECK_EQ(mkdir->ino, std::uint64_t(258));

    const MknodOperation* mknod = operations[3].as<MknodOperation>();
    CHECK(mknod != nullptr);
    CHECK_EQ(mknod->path, std::string("d1/zero"));
    CHECK(mknod->ino.has_value());
    CHECK_EQ(*mknod->ino, std::uint64_t(259));
    CHECK_EQ(mknod->mode, std::uint64_t(020644));
    CHECK_EQ(mknod->rdev, std::uint64_t(0x105));

    const MkfifoOperation* mkfifo = operations[4].as<MkfifoOperation>();
    CHECK(mkfifo != nullptr);
    CHECK_EQ(mkfifo->ino, std::uint64_t(260));
    CHECK(mkfifo->rdev.has_value());
    CHECK_EQ(*mkfifo->rdev, std::uint64_t(0));
    CHECK(mkfifo->mode.has_value());
    CHECK_EQ(*mkfifo->mode, std::uint64_t(010644));

    const MksockOperation* mksock = operations[5].as<MksockOperation>();
    CHECK(mksock != nullptr);
    CHECK_EQ(mksock->path, std::string("sock"));
    CHECK(mksock->mode.has_value());
    CHECK_EQ(*mksock->mode, std::uint64_t(0140644));
}

TEST_CASE(mknod_without_inode_number_is_accepted)
{
    //The published format for MKNOD lists only path, mode and rdev.
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Mknod);
    builder.put_string(Attribute::Path, "d1/zero");
    builder.put_u64(Attribute::Mode, 020644);
    builder.put_u64(Attribute::Rdev, 0x105);
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    const MknodOperation* mknod = operations[1].as<MknodOperation>();
    CHECK(mknod != nullptr);
    CHECK(!mknod->ino.has_value());
    CHECK_EQ(mknod->mode, std::uint64_t(020644));
}

TEST_CASE(link_commands_are_typed)
{
    StreamBuilder builder(1);
    put_subvol(builder);

    builder.begin_command(Command::Symlink);
    builder.put_string(Attribute::Path, "o261-7-0");
    builder.put_u64(Attribute::Ino, 261);
    builder.put_string(Attribute::PathLink, "target.txt");
    builder.end_command();

    builder.begin_command(Command::Rename);
    builder.put_string(Attribute::Path, "o261-7-0");
    builder.put_string(Attribute::PathTo, "renamed.txt");
    builder.end_command();

    builder.begin_command(Command::Link);
    builder.put_string(Attribute::Path, "renamed.txt");
    builder.put_string(Attribute::PathLink, "hard.txt");
    builder.end_command();

    builder.begin_command(Command::Unlink);
    builder.put_string(Attribute::Path, "hard.txt");
    builder.end_command();

    builder.begin_command(Command::Rmdir);
    builder.put_string(Attribute::Path, "d1");
    builder.end_command();

    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operations.size(), std::size_t(7));

    const SymlinkOperation* symlink = operations[1].as<SymlinkOperation>();
    CHECK(symlink != nullptr);
    CHECK_EQ(symlink->path, std::string("o261-7-0"));
    CHECK_EQ(symlink->ino, std::uint64_t(261));
    CHECK_EQ(symlink->link_target, std::string("target.txt"));

    const RenameOperation* rename = operations[2].as<RenameOperation>();
    CHECK(rename != nullptr);
    CHECK_EQ(rename->path, std::string("o261-7-0"));
    CHECK_EQ(rename->path_to, std::string("renamed.txt"));

    const LinkOperation* link = operations[3].as<LinkOperation>();
    CHECK(link != nullptr);
    CHECK_EQ(link->path, std::string("renamed.txt"));
    CHECK_EQ(link->link_target, std::string("hard.txt"));

    CHECK(operations[4].is<UnlinkOperation>());
    CHECK_EQ(operations[4].as<UnlinkOperation>()->path, std::string("hard.txt"));
    CHECK(operations[5].is<RmdirOperation>());
    CHECK_EQ(operations[5].as<RmdirOperation>()->path, std::string("d1"));
}

TEST_CASE(xattr_values_are_binary_safe)
{
    //NUL bytes, 0xff, and a value that is not valid UTF-8: the parser must not truncate or reinterpret.
    const std::string value("\x00\x01\x02\xff\x7f\x80", 6);

    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::SetXattr);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_string(Attribute::XattrName, "user.binary");
    builder.put_bytes(Attribute::XattrData, value);
    builder.end_command();
    builder.begin_command(Command::RemoveXattr);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_string(Attribute::XattrName, "user.text");
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operations.size(), std::size_t(4));

    const SetXattrOperation* set_xattr = operations[1].as<SetXattrOperation>();
    CHECK(set_xattr != nullptr);
    CHECK_EQ(set_xattr->name, std::string("user.binary"));
    CHECK_EQ(set_xattr->value.size(), value.size());
    CHECK_EQ(set_xattr->value.to_string(), value);
    CHECK_EQ(testfw::hex_encode(set_xattr->value.as_string_view()), std::string("000102ff7f80"));

    const RemoveXattrOperation* remove_xattr = operations[2].as<RemoveXattrOperation>();
    CHECK(remove_xattr != nullptr);
    CHECK_EQ(remove_xattr->name, std::string("user.text"));
}

TEST_CASE(write_data_length_rules_per_version)
{
    //Version 1: DATA carries an explicit 16 bit length.
    StreamBuilder version1(1);
    put_subvol(version1);
    version1.begin_command(Command::Write);
    version1.put_string(Attribute::Path, "f1.txt");
    version1.put_u64(Attribute::FileOffset, 4096);
    version1.put_raw_attribute(static_cast<std::uint16_t>(Attribute::Data), "hello world\n");
    version1.end_command();
    put_end(version1);

    const std::vector<SendOperation> version1_operations = parse_bytes(version1.bytes());
    const WriteOperation* version1_write = version1_operations[1].as<WriteOperation>();
    CHECK(version1_write != nullptr);
    CHECK_EQ(version1_write->file_offset, std::uint64_t(4096));
    CHECK_EQ(version1_write->data.size(), std::size_t(12));
    CHECK_EQ(version1_write->data.to_string(), std::string("hello world\n"));

    //Version 2: DATA has no length field and runs to the end of the command payload.
    const std::string payload(5000, 'z');
    StreamBuilder version2(2);
    put_subvol(version2);
    version2.begin_command(Command::Write);
    version2.put_string(Attribute::Path, "big.bin");
    version2.put_u64(Attribute::FileOffset, 0);
    version2.put_lengthless_data(payload);
    version2.end_command();
    put_end(version2);

    const std::vector<SendOperation> version2_operations = parse_bytes(version2.bytes());
    const WriteOperation* version2_write = version2_operations[1].as<WriteOperation>();
    CHECK(version2_write != nullptr);
    CHECK_EQ(version2_write->data.size(), payload.size());
    CHECK(version2_write->data.to_string() == payload);
    //The attribute is still listed, with the length the parser derived.
    CHECK_EQ(version2_operations[1].attribute(Attribute::Data)->value.size(), payload.size());
}

TEST_CASE(clone_command_is_typed)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Clone);
    builder.put_string(Attribute::Path, "big-copy.bin");
    builder.put_u64(Attribute::FileOffset, 0);
    builder.put_u64(Attribute::CloneLen, 16384);
    builder.put_uuid(Attribute::CloneUuid, kUuidB);
    builder.put_u64(Attribute::CloneCtransId, 11);
    builder.put_string(Attribute::ClonePath, "big.bin");
    builder.put_u64(Attribute::CloneOffset, 4096);
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    const CloneOperation* clone = operations[1].as<CloneOperation>();
    CHECK(clone != nullptr);
    CHECK_EQ(clone->path, std::string("big-copy.bin"));
    CHECK_EQ(clone->file_offset, std::uint64_t(0));
    CHECK_EQ(clone->length, std::uint64_t(16384));
    CHECK_EQ(clone->clone_uuid, kUuidB);
    CHECK_EQ(clone->clone_ctrans_id, std::uint64_t(11));
    CHECK_EQ(clone->clone_path, std::string("big.bin"));
    CHECK_EQ(clone->clone_offset, std::uint64_t(4096));
}

TEST_CASE(metadata_commands_are_typed)
{
    StreamBuilder builder(2);
    put_subvol(builder);

    builder.begin_command(Command::Truncate);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_u64(Attribute::Size, 100);
    builder.end_command();

    builder.begin_command(Command::Chmod);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_u64(Attribute::Mode, 0640);
    builder.end_command();

    builder.begin_command(Command::Chown);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_u64(Attribute::Uid, 1000);
    builder.put_u64(Attribute::Gid, 1000);
    builder.end_command();

    builder.begin_command(Command::Utimes);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_timespec(Attribute::Atime, 1577952245, 0);
    builder.put_timespec(Attribute::Mtime, 1577952246, 1);
    builder.put_timespec(Attribute::Ctime, 1577952247, 999999999);
    builder.put_timespec(Attribute::Otime, 1577952248, 2);
    builder.end_command();

    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK(operations[1].is<TruncateOperation>());
    CHECK_EQ(operations[1].as<TruncateOperation>()->size, std::uint64_t(100));
    CHECK(operations[2].is<ChmodOperation>());
    CHECK_EQ(operations[2].as<ChmodOperation>()->mode, std::uint64_t(0640));
    CHECK(operations[3].is<ChownOperation>());
    CHECK_EQ(operations[3].as<ChownOperation>()->uid, std::uint64_t(1000));
    CHECK_EQ(operations[3].as<ChownOperation>()->gid, std::uint64_t(1000));

    const UtimesOperation* utimes = operations[4].as<UtimesOperation>();
    CHECK(utimes != nullptr);
    CHECK_EQ(utimes->atime.seconds, std::int64_t(1577952245));
    CHECK_EQ(utimes->mtime.nanoseconds, std::uint32_t(1));
    CHECK_EQ(utimes->ctime.nanoseconds, std::uint32_t(999999999));
    CHECK(utimes->otime.has_value());
    CHECK_EQ(utimes->otime->seconds, std::int64_t(1577952248));
}

TEST_CASE(utimes_without_otime_in_version_one)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Utimes);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_timespec(Attribute::Atime, 1, 0);
    builder.put_timespec(Attribute::Mtime, 2, 0);
    builder.put_timespec(Attribute::Ctime, 3, 0);
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    const UtimesOperation* utimes = operations[1].as<UtimesOperation>();
    CHECK(utimes != nullptr);
    CHECK(!utimes->otime.has_value());
}

TEST_CASE(update_extent_is_typed)
{
    //This is what a "btrfs send --no-data" stream carries instead of WRITE.
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::UpdateExtent);
    builder.put_string(Attribute::Path, "sparse.bin");
    builder.put_u64(Attribute::FileOffset, 67108864);
    builder.put_u64(Attribute::Size, 4096);
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    const UpdateExtentOperation* update = operations[1].as<UpdateExtentOperation>();
    CHECK(update != nullptr);
    CHECK_EQ(update->path, std::string("sparse.bin"));
    CHECK_EQ(update->file_offset, std::uint64_t(67108864));
    CHECK_EQ(update->length, std::uint64_t(4096));
    CHECK_EQ(operations[1].attributes.size(), std::size_t(3));
}

TEST_CASE(version_two_commands_are_typed)
{
    StreamBuilder builder(2);
    put_subvol(builder);

    builder.begin_command(Command::Fallocate);
    builder.put_string(Attribute::Path, "comp/repeat.txt");
    builder.put_u32(Attribute::FallocateMode, 0x3);
    builder.put_u64(Attribute::FileOffset, 4096);
    builder.put_u64(Attribute::Size, 4096);
    builder.end_command();

    builder.begin_command(Command::Fileattr);
    builder.put_string(Attribute::Path, "d1/y");
    builder.put_u64(Attribute::Fileattr, 0x208);
    builder.end_command();

    builder.begin_command(Command::EncodedWrite);
    builder.put_string(Attribute::Path, "comp/repeat.txt");
    builder.put_u64(Attribute::FileOffset, 0);
    builder.put_u64(Attribute::UnencodedFileLen, 65536);
    builder.put_u64(Attribute::UnencodedLen, 16384);
    builder.put_u64(Attribute::UnencodedOffset, 0);
    builder.put_u32(Attribute::Compression, 1);
    builder.put_u32(Attribute::Encryption, 0);
    builder.put_lengthless_data("compressed payload");
    builder.end_command();

    //Compression and encryption are optional and default to NONE.
    builder.begin_command(Command::EncodedWrite);
    builder.put_string(Attribute::Path, "comp/other.txt");
    builder.put_u64(Attribute::FileOffset, 0);
    builder.put_u64(Attribute::UnencodedFileLen, 10);
    builder.put_u64(Attribute::UnencodedLen, 10);
    builder.put_u64(Attribute::UnencodedOffset, 0);
    builder.put_lengthless_data("payload");
    builder.end_command();

    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operations.size(), std::size_t(6));

    const FallocateOperation* fallocate = operations[1].as<FallocateOperation>();
    CHECK(fallocate != nullptr);
    CHECK_EQ(fallocate->mode, std::uint32_t(0x3));
    CHECK_EQ(fallocate->file_offset, std::uint64_t(4096));
    CHECK_EQ(fallocate->length, std::uint64_t(4096));

    const FileattrOperation* fileattr = operations[2].as<FileattrOperation>();
    CHECK(fileattr != nullptr);
    CHECK_EQ(fileattr->fileattr, std::uint64_t(0x208));

    const EncodedWriteOperation* encoded = operations[3].as<EncodedWriteOperation>();
    CHECK(encoded != nullptr);
    CHECK_EQ(encoded->unencoded_file_len, std::uint64_t(65536));
    CHECK_EQ(encoded->unencoded_len, std::uint64_t(16384));
    CHECK_EQ(encoded->unencoded_offset, std::uint64_t(0));
    CHECK_EQ(encoded->compression, std::uint32_t(1));
    CHECK_EQ(encoded->encryption, std::uint32_t(0));
    CHECK_EQ(encoded->data.to_string(), std::string("compressed payload"));

    const EncodedWriteOperation* defaults = operations[4].as<EncodedWriteOperation>();
    CHECK(defaults != nullptr);
    CHECK_EQ(defaults->compression, std::uint32_t(0));
    CHECK_EQ(defaults->encryption, std::uint32_t(0));
}

TEST_CASE(enable_verity_is_typed_in_version_three)
{
    StreamBuilder builder(3);
    put_subvol(builder);
    builder.begin_command(Command::EnableVerity);
    builder.put_string(Attribute::Path, "signed.bin");
    builder.put_u8(Attribute::VerityAlgorithm, 1);
    builder.put_u32(Attribute::VerityBlockSize, 4096);
    builder.put_bytes(Attribute::VeritySaltData, std::string("\x01\x02\x03", 3));
    builder.put_bytes(Attribute::VeritySigData, std::string("\xfe\xff", 2));
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    const EnableVerityOperation* verity = operations[1].as<EnableVerityOperation>();
    CHECK(verity != nullptr);
    CHECK_EQ(verity->path, std::string("signed.bin"));
    CHECK_EQ(verity->algorithm, std::uint8_t(1));
    CHECK_EQ(verity->block_size, std::uint32_t(4096));
    CHECK_EQ(verity->salt.size(), std::size_t(3));
    CHECK_EQ(verity->signature.size(), std::size_t(2));
}

TEST_CASE(empty_path_attribute_is_valid)
{
    //Real streams use an empty path for the subvolume root.
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Chmod);
    builder.put_string(Attribute::Path, "");
    builder.put_u64(Attribute::Mode, 0755);
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    const ChmodOperation* chmod = operations[1].as<ChmodOperation>();
    CHECK(chmod != nullptr);
    CHECK(chmod->path.empty());
    CHECK_EQ(operations[1].attribute(Attribute::Path)->value.size(), std::size_t(0));
}

TEST_CASE(duplicate_attribute_last_one_wins)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Unlink);
    builder.put_string(Attribute::Path, "first");
    builder.put_string(Attribute::Path, "second");
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operations[1].attributes.size(), std::size_t(2));
    CHECK_EQ(operations[1].as<UnlinkOperation>()->path, std::string("second"));
}

TEST_CASE(attributes_keep_stream_order)
{
    StreamBuilder builder(1);
    put_subvol(builder, "subvol", kUuidA, 3);
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operations[0].attributes.size(), std::size_t(3));
    CHECK_EQ(operations[0].attributes[0].type, Attribute::Path);
    CHECK_EQ(operations[0].attributes[1].type, Attribute::Uuid);
    CHECK_EQ(operations[0].attributes[2].type, Attribute::CtransId);
}

TEST_CASE(unknown_command_is_rejected_by_default)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(static_cast<Command>(99));
    builder.put_string(Attribute::Path, "future");
    builder.end_command();
    put_end(builder);

    MemorySource source(builder.bytes());
    Parser parser(source);
    CHECK(parser.next().has_value());
    CHECK_PARSE_ERROR(parser.next(), ErrorCode::UnknownCommand);
}

TEST_CASE(unknown_command_can_be_preserved)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(static_cast<Command>(99));
    builder.put_string(Attribute::Path, "future");
    builder.end_command();
    put_end(builder);

    Parser::Options options;
    options.allow_unknown_commands = true;
    const std::vector<SendOperation> operations = parse_bytes(builder.bytes(), options);
    CHECK_EQ(operations.size(), std::size_t(3));
    CHECK_EQ(operations[1].command, static_cast<Command>(99));
    const UnknownCommandOperation* unknown = operations[1].as<UnknownCommandOperation>();
    CHECK(unknown != nullptr);
    CHECK_EQ(unknown->command, static_cast<Command>(99));
    //The raw payload is handed over as it arrived, TLV header included, so nothing is dropped.
    CHECK_EQ(unknown->payload.size(), std::size_t(10));
}

TEST_CASE(unknown_attribute_type_is_rejected_by_default)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Unlink);
    builder.put_raw_attribute(40, "future attribute");
    builder.end_command();
    put_end(builder);

    MemorySource source(builder.bytes());
    Parser parser(source);
    CHECK(parser.next().has_value());
    CHECK_PARSE_ERROR(parser.next(), ErrorCode::UnknownAttributeType);
}

TEST_CASE(unknown_attribute_type_can_be_skipped)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Unlink);
    builder.put_raw_attribute(40, "future attribute");
    builder.put_string(Attribute::Path, "gone.txt");
    builder.end_command();
    put_end(builder);

    Parser::Options options;
    options.allow_unknown_attribute_types = true;
    const std::vector<SendOperation> operations = parse_bytes(builder.bytes(), options);
    CHECK_EQ(operations.size(), std::size_t(3));
    CHECK_EQ(operations[1].as<UnlinkOperation>()->path, std::string("gone.txt"));
    CHECK_EQ(operations[1].attributes.size(), std::size_t(2));
    CHECK_EQ(operations[1].attributes[0].type, static_cast<Attribute>(40));
    CHECK_EQ(operations[1].attributes[0].value.as_string_view(), std::string_view("future attribute"));
}

TEST_CASE(unexpected_attribute_is_rejected_by_default)
{
    StreamBuilder builder(2);
    put_subvol(builder);
    builder.begin_command(Command::Chmod);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_u64(Attribute::Mode, 0644);
    builder.put_u32(Attribute::Compression, 1);
    builder.end_command();
    put_end(builder);

    MemorySource source(builder.bytes());
    Parser parser(source);
    CHECK(parser.next().has_value());
    CHECK_PARSE_ERROR(parser.next(), ErrorCode::UnexpectedAttribute);
}

TEST_CASE(unexpected_attribute_can_be_kept)
{
    StreamBuilder builder(2);
    put_subvol(builder);
    builder.begin_command(Command::Chmod);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_u64(Attribute::Mode, 0644);
    builder.put_u32(Attribute::Compression, 1);
    builder.end_command();
    put_end(builder);

    Parser::Options options;
    options.allow_unexpected_attributes = true;
    const std::vector<SendOperation> operations = parse_bytes(builder.bytes(), options);
    CHECK_EQ(operations.size(), std::size_t(3));
    CHECK_EQ(operations[1].as<ChmodOperation>()->mode, std::uint64_t(0644));
    CHECK_EQ(operations[1].attributes.size(), std::size_t(3));
    CHECK_EQ(operations[1].attributes[2].type, Attribute::Compression);
}

TEST_CASE(headerless_stream_needs_the_option)
{
    StreamBuilder builder = StreamBuilder::headerless(2);
    put_subvol(builder);
    put_end(builder);

    {
        MemorySource source(builder.bytes());
        Parser parser(source);
        //Without the option the first 13 bytes are compared against the magic, which a headerless stream
        //cannot match.
        CHECK_PARSE_ERROR(parser.next(), ErrorCode::InvalidMagic);
    }

    Parser::Options options;
    options.expect_stream_header = false;
    options.assumed_stream_version = 2;
    const std::vector<SendOperation> operations = parse_bytes(builder.bytes(), options);
    CHECK_EQ(operations.size(), std::size_t(2));
    CHECK(operations[0].is<SubvolOperation>());
}

TEST_CASE(headerless_stream_rejects_a_bad_assumed_version)
{
    StreamBuilder builder = StreamBuilder::headerless();
    put_subvol(builder);
    put_end(builder);

    Parser::Options options;
    options.expect_stream_header = false;
    options.assumed_stream_version = 9;
    MemorySource source(builder.bytes());
    Parser parser(source, options);
    CHECK_PARSE_ERROR(parser.next(), ErrorCode::UnsupportedVersion);
}

TEST_CASE(concatenated_streams_are_parsed)
{
    //"btrfs send subvol1 subvol2" without -e writes two complete streams into one file.
    StreamBuilder builder(1);
    put_subvol(builder, "first");
    put_end(builder);
    builder.append_stream_header(1);
    put_subvol(builder, "second");
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operations.size(), std::size_t(4));
    CHECK_EQ(operations[0].as<SubvolOperation>()->path, std::string("first"));
    CHECK(operations[1].is<EndOperation>());
    CHECK_EQ(operations[2].as<SubvolOperation>()->path, std::string("second"));
    CHECK(operations[3].is<EndOperation>());
    CHECK_EQ(operations[2].index, std::uint64_t(3));
}

TEST_CASE(concatenated_streams_can_be_refused)
{
    StreamBuilder builder(1);
    put_subvol(builder, "first");
    put_end(builder);
    builder.append_stream_header(1);
    put_subvol(builder, "second");
    put_end(builder);

    Parser::Options options;
    options.allow_concatenated_streams = false;
    MemorySource source(builder.bytes());
    Parser parser(source, options);
    CHECK(parser.next().has_value());
    CHECK(parser.next().has_value());
    CHECK_PARSE_ERROR(parser.next(), ErrorCode::UnknownCommand);
}

TEST_CASE(multiple_subvolumes_without_end_command)
{
    //"btrfs send -e subvol1 subvol2": one header, several subvolumes, no END in between.
    StreamBuilder builder(1);
    put_subvol(builder, "first");
    builder.begin_command(Command::Mkfile);
    builder.put_string(Attribute::Path, "first.txt");
    builder.put_u64(Attribute::Ino, 257);
    builder.end_command();
    put_subvol(builder, "second");
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operations.size(), std::size_t(4));
    CHECK_EQ(operations[0].as<SubvolOperation>()->path, std::string("first"));
    CHECK_EQ(operations[2].as<SubvolOperation>()->path, std::string("second"));
}

TEST_CASE(first_command_rule)
{
    StreamBuilder builder(1);
    builder.begin_command(Command::Mkfile);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.put_u64(Attribute::Ino, 257);
    builder.end_command();
    put_end(builder);

    {
        MemorySource source(builder.bytes());
        Parser parser(source);
        CHECK_PARSE_ERROR(parser.next(), ErrorCode::InvalidCommandStructure);
    }

    Parser::Options options;
    options.require_subvol_or_snapshot_first = false;
    const std::vector<SendOperation> operations = parse_bytes(builder.bytes(), options);
    CHECK_EQ(operations.size(), std::size_t(2));
    CHECK(operations[0].is<MkfileOperation>());
}

TEST_CASE(stop_after_end_command)
{
    StreamBuilder builder(1);
    put_subvol(builder, "first");
    put_end(builder);
    builder.append_stream_header(1);
    put_subvol(builder, "second");
    put_end(builder);

    Parser::Options options;
    options.stop_after_end_command = true;

    std::vector<SendOperation> operations;
    MemorySource source(builder.bytes());
    Parser parser(source, options);
    while (std::optional<SendOperation> operation = parser.next())
    {
        operation->materialize();
        operations.push_back(std::move(*operation));
    }
    CHECK_EQ(operations.size(), std::size_t(2));
    CHECK(operations[1].is<EndOperation>());
    CHECK(parser.finished());
    CHECK(!parser.next().has_value());
}

TEST_CASE(checksum_verification_can_be_switched_off)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    put_end(builder);
    builder.corrupt_checksum_of_command(0);

    {
        MemorySource source(builder.bytes());
        Parser parser(source);
        CHECK_PARSE_ERROR(parser.next(), ErrorCode::ChecksumMismatch);
    }

    Parser::Options options;
    options.verify_checksums = false;
    const std::vector<SendOperation> operations = parse_bytes(builder.bytes(), options);
    CHECK_EQ(operations.size(), std::size_t(2));
}

TEST_CASE(parser_state_reporting)
{
    StreamBuilder builder(2);
    put_subvol(builder, "subvol");
    put_end(builder);

    MemorySource source(builder.bytes());
    Parser parser(source);
    CHECK(!parser.header_parsed());
    CHECK_EQ(parser.stream_version(), std::uint32_t(0));
    CHECK_EQ(parser.commands_parsed(), std::uint64_t(0));
    CHECK_EQ(parser.bytes_consumed(), std::uint64_t(0));

    CHECK(parser.next().has_value());
    CHECK(parser.header_parsed());
    CHECK_EQ(parser.stream_version(), std::uint32_t(2));
    CHECK_EQ(parser.commands_parsed(), std::uint64_t(1));
    CHECK_EQ(parser.bytes_consumed(), std::uint64_t(protocol::kStreamHeaderSize + protocol::kCommandHeaderSize + 42));

    SendOperation storage;
    ParseFailure failure;
    CHECK_EQ(parser.next(storage, failure), NextStatus::HaveOperation);
    CHECK(storage.is<EndOperation>());
    CHECK_EQ(parser.commands_parsed(), std::uint64_t(2));

    CHECK_EQ(parser.next(storage, failure), NextStatus::EndOfStream);
    CHECK(parser.finished());
    CHECK_EQ(parser.commands_parsed(), std::uint64_t(2));
    CHECK(!parser.next().has_value());
}

TEST_CASE(non_throwing_api_reports_failures)
{
    StreamBuilder builder(1);
    put_subvol(builder);
    builder.begin_command(Command::Mkfile);
    builder.put_string(Attribute::Path, "f1.txt");
    builder.end_command();   //no ino

    MemorySource source(builder.bytes());
    Parser parser(source);
    SendOperation operation;
    ParseFailure failure;

    CHECK_EQ(parser.next(operation, failure), NextStatus::HaveOperation);
    CHECK_EQ(parser.next(operation, failure), NextStatus::Error);
    CHECK_EQ(failure.code, ErrorCode::MissingAttribute);
    CHECK_EQ(failure.command_index, std::uint64_t(2));
    CHECK(!failure.message.empty());
    CHECK(failure.stream_offset >= protocol::kStreamHeaderSize);

    //The parser is finished, later calls must not throw or continue.
    CHECK_EQ(parser.next(operation, failure), NextStatus::EndOfStream);
    CHECK_EQ(parser.next(operation, failure), NextStatus::EndOfStream);
}

TEST_CASE(operation_summary_is_renderable)
{
    StreamBuilder builder(1);
    put_subvol(builder, "snap1", kUuidA, 9);
    builder.begin_command(Command::Rename);
    builder.put_string(Attribute::Path, "d1/x");
    builder.put_string(Attribute::PathTo, "d1/y");
    builder.end_command();
    builder.begin_command(Command::SetXattr);
    builder.put_string(Attribute::Path, "f\"quote\".txt");
    builder.put_string(Attribute::XattrName, "user.binary");
    builder.put_bytes(Attribute::XattrData, std::string("\x01\x02", 2));
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(summary(operations[0]), std::string("subvol path=\"snap1\" uuid=aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa ctransid=9"));
    CHECK_EQ(summary(operations[1]), std::string("rename path=\"d1/x\" path_to=\"d1/y\""));
    CHECK_EQ(summary(operations[2]), std::string("set_xattr path=\"f\\\"quote\\\".txt\" xattr_name=\"user.binary\" xattr_data=<2 bytes>"));
    CHECK_EQ(summary(operations[3]), std::string("end"));
}

TEST_CASE(operation_path_helper)
{
    StreamBuilder builder(1);
    put_subvol(builder, "subvol");
    builder.begin_command(Command::Unlink);
    builder.put_string(Attribute::Path, "gone.txt");
    builder.end_command();
    put_end(builder);

    const std::vector<SendOperation> operations = parse_bytes(builder.bytes());
    CHECK_EQ(operation_path(operations[0]), std::string_view("subvol"));
    CHECK_EQ(operation_path(operations[1]), std::string_view("gone.txt"));
    CHECK_EQ(operation_path(operations[2]), std::string_view());
}
