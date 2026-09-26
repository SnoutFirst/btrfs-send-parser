#include "btrfs/send/byte_source.hpp"

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace btrfs::send {

MemorySource::MemorySource(const void* data, std::size_t size) noexcept
    : m_Data(static_cast<const std::byte*>(data))
    , m_Size(size)
{
}

MemorySource::MemorySource(std::string_view text) noexcept
    : m_Data(reinterpret_cast<const std::byte*>(text.data()))
    , m_Size(text.size())
{
}

MemorySource::MemorySource(const std::vector<std::byte>& bytes) noexcept
    : m_Data(bytes.data())
    , m_Size(bytes.size())
{
}

std::size_t MemorySource::read(std::byte* out, std::size_t size)
{
    const std::size_t remaining = m_Size - m_Position;
    const std::size_t count = std::min(size, remaining);
    if (count != 0)
    {
        std::copy_n(m_Data + m_Position, count, out);
        m_Position += count;
    }
    return count;
}

std::size_t MemorySource::position() const noexcept
{
    return m_Position;
}

std::size_t MemorySource::size() const noexcept
{
    return m_Size;
}

IstreamSource::IstreamSource(std::istream& stream) noexcept
    : m_Stream(&stream)
{
}

std::size_t IstreamSource::read(std::byte* out, std::size_t size)
{
    if (size == 0)
        return 0;
    m_Stream->read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
    const std::streamsize got = m_Stream->gcount();
    if (got <= 0)
    {
        if (m_Stream->bad())
            throw std::runtime_error("read from std::istream failed");
        return 0;
    }
    return static_cast<std::size_t>(got);
}

FdSource::FdSource(int descriptor) noexcept
    : m_Descriptor(descriptor)
{
}

std::size_t FdSource::read(std::byte* out, std::size_t size)
{
    if (size == 0)
        return 0;
    for (;;)
    {
#if defined(_WIN32)
        const int got = ::_read(m_Descriptor, out, static_cast<unsigned int>(size));
#else
        const ssize_t got = ::read(m_Descriptor, out, size);
#endif
        if (got > 0)
            return static_cast<std::size_t>(got);
        if (got == 0)
            return 0;
        if (errno == EINTR)
            continue;
        throw std::system_error(errno, std::generic_category(), "read from file descriptor failed");
    }
}

FileSource::FileSource(std::FILE* file) noexcept
    : m_File(file)
{
}

std::size_t FileSource::read(std::byte* out, std::size_t size)
{
    if (size == 0)
        return 0;
    const std::size_t got = std::fread(out, 1, size, m_File);
    if (got == 0 && std::ferror(m_File) != 0)
        throw std::runtime_error("read from FILE stream failed");
    return got;
}

ChunkedSource::ChunkedSource(ByteSource& source, std::size_t max_chunk) noexcept
    : m_Source(&source)
    , m_MaxChunk(max_chunk == 0 ? 1 : max_chunk)
{
}

std::size_t ChunkedSource::read(std::byte* out, std::size_t size)
{
    return m_Source->read(out, std::min(size, m_MaxChunk));
}

std::size_t ChunkedSource::max_chunk() const noexcept
{
    return m_MaxChunk;
}

}
