#pragma once

//Convenience layer on top of Parser: parse a whole stream into memory, with every operation detached from
//the parser buffer. Use this for streams that fit in memory; use Parser directly for large ones.

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "btrfs/send/byte_source.hpp"
#include "btrfs/send/operations.hpp"
#include "btrfs/send/parser.hpp"

namespace btrfs::send {

//A complete stream, in order, with owned payloads.
struct SendStream
{
    std::uint32_t version = 0;
    std::vector<SendOperation> operations;

    std::size_t count(protocol::Command command) const noexcept;

    const SendOperation* first(protocol::Command command) const noexcept;

    //All operations of one type, in stream order, for example stream.of_type<WriteOperation>().
    template <typename T>
    std::vector<const T*> of_type() const
    {
        std::vector<const T*> result;
        for (const SendOperation& operation : operations)
        {
            if (const T* typed = operation.as<T>())
                result.push_back(typed);
        }
        return result;
    }
};

SendStream parse(ByteSource& source, Parser::Options options = {});
SendStream parse(std::istream& stream, Parser::Options options = {});
SendStream parse_fd(int descriptor, Parser::Options options = {});

//Throws std::runtime_error when the file cannot be opened.
SendStream parse_file(const std::string& path, Parser::Options options = {});

//An extent whose data changed but was not transferred, as reported by BTRFS_SEND_C_UPDATE_EXTENT.
//A "btrfs send --no-data" stream describes file changes exclusively through these entries.
struct ChangedExtent
{
    std::string path;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

//Convenience for the --no-data case: every changed extent of every file in the stream.
std::vector<ChangedExtent> changed_extents(const SendStream& stream);

}
