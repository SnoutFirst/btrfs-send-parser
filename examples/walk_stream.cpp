//Prints the contents of a send stream while it is being read, one line per operation.
//
//Nothing is buffered beyond a single command, so this is the shape you want for a stream that does not fit in
//memory:
//
//    btrfs send /snap | ./btrfs-send-example-walk_stream
//    ./btrfs-send-example-walk_stream stream.bin
//
//The detail line for each command comes from the typed operation, not from re-reading attribute values, so
//this also shows the shape of the decoded API.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "btrfs/send.hpp"

namespace {

using namespace btrfs::send;

std::string join(const std::vector<std::string>& parts)
{
    std::string result;
    for (const std::string& part : parts)
    {
        if (!result.empty())
            result += ", ";
        result += part;
    }
    return result;
}

//Extra detail for commands where the interesting part is not just the path. Anything not listed prints
//nothing, which keeps the generic line readable.
std::string detail_of(const SendOperation& operation)
{
    return std::visit(
        [](const auto& typed) -> std::string {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, SnapshotOperation>)
                return "parent " + typed.clone_uuid.to_string() + " at ctransid " + std::to_string(typed.clone_ctrans_id);
            else if constexpr (std::is_same_v<T, WriteOperation>)
                return std::to_string(typed.data.size()) + " bytes at offset " + std::to_string(typed.file_offset);
            else if constexpr (std::is_same_v<T, EncodedWriteOperation>)
                return std::to_string(typed.data.size()) + " encoded bytes for " + std::to_string(typed.unencoded_file_len) + " file bytes, " + flags::to_string(static_cast<flags::Compression>(typed.compression));
            else if constexpr (std::is_same_v<T, CloneOperation>)
                return std::to_string(typed.length) + " bytes from \"" + typed.clone_path + "\" at " + std::to_string(typed.clone_offset);
            else if constexpr (std::is_same_v<T, UpdateExtentOperation>)
                return "changed without data: offset " + std::to_string(typed.file_offset) + ", length " + std::to_string(typed.length);
            else if constexpr (std::is_same_v<T, SetXattrOperation>)
                return typed.name + ", " + std::to_string(typed.value.size()) + " value bytes";
            else if constexpr (std::is_same_v<T, FileattrOperation>)
            {
                const std::vector<std::string> names = flags::inode_flag_names(typed.fileattr);
                return "fileattr 0x" + std::to_string(typed.fileattr) + (names.empty() ? std::string() : " (" + join(names) + ")");
            }
            else if constexpr (std::is_same_v<T, FallocateOperation>)
            {
                const std::vector<std::string> names = flags::fallocate_mode_names(typed.mode);
                return "mode " + std::to_string(typed.mode) + (names.empty() ? std::string() : " (" + join(names) + ")") + ", length " + std::to_string(typed.length);
            }
            else if constexpr (std::is_same_v<T, MknodOperation>)
                return "mode " + std::to_string(typed.mode) + ", rdev " + std::to_string(typed.rdev);
            else if constexpr (std::is_same_v<T, UnknownCommandOperation>)
                return std::to_string(typed.payload.size()) + " bytes of an undefined command";
            else
                return {};
        },
        operation.operation);
}

int walk(ByteSource& source)
{
    Parser::Options options;
    //The kernel may be newer than this build, in which case unknown attributes arrive. Keeping them is
    //cheaper than failing the whole stream, and they stay visible in operation.attributes.
    options.allow_unexpected_attributes = true;

    Parser parser(source, options);
    SendOperation operation;
    ParseFailure failure;

    for (;;)
    {
        const NextStatus status = parser.next(operation, failure);
        if (status == NextStatus::EndOfStream)
            break;
        if (status == NextStatus::Error)
        {
            std::cerr << describe(failure) << "\n";
            return 1;
        }

        std::cout << operation.index << ": " << protocol::to_string(operation.command);
        const std::string_view path = operation_path(operation);
        if (!path.empty())
            std::cout << " " << path;
        const std::string detail = detail_of(operation);
        if (!detail.empty())
            std::cout << " (" << detail << ")";
        std::cout << "\n";
    }

    std::cout << "stream version " << parser.stream_version()
              << ", " << parser.commands_parsed() << " commands"
              << ", " << parser.bytes_consumed() << " bytes\n";
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "-"))
    {
        FileSource source(stdin);
        return walk(source);
    }
    if (argc == 2)
    {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file)
        {
            std::cerr << "cannot open " << argv[1] << "\n";
            return 1;
        }
        IstreamSource source(file);
        return walk(source);
    }

    std::cerr << "usage: " << argv[0] << " [file]\n"
              << "reads standard input when no file is given\n";
    return 2;
}
