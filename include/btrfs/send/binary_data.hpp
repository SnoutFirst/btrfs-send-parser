#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace btrfs::send {

//A byte blob taken from the stream. It is a view into the parser's internal command buffer by default,
//so it is valid until Parser::next() is called again; call materialize() to detach it from that buffer
//(parse() and the helpers in stream.hpp do that for you).
class BinaryData
{
public:
    using Container = std::vector<std::byte>;

    BinaryData() = default;

    static BinaryData view(const std::byte* data, std::size_t size) noexcept
    {
        BinaryData result;
        result.m_Data = data;
        result.m_Size = size;
        return result;
    }

    const std::byte* data() const noexcept
    {
        return m_Data;
    }

    std::size_t size() const noexcept
    {
        return m_Size;
    }

    bool empty() const noexcept
    {
        return m_Size == 0;
    }

    bool owns_data() const noexcept
    {
        return static_cast<bool>(m_Owner);
    }

    //Raw bytes reinterpreted as characters. Payloads are arbitrary binary data, so no terminator is added.
    std::string_view as_string_view() const noexcept
    {
        if (m_Size == 0)
            return {};
        return std::string_view(reinterpret_cast<const char*>(m_Data), m_Size);
    }

    const std::byte* begin() const noexcept
    {
        return m_Data;
    }

    const std::byte* end() const noexcept
    {
        return m_Data + m_Size;
    }

    std::vector<std::byte> to_vector() const
    {
        if (m_Size == 0)
            return {};
        return Container(m_Data, m_Data + m_Size);
    }

    std::string to_string() const
    {
        return std::string(as_string_view());
    }

    //Copies the viewed bytes into storage owned by this object. Cheap when the data is already owned.
    void materialize()
    {
        if (m_Size == 0 || m_Owner)
            return;
        auto owner = std::make_shared<Container>(m_Data, m_Data + m_Size);
        m_Data = owner->data();
        m_Owner = std::move(owner);
    }

private:
    const std::byte* m_Data = nullptr;
    std::size_t m_Size = 0;
    std::shared_ptr<const Container> m_Owner;
};

}
