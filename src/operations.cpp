#include "btrfs/send/operations.hpp"

#include <type_traits>

namespace btrfs::send {

namespace {

//Appends a field in the "name=value" form used by summary(), quoting and escaping strings.
void append_string_field(std::string& out, const char* name, std::string_view value)
{
    out += ' ';
    out += name;
    out += "=\"";
    for (const char character : value)
    {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20 || byte == 0x7f)
            {
                static constexpr char kHexDigits[] = "0123456789abcdef";
                out += "\\x";
                out += kHexDigits[(byte >> 4) & 0xfu];
                out += kHexDigits[byte & 0xfu];
            }
            else
            {
                out += character;
            }
            break;
        }
    }
    out += '"';
}

void append_number_field(std::string& out, const char* name, std::uint64_t value)
{
    out += ' ';
    out += name;
    out += '=';
    out += std::to_string(value);
}

void append_uuid_field(std::string& out, const char* name, const Uuid& uuid)
{
    out += ' ';
    out += name;
    out += '=';
    out += uuid.to_string();
}

void append_timestamp_field(std::string& out, const char* name, const Timestamp& timestamp)
{
    out += ' ';
    out += name;
    out += '=';
    out += timestamp.to_iso8601_utc();
}

void append_bytes_field(std::string& out, const char* name, const BinaryData& data)
{
    out += ' ';
    out += name;
    out += "=<";
    out += std::to_string(data.size());
    out += " bytes>";
}

}

const char* protocol::to_string(Command command) noexcept
{
    switch (command)
    {
    case Command::Unspec:
        return "unspec";
    case Command::Subvol:
        return "subvol";
    case Command::Snapshot:
        return "snapshot";
    case Command::Mkfile:
        return "mkfile";
    case Command::Mkdir:
        return "mkdir";
    case Command::Mknod:
        return "mknod";
    case Command::Mkfifo:
        return "mkfifo";
    case Command::Mksock:
        return "mksock";
    case Command::Symlink:
        return "symlink";
    case Command::Rename:
        return "rename";
    case Command::Link:
        return "link";
    case Command::Unlink:
        return "unlink";
    case Command::Rmdir:
        return "rmdir";
    case Command::SetXattr:
        return "set_xattr";
    case Command::RemoveXattr:
        return "remove_xattr";
    case Command::Write:
        return "write";
    case Command::Clone:
        return "clone";
    case Command::Truncate:
        return "truncate";
    case Command::Chmod:
        return "chmod";
    case Command::Chown:
        return "chown";
    case Command::Utimes:
        return "utimes";
    case Command::End:
        return "end";
    case Command::UpdateExtent:
        return "update_extent";
    case Command::Fallocate:
        return "fallocate";
    case Command::Fileattr:
        return "fileattr";
    case Command::EncodedWrite:
        return "encoded_write";
    case Command::EnableVerity:
        return "enable_verity";
    }
    return "unknown";
}

const char* protocol::to_string(Attribute attribute) noexcept
{
    switch (attribute)
    {
    case Attribute::Unspec:
        return "unspec";
    case Attribute::Uuid:
        return "uuid";
    case Attribute::CtransId:
        return "ctransid";
    case Attribute::Ino:
        return "ino";
    case Attribute::Size:
        return "size";
    case Attribute::Mode:
        return "mode";
    case Attribute::Uid:
        return "uid";
    case Attribute::Gid:
        return "gid";
    case Attribute::Rdev:
        return "rdev";
    case Attribute::Ctime:
        return "ctime";
    case Attribute::Mtime:
        return "mtime";
    case Attribute::Atime:
        return "atime";
    case Attribute::Otime:
        return "otime";
    case Attribute::XattrName:
        return "xattr_name";
    case Attribute::XattrData:
        return "xattr_data";
    case Attribute::Path:
        return "path";
    case Attribute::PathTo:
        return "path_to";
    case Attribute::PathLink:
        return "path_link";
    case Attribute::FileOffset:
        return "file_offset";
    case Attribute::Data:
        return "data";
    case Attribute::CloneUuid:
        return "clone_uuid";
    case Attribute::CloneCtransId:
        return "clone_ctransid";
    case Attribute::ClonePath:
        return "clone_path";
    case Attribute::CloneOffset:
        return "clone_offset";
    case Attribute::CloneLen:
        return "clone_len";
    case Attribute::FallocateMode:
        return "fallocate_mode";
    case Attribute::Fileattr:
        return "fileattr";
    case Attribute::UnencodedFileLen:
        return "unencoded_file_len";
    case Attribute::UnencodedLen:
        return "unencoded_len";
    case Attribute::UnencodedOffset:
        return "unencoded_offset";
    case Attribute::Compression:
        return "compression";
    case Attribute::Encryption:
        return "encryption";
    case Attribute::VerityAlgorithm:
        return "verity_algorithm";
    case Attribute::VerityBlockSize:
        return "verity_block_size";
    case Attribute::VeritySaltData:
        return "verity_salt_data";
    case Attribute::VeritySigData:
        return "verity_sig_data";
    }
    return "unknown";
}

const AttributeValue* SendOperation::attribute(protocol::Attribute type) const noexcept
{
    const AttributeValue* found = nullptr;
    for (const AttributeValue& attribute : attributes)
    {
        if (attribute.type == type)
            found = &attribute;
    }
    return found;
}

void SendOperation::materialize()
{
    for (AttributeValue& attribute : attributes)
        attribute.value.materialize();

    std::visit([](auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, SetXattrOperation>)
        {
            typed.value.materialize();
        }
        else if constexpr (std::is_same_v<T, WriteOperation> || std::is_same_v<T, EncodedWriteOperation>)
        {
            typed.data.materialize();
        }
        else if constexpr (std::is_same_v<T, UnknownCommandOperation>)
        {
            typed.payload.materialize();
        }
        else if constexpr (std::is_same_v<T, EnableVerityOperation>)
        {
            typed.salt.materialize();
            typed.signature.materialize();
        }
    }, operation);
}

std::string_view operation_path(const SendOperation& operation) noexcept
{
    return std::visit([](const auto& typed) -> std::string_view {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, EndOperation> || std::is_same_v<T, UnknownCommandOperation>)
            return {};
        else
            return typed.path;
    }, operation.operation);
}

std::string summary(const SendOperation& operation)
{
    std::string result = protocol::to_string(operation.command);
    std::visit([&result](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, SubvolOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_uuid_field(result, "uuid", typed.uuid);
            append_number_field(result, "ctransid", typed.ctrans_id);
        }
        else if constexpr (std::is_same_v<T, SnapshotOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_uuid_field(result, "uuid", typed.uuid);
            append_number_field(result, "ctransid", typed.ctrans_id);
            append_uuid_field(result, "clone_uuid", typed.clone_uuid);
            append_number_field(result, "clone_ctransid", typed.clone_ctrans_id);
        }
        else if constexpr (std::is_same_v<T, MkfileOperation> || std::is_same_v<T, MkdirOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "ino", typed.ino);
        }
        else if constexpr (std::is_same_v<T, MkfifoOperation> || std::is_same_v<T, MksockOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "ino", typed.ino);
            if (typed.rdev)
                append_number_field(result, "rdev", *typed.rdev);
            if (typed.mode)
                append_number_field(result, "mode", *typed.mode);
        }
        else if constexpr (std::is_same_v<T, MknodOperation>)
        {
            append_string_field(result, "path", typed.path);
            if (typed.ino)
                append_number_field(result, "ino", *typed.ino);
            append_number_field(result, "mode", typed.mode);
            append_number_field(result, "rdev", typed.rdev);
        }
        else if constexpr (std::is_same_v<T, SymlinkOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "ino", typed.ino);
            append_string_field(result, "path_link", typed.link_target);
        }
        else if constexpr (std::is_same_v<T, RenameOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_string_field(result, "path_to", typed.path_to);
        }
        else if constexpr (std::is_same_v<T, LinkOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_string_field(result, "path_link", typed.link_target);
        }
        else if constexpr (std::is_same_v<T, UnlinkOperation> || std::is_same_v<T, RmdirOperation>)
        {
            append_string_field(result, "path", typed.path);
        }
        else if constexpr (std::is_same_v<T, SetXattrOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_string_field(result, "xattr_name", typed.name);
            append_bytes_field(result, "xattr_data", typed.value);
        }
        else if constexpr (std::is_same_v<T, RemoveXattrOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_string_field(result, "xattr_name", typed.name);
        }
        else if constexpr (std::is_same_v<T, WriteOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "file_offset", typed.file_offset);
            append_bytes_field(result, "data", typed.data);
        }
        else if constexpr (std::is_same_v<T, CloneOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "file_offset", typed.file_offset);
            append_number_field(result, "clone_len", typed.length);
            append_uuid_field(result, "clone_uuid", typed.clone_uuid);
            append_number_field(result, "clone_ctransid", typed.clone_ctrans_id);
            append_string_field(result, "clone_path", typed.clone_path);
            append_number_field(result, "clone_offset", typed.clone_offset);
        }
        else if constexpr (std::is_same_v<T, TruncateOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "size", typed.size);
        }
        else if constexpr (std::is_same_v<T, ChmodOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "mode", typed.mode);
        }
        else if constexpr (std::is_same_v<T, ChownOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "uid", typed.uid);
            append_number_field(result, "gid", typed.gid);
        }
        else if constexpr (std::is_same_v<T, UtimesOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_timestamp_field(result, "atime", typed.atime);
            append_timestamp_field(result, "mtime", typed.mtime);
            append_timestamp_field(result, "ctime", typed.ctime);
            if (typed.otime)
                append_timestamp_field(result, "otime", *typed.otime);
        }
        else if constexpr (std::is_same_v<T, EndOperation>)
        {
        }
        else if constexpr (std::is_same_v<T, UpdateExtentOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "file_offset", typed.file_offset);
            append_number_field(result, "length", typed.length);
        }
        else if constexpr (std::is_same_v<T, FallocateOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "mode", typed.mode);
            append_number_field(result, "file_offset", typed.file_offset);
            append_number_field(result, "length", typed.length);
        }
        else if constexpr (std::is_same_v<T, FileattrOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "fileattr", typed.fileattr);
        }
        else if constexpr (std::is_same_v<T, EncodedWriteOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "file_offset", typed.file_offset);
            append_number_field(result, "unencoded_file_len", typed.unencoded_file_len);
            append_number_field(result, "unencoded_len", typed.unencoded_len);
            append_number_field(result, "unencoded_offset", typed.unencoded_offset);
            append_number_field(result, "compression", typed.compression);
            append_number_field(result, "encryption", typed.encryption);
            append_bytes_field(result, "data", typed.data);
        }
        else if constexpr (std::is_same_v<T, EnableVerityOperation>)
        {
            append_string_field(result, "path", typed.path);
            append_number_field(result, "algorithm", typed.algorithm);
            append_number_field(result, "block_size", typed.block_size);
            append_bytes_field(result, "salt", typed.salt);
            append_bytes_field(result, "signature", typed.signature);
        }
        else if constexpr (std::is_same_v<T, UnknownCommandOperation>)
        {
            append_number_field(result, "command", static_cast<std::uint16_t>(typed.command));
            append_bytes_field(result, "payload", typed.payload);
        }
    }, operation.operation);
    return result;
}

}
