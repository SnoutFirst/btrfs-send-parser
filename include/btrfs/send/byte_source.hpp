#pragma once

#include <cstddef>
#include <cstdio>
#include <istream>
#include <string_view>
#include <vector>

namespace btrfs::send {

//Abstract byte input for the parser.
//
//A source is free to return fewer bytes than requested at any time, and the parser copes with that,
//so a source that hands out whatever a single read(2) returned (a pipe, a socket, a decompressor) is fine.
//A return value of 0 means end of input.
//Sources report I/O failures by throwing; the parser converts any std::exception thrown by read() into an
//ErrorCode::SourceReadFailed failure, so a throwing implementation never takes the parser by surprise.
class ByteSource
{
public:
    ByteSource() = default;
    virtual ~ByteSource() = default;

    ByteSource(const ByteSource&) = delete;
    ByteSource& operator=(const ByteSource&) = delete;

    //Reads at most size bytes into out and returns how many were stored (0 only at end of input).
    virtual std::size_t read(std::byte* out, std::size_t size) = 0;
};

//Borrows a caller owned buffer, which must outlive the source.
class MemorySource final : public ByteSource
{
public:
    MemorySource(const void* data, std::size_t size) noexcept;
    explicit MemorySource(std::string_view text) noexcept;
    explicit MemorySource(const std::vector<std::byte>& bytes) noexcept;

    std::size_t read(std::byte* out, std::size_t size) override;

    std::size_t position() const noexcept;
    std::size_t size() const noexcept;

private:
    const std::byte* m_Data;
    std::size_t m_Size;
    std::size_t m_Position = 0;
};

//Reads from a std::istream. Always uses read(), so a stream in text mode is the caller's problem.
class IstreamSource final : public ByteSource
{
public:
    explicit IstreamSource(std::istream& stream) noexcept;

    std::size_t read(std::byte* out, std::size_t size) override;

private:
    std::istream* m_Stream;
};

//Reads from a file descriptor, retrying on EINTR. The descriptor is neither owned nor closed.
class FdSource final : public ByteSource
{
public:
    explicit FdSource(int descriptor) noexcept;

    std::size_t read(std::byte* out, std::size_t size) override;

private:
    int m_Descriptor;
};

//Reads from a stdio stream. The stream is neither owned nor closed.
class FileSource final : public ByteSource
{
public:
    explicit FileSource(std::FILE* file) noexcept;

    std::size_t read(std::byte* out, std::size_t size) override;

private:
    std::FILE* m_File;
};

//Wraps another source and never returns more than max_chunk bytes per call.
//Handy for reproducing the short reads a pipe produces and for testing a consumer that must not
//assume a single read delivers a whole command.
class ChunkedSource final : public ByteSource
{
public:
    ChunkedSource(ByteSource& source, std::size_t max_chunk) noexcept;

    std::size_t read(std::byte* out, std::size_t size) override;

    std::size_t max_chunk() const noexcept;

private:
    ByteSource* m_Source;
    std::size_t m_MaxChunk;
};

}
