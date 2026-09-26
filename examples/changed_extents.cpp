//Lists the file extents that changed in a stream, which is what a "btrfs send --no-data" stream carries.
//
//    btrfs send --no-data -p /snap-old /snap-new > changes.bin
//    ./btrfs-send-example-changed_extents changes.bin
//
//Such a stream reports BTRFS_SEND_C_UPDATE_EXTENT instead of BTRFS_SEND_C_WRITE, so this output is the list a
//backup tool would use to decide what to re-read. A data carrying stream simply has no update extents, which
//the program reports as a note instead of an error.

#include <cstdint>
#include <iostream>
#include <string>

#include "btrfs/send.hpp"

using namespace btrfs::send;

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " <stream file>\n";
        return 2;
    }

    //parse_file() reads the whole stream and detaches every payload, so the result stays valid afterwards.
    SendStream stream;
    try
    {
        stream = parse_file(argv[1]);
    }
    catch (const ParseError& error)
    {
        std::cerr << argv[1] << ": " << describe(error.failure()) << "\n";
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << argv[1] << ": " << error.what() << "\n";
        return 1;
    }

    const std::vector<ChangedExtent> extents = changed_extents(stream);
    for (const ChangedExtent& extent : extents)
        std::cout << extent.path << " " << extent.offset << " " << extent.length << "\n";

    if (extents.empty())
    {
        const std::size_t writes = stream.count(protocol::Command::Write) + stream.count(protocol::Command::EncodedWrite);
        if (writes != 0)
            std::cerr << "no update extents: this stream carries file data (" << writes << " write commands)\n";
        else
            std::cerr << "no update extents and no file data in this stream\n";
    }

    std::cout << "stream version " << stream.version << ", " << stream.operations.size() << " commands, " << extents.size() << " changed extents\n";
    return 0;
}
