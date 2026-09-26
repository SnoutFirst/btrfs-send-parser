#include "framework.hpp"
#include "stream_builder.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "btrfs/send.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace btrfs::send;
using testfw::StreamBuilder;
using protocol::Attribute;
using protocol::Command;

namespace {

const Uuid kUuidA = testfw::make_uuid(0xaa);

//Contains every byte class a payload may hold: NUL, a byte with the high bit set, and spaces.
const std::string kBinaryPayload = std::string("\x00\x01\xff binary\x00", 11);

void put_subvol(StreamBuilder& builder, std::string_view path = "subvol")
{
    builder.begin_command(Command::Subvol);
    builder.put_string(Attribute::Path, path);
    builder.put_uuid(Attribute::Uuid, kUuidA);
    builder.put_u64(Attribute::CtransId, 7);
    builder.end_command();
}

void put_write(StreamBuilder& builder, std::string_view path, std::uint64_t file_offset, std::string_view data)
{
    builder.begin_command(Command::Write);
    builder.put_string(Attribute::Path, path);
    builder.put_u64(Attribute::FileOffset, file_offset);
    //From stream version 2 on DATA carries no length field and runs to the end of the payload, so writing it
    //as a normal TLV would make the parser include the length field in the value.
    if (builder.version() >= protocol::kVersionEncodedIo)
        builder.put_lengthless_data(data);
    else
        builder.put_bytes(Attribute::Data, data);
    builder.end_command();
}

void put_end(StreamBuilder& builder)
{
    builder.begin_command(Command::End);
    builder.end_command();
}

//One subvolume, one write whose payload is not text, one end.
StreamBuilder make_stream(std::uint32_t version = 2)
{
    StreamBuilder builder(version);
    put_subvol(builder);
    put_write(builder, "f1.bin", 4096, kBinaryPayload);
    put_end(builder);
    return builder;
}

SendStream parse_bytes(const std::string& bytes, Parser::Options options = {})
{
    MemorySource source(bytes);
    return parse(source, options);
}

//The same view of a stream no matter which byte source produced it.
std::vector<std::string> summaries(const SendStream& stream)
{
    std::vector<std::string> result;
    result.reserve(stream.operations.size());
    for (const SendOperation& operation : stream.operations)
        result.push_back(summary(operation));
    return result;
}

//A file in the current directory that removes itself, so these cases leave nothing behind.
class TempFile
{
public:
    TempFile()
    {
        static unsigned long long counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_Path = "btrfs-send-parser-test-" + std::to_string(stamp) + "-" + std::to_string(counter++) + ".tmp";
    }

    ~TempFile()
    {
        std::remove(m_Path.c_str());
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const
    {
        return m_Path;
    }

    void write(const std::string& bytes) const
    {
        std::ofstream file(m_Path, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

private:
    std::string m_Path;
};

}

TEST_CASE(parse_from_a_memory_source)
{
    const StreamBuilder builder = make_stream(2);
    MemorySource source(builder.bytes());

    CHECK_EQ(source.size(), builder.bytes().size());
    CHECK_EQ(source.position(), std::size_t(0));

    const SendStream stream = parse(source);
    CHECK_EQ(stream.version, std::uint32_t(2));
    CHECK_EQ(stream.operations.size(), std::size_t(3));
    CHECK_EQ(source.position(), builder.bytes().size());

    const std::vector<const WriteOperation*> writes = stream.of_type<WriteOperation>();
    CHECK_EQ(writes.size(), std::size_t(1));
    CHECK_EQ(writes[0]->path, std::string("f1.bin"));
    CHECK_EQ(writes[0]->file_offset, std::uint64_t(4096));
    CHECK_EQ(writes[0]->data.size(), kBinaryPayload.size());
    CHECK_EQ(writes[0]->data.to_string(), kBinaryPayload);
    //parse() detaches payloads, so they outlive the parser's command buffer.
    CHECK(writes[0]->data.owns_data());
}

TEST_CASE(parse_from_an_istream)
{
    const StreamBuilder builder = make_stream(1);
    std::istringstream input(builder.bytes(), std::ios::in | std::ios::binary);

    const SendStream stream = parse(input);
    CHECK_EQ(stream.version, std::uint32_t(1));
    CHECK_EQ(summaries(stream), summaries(parse_bytes(builder.bytes())));

    const std::vector<const WriteOperation*> writes = stream.of_type<WriteOperation>();
    CHECK_EQ(writes.size(), std::size_t(1));
    //An istream must not be treated as a text stream: the NUL and the 0xff byte have to survive.
    CHECK_EQ(writes[0]->data.to_string(), kBinaryPayload);
}

TEST_CASE(parse_from_a_file_path)
{
    const TempFile file;
    file.write(make_stream(2).bytes());

    const SendStream stream = parse_file(file.path());
    CHECK_EQ(stream.version, std::uint32_t(2));
    CHECK_EQ(stream.operations.size(), std::size_t(3));

    bool threw = false;
    try
    {
        (void)parse_file("this-file-does-not-exist.bin");
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE(file_source_and_memory_source_agree)
{
    const StreamBuilder builder = make_stream(2);
    const TempFile file;
    file.write(builder.bytes());

    std::FILE* handle = std::fopen(file.path().c_str(), "rb");
    CHECK(handle != nullptr);
    FileSource source(handle);
    const SendStream from_file = parse(source);
    CHECK_EQ(std::fclose(handle), 0);

    const SendStream from_memory = parse_bytes(builder.bytes());
    CHECK_EQ(summaries(from_file), summaries(from_memory));
}

#if !defined(_WIN32)

TEST_CASE(parse_from_a_file_descriptor)
{
    const TempFile file;
    file.write(make_stream(2).bytes());

    const int descriptor = ::open(file.path().c_str(), O_RDONLY);
    CHECK(descriptor >= 0);
    const SendStream stream = parse_fd(descriptor);
    CHECK_EQ(::close(descriptor), 0);

    CHECK_EQ(stream.operations.size(), std::size_t(3));
    CHECK_EQ(stream.count(Command::Subvol), std::size_t(1));
    CHECK_EQ(stream.of_type<WriteOperation>()[0]->data.to_string(), kBinaryPayload);
}

TEST_CASE(parse_from_a_pipe_descriptor)
{
    //A pipe hands out whatever a single write(2) stored, so this is a source that produces short reads
    //without any help from ChunkedSource.
    const std::string bytes = make_stream(2).bytes();
    int descriptors[2] = {-1, -1};
    CHECK_EQ(::pipe(descriptors), 0);

    std::size_t written = 0;
    while (written < bytes.size())
    {
        const ssize_t got = ::write(descriptors[1], bytes.data() + written, bytes.size() - written);
        CHECK(got > 0);
        written += static_cast<std::size_t>(got);
    }
    CHECK_EQ(::close(descriptors[1]), 0);

    const SendStream stream = parse_fd(descriptors[0]);
    CHECK_EQ(::close(descriptors[0]), 0);

    CHECK_EQ(stream.operations.size(), std::size_t(3));
    CHECK_EQ(stream.of_type<WriteOperation>()[0]->data.to_string(), kBinaryPayload);
}

#endif

TEST_CASE(stream_helpers_index_the_operations)
{
    StreamBuilder builder(2);
    put_subvol(builder, "subvol");
    put_write(builder, "first.bin", 0, "one");
    put_write(builder, "second.bin", 10, "two");
    put_end(builder);

    const SendStream stream = parse_bytes(builder.bytes());
    CHECK_EQ(stream.count(Command::Write), std::size_t(2));
    CHECK_EQ(stream.count(Command::Mkdir), std::size_t(0));

    const SendOperation* first_end = stream.first(Command::End);
    CHECK(first_end != nullptr);
    CHECK_EQ(first_end->index, std::uint64_t(4));
    CHECK(stream.first(Command::Symlink) == nullptr);

    const std::vector<const WriteOperation*> writes = stream.of_type<WriteOperation>();
    CHECK_EQ(writes.size(), std::size_t(2));
    CHECK_EQ(writes[0]->path, std::string("first.bin"));
    CHECK_EQ(writes[1]->path, std::string("second.bin"));
    CHECK_EQ(writes[1]->file_offset, std::uint64_t(10));
    CHECK_EQ(writes[1]->data.to_string(), std::string("two"));
}

TEST_CASE(changed_extents_helper_collects_update_extents)
{
    StreamBuilder builder(2);
    put_subvol(builder, "snap");
    builder.begin_command(Command::UpdateExtent);
    builder.put_string(Attribute::Path, "changed.bin");
    builder.put_u64(Attribute::FileOffset, 1048576);
    builder.put_u64(Attribute::Size, 4096);
    builder.end_command();
    builder.begin_command(Command::UpdateExtent);
    builder.put_string(Attribute::Path, "other.bin");
    builder.put_u64(Attribute::FileOffset, 0);
    builder.put_u64(Attribute::Size, 512);
    builder.end_command();
    put_end(builder);

    const std::vector<ChangedExtent> extents = changed_extents(parse_bytes(builder.bytes()));
    CHECK_EQ(extents.size(), std::size_t(2));
    CHECK_EQ(extents[0].path, std::string("changed.bin"));
    CHECK_EQ(extents[0].offset, std::uint64_t(1048576));
    CHECK_EQ(extents[0].length, std::uint64_t(4096));
    CHECK_EQ(extents[1].path, std::string("other.bin"));
    CHECK_EQ(extents[1].length, std::uint64_t(512));

    //A data carrying stream has no update extents at all.
    CHECK(changed_extents(parse_bytes(make_stream(2).bytes())).empty());
}

TEST_CASE(concatenated_streams_report_the_last_version)
{
    StreamBuilder builder(1);
    put_subvol(builder, "first");
    put_end(builder);
    //The second stream is a complete stream of its own, header included: this is what "btrfs send" writes
    //for several subvolumes without -e.
    builder.append_stream_header(2);
    put_subvol(builder, "second");
    put_end(builder);

    const SendStream stream = parse_bytes(builder.bytes());
    CHECK_EQ(stream.version, std::uint32_t(2));
    CHECK_EQ(stream.count(Command::Subvol), std::size_t(2));
    CHECK_EQ(stream.count(Command::End), std::size_t(2));
    CHECK_EQ(operation_path(stream.operations[0]), std::string_view("first"));
    CHECK_EQ(operation_path(stream.operations[2]), std::string_view("second"));

    Parser::Options strict;
    strict.allow_concatenated_streams = false;
    bool rejected = false;
    try
    {
        (void)parse_bytes(builder.bytes(), strict);
    }
    catch (const ParseError& error)
    {
        rejected = error.code() == ErrorCode::UnknownCommand;
    }
    CHECK(rejected);
}

TEST_CASE(materialize_detaches_payloads_from_the_parser)
{
    const StreamBuilder builder = make_stream(2);
    MemorySource source(builder.bytes());
    Parser parser(source);

    //The stream opens with SUBVOL, which carries no payload view, so the parser is advanced to the WRITE.
    std::optional<SendOperation> operation = parser.next();
    CHECK(operation.has_value());
    CHECK(operation->is<SubvolOperation>());

    operation = parser.next();
    CHECK(operation.has_value());
    WriteOperation* write = operation->as<WriteOperation>();
    CHECK(write != nullptr);
    CHECK(!write->data.owns_data());

    //Before materialize() the payload points into the parser's command buffer.
    operation->materialize();
    CHECK(write->data.owns_data());
    CHECK_EQ(write->data.to_string(), kBinaryPayload);

    //Calling it twice must not copy again, and the bytes must survive the next command.
    operation->materialize();
    const std::byte* before = write->data.data();
    operation->materialize();
    CHECK(write->data.data() == before);

    SendOperation next_operation;
    ParseFailure failure;
    CHECK_EQ(parser.next(next_operation, failure), NextStatus::HaveOperation);
    CHECK(next_operation.is<EndOperation>());
    CHECK_EQ(write->data.to_string(), kBinaryPayload);
}
