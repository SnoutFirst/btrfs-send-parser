#pragma once

//Typed representation of the decoded operations.
//
//The parser hands out a SendOperation per command: the wire command id, the typed payload of that command,
//every attribute of the command in stream order, and where the command sits in the stream.
//
//Strings are copied into std::string, binary payloads are BinaryData views that stay valid until the next
//call to Parser::next(). SendOperation::materialize() detaches the binary payloads so the operation can be
//stored and used after the parser moved on.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "btrfs/send/binary_data.hpp"
#include "btrfs/send/protocol.hpp"
#include "btrfs/send/types.hpp"

namespace btrfs::send {

//One attribute of a command, in the order it appeared in the payload.
struct AttributeValue
{
    protocol::Attribute type = protocol::Attribute::Unspec;
    BinaryData value;
};

//BTRFS_SEND_C_SUBVOL: start of a full send of a subvolume.
struct SubvolOperation
{
    std::string path;
    Uuid uuid;
    std::uint64_t ctrans_id = 0;
};

//BTRFS_SEND_C_SNAPSHOT: start of an incremental send, clone_uuid/clone_ctrans_id identify the parent.
struct SnapshotOperation
{
    std::string path;
    Uuid uuid;
    std::uint64_t ctrans_id = 0;
    Uuid clone_uuid;
    std::uint64_t clone_ctrans_id = 0;
};

//BTRFS_SEND_C_MKFILE.
struct MkfileOperation
{
    std::string path;
    std::uint64_t ino = 0;
};

//BTRFS_SEND_C_MKDIR.
struct MkdirOperation
{
    std::string path;
    std::uint64_t ino = 0;
};

//BTRFS_SEND_C_MKNOD. The published format lists only path, mode and rdev, but the kernel has always sent
//the inode number as well, so it is optional here.
struct MknodOperation
{
    std::string path;
    std::optional<std::uint64_t> ino;
    std::uint64_t mode = 0;
    std::uint64_t rdev = 0;
};

//BTRFS_SEND_C_MKFIFO. The published format lists path and ino, the kernel additionally sends rdev and
//mode, which are exposed here when they are present.
struct MkfifoOperation
{
    std::string path;
    std::uint64_t ino = 0;
    std::optional<std::uint64_t> rdev;
    std::optional<std::uint64_t> mode;
};

//BTRFS_SEND_C_MKSOCK, same attribute set as MKFIFO.
struct MksockOperation
{
    std::string path;
    std::uint64_t ino = 0;
    std::optional<std::uint64_t> rdev;
    std::optional<std::uint64_t> mode;
};

//BTRFS_SEND_C_SYMLINK.
struct SymlinkOperation
{
    std::string path;
    std::uint64_t ino = 0;
    std::string link_target;
};

//BTRFS_SEND_C_RENAME.
struct RenameOperation
{
    std::string path;
    std::string path_to;
};

//BTRFS_SEND_C_LINK: hard link path to an existing entry, the new name is in link_target.
struct LinkOperation
{
    std::string path;
    std::string link_target;
};

//BTRFS_SEND_C_UNLINK.
struct UnlinkOperation
{
    std::string path;
};

//BTRFS_SEND_C_RMDIR.
struct RmdirOperation
{
    std::string path;
};

//BTRFS_SEND_C_SET_XATTR. The value is arbitrary binary data and is not NUL terminated.
struct SetXattrOperation
{
    std::string path;
    std::string name;
    BinaryData value;
};

//BTRFS_SEND_C_REMOVE_XATTR.
struct RemoveXattrOperation
{
    std::string path;
    std::string name;
};

//BTRFS_SEND_C_WRITE: file data at an offset.
struct WriteOperation
{
    std::string path;
    std::uint64_t file_offset = 0;
    BinaryData data;
};

//BTRFS_SEND_C_CLONE: copy a range of an extent from another file of another subvolume.
struct CloneOperation
{
    std::string path;
    std::uint64_t file_offset = 0;
    std::uint64_t length = 0;
    Uuid clone_uuid;
    std::uint64_t clone_ctrans_id = 0;
    std::string clone_path;
    std::uint64_t clone_offset = 0;
};

//BTRFS_SEND_C_TRUNCATE.
struct TruncateOperation
{
    std::string path;
    std::uint64_t size = 0;
};

//BTRFS_SEND_C_CHMOD.
struct ChmodOperation
{
    std::string path;
    std::uint64_t mode = 0;
};

//BTRFS_SEND_C_CHOWN: raw ids, no name mapping is done by the protocol.
struct ChownOperation
{
    std::string path;
    std::uint64_t uid = 0;
    std::uint64_t gid = 0;
};

//BTRFS_SEND_C_UTIMES. otime (the btrfs creation time) is only present from stream version 2 on and cannot
//be restored by the receiver.
struct UtimesOperation
{
    std::string path;
    Timestamp atime;
    Timestamp mtime;
    Timestamp ctime;
    std::optional<Timestamp> otime;
};

//BTRFS_SEND_C_END: end of a logical stream, no attributes.
struct EndOperation
{
};

//BTRFS_SEND_C_UPDATE_EXTENT: an extent of a file changed, but the data was not transferred.
//This is what a "btrfs send --no-data" stream reports instead of BTRFS_SEND_C_WRITE.
struct UpdateExtentOperation
{
    std::string path;
    std::uint64_t file_offset = 0;
    std::uint64_t length = 0;
};

//BTRFS_SEND_C_FALLOCATE.
struct FallocateOperation
{
    std::string path;
    std::uint32_t mode = 0;   //flags::FallocateMode
    std::uint64_t file_offset = 0;
    std::uint64_t length = 0;
};

//BTRFS_SEND_C_FILEATTR: the Btrfs inode flags (flags::InodeFlag).
struct FileattrOperation
{
    std::string path;
    std::uint64_t fileattr = 0;
};

//BTRFS_SEND_C_ENCODED_WRITE: data that is already compressed (or encrypted) on the sending side.
//compression and encryption default to NONE when the attributes are absent.
struct EncodedWriteOperation
{
    std::string path;
    std::uint64_t file_offset = 0;
    std::uint64_t unencoded_file_len = 0;
    std::uint64_t unencoded_len = 0;
    std::uint64_t unencoded_offset = 0;
    std::uint32_t compression = 0;   //flags::Compression
    std::uint32_t encryption = 0;    //flags::Encryption
    BinaryData data;
};

//BTRFS_SEND_C_ENABLE_VERITY (stream version 3, experimental upstream).
struct EnableVerityOperation
{
    std::string path;
    std::uint8_t algorithm = 0;
    std::uint32_t block_size = 0;
    BinaryData salt;
    BinaryData signature;
};

//A command this parser does not know. Only produced when Parser::Options::allow_unknown_commands is set,
//otherwise an unknown command id is an ErrorCode::UnknownCommand failure.
struct UnknownCommandOperation
{
    protocol::Command command = protocol::Command::Unspec;
    BinaryData payload;   //raw command payload, before any attribute decoding
};

//Every operation the parser can produce, keyed by the wire command id in SendOperation::command.
using Operation = std::variant<SubvolOperation,
                               SnapshotOperation,
                               MkfileOperation,
                               MkdirOperation,
                               MknodOperation,
                               MkfifoOperation,
                               MksockOperation,
                               SymlinkOperation,
                               RenameOperation,
                               LinkOperation,
                               UnlinkOperation,
                               RmdirOperation,
                               SetXattrOperation,
                               RemoveXattrOperation,
                               WriteOperation,
                               CloneOperation,
                               TruncateOperation,
                               ChmodOperation,
                               ChownOperation,
                               UtimesOperation,
                               EndOperation,
                               UpdateExtentOperation,
                               FallocateOperation,
                               FileattrOperation,
                               EncodedWriteOperation,
                               EnableVerityOperation,
                               UnknownCommandOperation>;

//One decoded command of the stream.
struct SendOperation
{
    protocol::Command command = protocol::Command::Unspec;   //wire command id
    Operation operation{};                                   //typed payload
    std::vector<AttributeValue> attributes;                  //all attributes, in stream order
    std::uint64_t index = 0;                                 //1-based position of the command in the stream
    std::uint64_t stream_offset = 0;                         //offset of the command header in the stream
    std::uint32_t payload_size = 0;                          //declared command payload size

    //Typed access without std::visit boilerplate; nullptr when the command has another type.
    template <typename T>
    const T* as() const noexcept
    {
        return std::get_if<T>(&operation);
    }

    template <typename T>
    T* as() noexcept
    {
        return std::get_if<T>(&operation);
    }

    template <typename T>
    bool is() const noexcept
    {
        return std::holds_alternative<T>(operation);
    }

    //Returns the attribute with the given type, or nullptr. When an attribute is repeated the last
    //occurrence wins, matching the reference receiver.
    const AttributeValue* attribute(protocol::Attribute type) const noexcept;

    //Deep copies the binary payloads so they survive the next call to Parser::next().
    void materialize();
};

//Path the operation applies to, or an empty view for operations without one.
std::string_view operation_path(const SendOperation& operation) noexcept;

//One line, human readable rendering used by the dump tool's text mode and by test diagnostics.
std::string summary(const SendOperation& operation);

}
