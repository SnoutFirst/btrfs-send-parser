#include "btrfs/send/stream.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

namespace btrfs::send {

std::size_t SendStream::count(protocol::Command command) const noexcept
{
    std::size_t result = 0;
    for (const SendOperation& operation : operations)
    {
        if (operation.command == command)
            ++result;
    }
    return result;
}

const SendOperation* SendStream::first(protocol::Command command) const noexcept
{
    for (const SendOperation& operation : operations)
    {
        if (operation.command == command)
            return &operation;
    }
    return nullptr;
}

SendStream parse(ByteSource& source, Parser::Options options)
{
    SendStream stream;
    Parser parser(source, options);
    while (std::optional<SendOperation> operation = parser.next())
    {
        operation->materialize();
        stream.operations.push_back(std::move(*operation));
    }
    stream.version = parser.stream_version();
    return stream;
}

SendStream parse(std::istream& source, Parser::Options options)
{
    IstreamSource byte_source(source);
    return parse(byte_source, options);
}

SendStream parse_fd(int descriptor, Parser::Options options)
{
    FdSource byte_source(descriptor);
    return parse(byte_source, options);
}

SendStream parse_file(const std::string& path, Parser::Options options)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot open " + path);
    return parse(file, options);
}

std::vector<ChangedExtent> changed_extents(const SendStream& stream)
{
    std::vector<ChangedExtent> result;
    for (const SendOperation& operation : stream.operations)
    {
        if (const UpdateExtentOperation* update = operation.as<UpdateExtentOperation>())
            result.push_back(ChangedExtent{update->path, update->file_offset, update->length});
    }
    return result;
}

}
