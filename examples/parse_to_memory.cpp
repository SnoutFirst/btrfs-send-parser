//Parses a stream into memory and prints what it found.
//
//    btrfs send --proto 2 /snap | ./btrfs-send-example-parse_to_memory
//
//parse() reads every command, detaches each payload from the parser's buffer and returns one SendStream, so
//the caller can index into the result, call count() and of_type<T>() and keep the data around. Use the Parser
//directly (see walk_stream.cpp) when the stream is too large to hold in memory.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "btrfs/send.hpp"

using namespace btrfs::send;

namespace {

void print_counts(const SendStream& stream)
{
    const protocol::Command commands[] = {
        protocol::Command::Subvol,
        protocol::Command::Snapshot,
        protocol::Command::Mkfile,
        protocol::Command::Mkdir,
        protocol::Command::Mknod,
        protocol::Command::Mkfifo,
        protocol::Command::Mksock,
        protocol::Command::Symlink,
        protocol::Command::Rename,
        protocol::Command::Link,
        protocol::Command::Unlink,
        protocol::Command::Rmdir,
        protocol::Command::SetXattr,
        protocol::Command::RemoveXattr,
        protocol::Command::Write,
        protocol::Command::Clone,
        protocol::Command::Truncate,
        protocol::Command::Chmod,
        protocol::Command::Chown,
        protocol::Command::Utimes,
        protocol::Command::End,
        protocol::Command::UpdateExtent,
        protocol::Command::Fallocate,
        protocol::Command::Fileattr,
        protocol::Command::EncodedWrite,
        protocol::Command::EnableVerity,
    };

    for (const protocol::Command command : commands)
    {
        const std::size_t count = stream.count(command);
        if (count != 0)
            std::cout << "  " << protocol::to_string(command) << " " << count << "\n";
    }
}

}

int main()
{
    SendStream stream;
    try
    {
        stream = parse(std::cin);
    }
    catch (const ParseError& error)
    {
        std::cerr << describe(error.failure()) << "\n";
        return 1;
    }

    std::cout << "stream version " << stream.version << "\n"
              << "operations " << stream.operations.size() << "\n"
              << "commands by name:\n";
    print_counts(stream);

    //The first subvolume tells you what the stream is about.
    if (const SendOperation* first = stream.first(protocol::Command::Subvol))
    {
        const SubvolOperation* subvol = first->as<SubvolOperation>();
        std::cout << "first subvolume: " << subvol->path << " uuid " << subvol->uuid.to_string() << " ctransid " << subvol->ctrans_id << "\n";
    }

    //of_type<T>() walks the same vector and returns the typed pointers, so counting by hand is not needed.
    const std::vector<const WriteOperation*> writes = stream.of_type<WriteOperation>();
    std::uint64_t written = 0;
    for (const WriteOperation* write : writes)
    {
        written += write->data.size();
        //Payloads survive the next call to Parser::next() because parse() materialized them.
        if (!write->data.owns_data())
            std::cerr << "payload of " << write->path << " is not owned, this should not happen\n";
    }
    std::cout << "file data: " << written << " bytes in " << writes.size() << " write commands\n";
    return 0;
}
