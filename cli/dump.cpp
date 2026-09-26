#include "dump.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "btrfs/send.hpp"

#ifndef BTRFS_SEND_PARSER_VERSION
#define BTRFS_SEND_PARSER_VERSION "0.0.0"
#endif

namespace dump {

namespace {

using btrfs::send::AttributeValue;
using btrfs::send::BinaryData;
using btrfs::send::ByteSource;
using btrfs::send::ChangedExtent;
using btrfs::send::EncodedWriteOperation;
using btrfs::send::FileSource;
using btrfs::send::IstreamSource;
using btrfs::send::NextStatus;
using btrfs::send::ParseFailure;
using btrfs::send::Parser;
using btrfs::send::SendOperation;
using btrfs::send::Timestamp;
using btrfs::send::UpdateExtentOperation;
using btrfs::send::WriteOperation;
using btrfs::send::protocol::Attribute;
using btrfs::send::protocol::Command;

constexpr const char* kToolName = "btrfs-send-dump";

//How many bytes of an attribute value the text output shows before it gives up.
constexpr std::size_t kTextBlobLimit = 32;

//Attribute values are classified by how they are best rendered. The classification follows the protocol
//definition of each attribute, so an attribute that shows up in a command it does not belong to (possible
//with allow_unexpected_attributes) is still rendered the way the format defines it.
enum class ValueKind
{
    Unsigned,   //a little endian integer
    Timestamp,  //64 bit seconds plus 32 bit nanoseconds
    Uuid,       //16 raw bytes in textual order
    Text,       //a path or a name, arbitrary bytes but usually readable
    Blob,       //opaque bytes, shown as hex
};

ValueKind value_kind(Attribute type) noexcept
{
    switch (type)
    {
    case Attribute::Uuid:
    case Attribute::CloneUuid:
        return ValueKind::Uuid;
    case Attribute::CtransId:
    case Attribute::Ino:
    case Attribute::Size:
    case Attribute::Mode:
    case Attribute::Uid:
    case Attribute::Gid:
    case Attribute::Rdev:
    case Attribute::FileOffset:
    case Attribute::CloneCtransId:
    case Attribute::CloneOffset:
    case Attribute::CloneLen:
    case Attribute::FallocateMode:
    case Attribute::Fileattr:
    case Attribute::UnencodedFileLen:
    case Attribute::UnencodedLen:
    case Attribute::UnencodedOffset:
    case Attribute::Compression:
    case Attribute::Encryption:
    case Attribute::VerityAlgorithm:
    case Attribute::VerityBlockSize:
        return ValueKind::Unsigned;
    case Attribute::Ctime:
    case Attribute::Mtime:
    case Attribute::Atime:
    case Attribute::Otime:
        return ValueKind::Timestamp;
    case Attribute::Path:
    case Attribute::PathTo:
    case Attribute::PathLink:
    case Attribute::XattrName:
    case Attribute::ClonePath:
        return ValueKind::Text;
    case Attribute::XattrData:
    case Attribute::Data:
    case Attribute::VeritySaltData:
    case Attribute::VeritySigData:
        return ValueKind::Blob;
    case Attribute::Unspec:
        break;
    }
    return ValueKind::Blob;
}

std::string hex_encode(std::string_view bytes)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const char character : bytes)
    {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(kHexDigits[(byte >> 4) & 0xfu]);
        result.push_back(kHexDigits[byte & 0xfu]);
    }
    return result;
}

//Text output escapes the same way the library's summary() does, so both stay readable and printable.
void append_text_escaped(std::string& out, std::string_view text)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (const char character : text)
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
}

//JSON has to be valid UTF-8, and a send stream path is arbitrary bytes, so everything outside printable
//ASCII is written as \u00xx. A consumer that wants the exact bytes can decode the escape again; strings
//that are valid UTF-8 come out readable but with the multi byte sequences expanded.
void append_json_escaped(std::string& out, std::string_view text)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (const char character : text)
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
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (byte < 0x20 || byte >= 0x7f)
            {
                out += "\\u00";
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
}

std::string json_string(std::string_view text)
{
    std::string result = "\"";
    append_json_escaped(result, text);
    result += '"';
    return result;
}

std::optional<std::uint64_t> read_unsigned(const BinaryData& data) noexcept
{
    switch (data.size())
    {
    case 1:
        return static_cast<std::uint64_t>(static_cast<unsigned char>(*data.data()));
    case 4:
        return static_cast<std::uint64_t>(btrfs::send::protocol::read_le32(data.data()));
    case 8:
        return btrfs::send::protocol::read_le64(data.data());
    default:
        return std::nullopt;
    }
}

std::optional<Timestamp> read_timestamp(const BinaryData& data) noexcept
{
    if (data.size() != 12)
        return std::nullopt;
    Timestamp stamp;
    stamp.seconds = static_cast<std::int64_t>(btrfs::send::protocol::read_le64(data.data()));
    stamp.nanoseconds = btrfs::send::protocol::read_le32(data.data() + 8);
    return stamp;
}

std::optional<btrfs::send::Uuid> read_uuid(const BinaryData& data) noexcept
{
    if (data.size() != btrfs::send::Uuid::kSize)
        return std::nullopt;
    btrfs::send::Uuid uuid;
    for (std::size_t index = 0; index < uuid.bytes.size(); ++index)
        uuid.bytes[index] = static_cast<std::uint8_t>(data.data()[index]);
    return uuid;
}

std::string text_attribute_value(const AttributeValue& attribute)
{
    switch (value_kind(attribute.type))
    {
    case ValueKind::Unsigned:
        if (const std::optional<std::uint64_t> value = read_unsigned(attribute.value))
            return std::to_string(*value);
        break;
    case ValueKind::Timestamp:
        if (const std::optional<Timestamp> stamp = read_timestamp(attribute.value))
            return stamp->to_iso8601_utc();
        break;
    case ValueKind::Uuid:
        if (const std::optional<btrfs::send::Uuid> uuid = read_uuid(attribute.value))
            return uuid->to_string();
        break;
    case ValueKind::Text:
    {
        std::string result = "\"";
        append_text_escaped(result, attribute.value.as_string_view());
        result += '"';
        return result;
    }
    case ValueKind::Blob:
        break;
    }

    //Anything whose size does not match its type is reported as the raw bytes it actually is.
    std::string_view bytes = attribute.value.as_string_view();
    const bool truncated = bytes.size() > kTextBlobLimit;
    if (truncated)
        bytes = bytes.substr(0, kTextBlobLimit);
    return "<" + std::to_string(attribute.value.size()) + " bytes " + hex_encode(bytes) + (truncated ? "..." : "") + ">";
}

std::string json_attribute_value(const AttributeValue& attribute)
{
    switch (value_kind(attribute.type))
    {
    case ValueKind::Unsigned:
        if (const std::optional<std::uint64_t> value = read_unsigned(attribute.value))
            return std::to_string(*value);
        break;
    case ValueKind::Timestamp:
        if (const std::optional<Timestamp> stamp = read_timestamp(attribute.value))
            return "{\"seconds\": " + std::to_string(stamp->seconds) + ", \"nanoseconds\": " + std::to_string(stamp->nanoseconds) + "}";
        break;
    case ValueKind::Uuid:
        if (const std::optional<btrfs::send::Uuid> uuid = read_uuid(attribute.value))
            return json_string(uuid->to_string());
        break;
    case ValueKind::Text:
        return json_string(attribute.value.as_string_view());
    case ValueKind::Blob:
        break;
    }

    return "{\"size\": " + std::to_string(attribute.value.size()) + ", \"hex\": " + json_string(hex_encode(attribute.value.as_string_view())) + "}";
}

std::string operation_json(const SendOperation& operation)
{
    std::ostringstream line;
    line << "{\"index\": " << operation.index
         << ", \"offset\": " << operation.stream_offset
         << ", \"command\": " << json_string(btrfs::send::protocol::to_string(operation.command))
         << ", \"command_id\": " << static_cast<std::uint16_t>(operation.command)
         << ", \"payload_size\": " << operation.payload_size
         << ", \"summary\": " << json_string(btrfs::send::summary(operation));

    const std::string_view path = btrfs::send::operation_path(operation);
    if (!path.empty())
        line << ", \"path\": " << json_string(path);

    line << ", \"attributes\": [";
    bool first = true;
    for (const AttributeValue& attribute : operation.attributes)
    {
        if (!first)
            line << ", ";
        first = false;
        line << "{\"type\": " << json_string(btrfs::send::protocol::to_string(attribute.type))
             << ", \"type_id\": " << static_cast<std::uint16_t>(attribute.type)
             << ", \"size\": " << attribute.value.size()
             << ", \"value\": " << json_attribute_value(attribute) << "}";
    }
    line << "]}";
    return line.str();
}

std::string changed_extent_json(const ChangedExtent& extent)
{
    return "{\"path\": " + json_string(extent.path) + ", \"offset\": " + std::to_string(extent.offset) + ", \"length\": " + std::to_string(extent.length) + "}";
}

struct StreamStats
{
    std::uint32_t version = 0;
    std::uint64_t commands = 0;
    std::uint64_t attributes = 0;
    std::uint64_t bytes_consumed = 0;
    std::uint64_t data_bytes = 0;
    std::map<Command, std::uint64_t> per_command;
    std::vector<ChangedExtent> changed_extents;
};

void accumulate(StreamStats& stats, const SendOperation& operation)
{
    stats.attributes += operation.attributes.size();
    stats.per_command[operation.command] += 1;
    if (const WriteOperation* write = operation.as<WriteOperation>())
        stats.data_bytes += write->data.size();
    else if (const EncodedWriteOperation* encoded = operation.as<EncodedWriteOperation>())
        stats.data_bytes += encoded->data.size();
    if (const UpdateExtentOperation* update = operation.as<UpdateExtentOperation>())
        stats.changed_extents.push_back(ChangedExtent{update->path, update->file_offset, update->length});
}

void print_text_stats(std::ostream& out, const StreamStats& stats)
{
    out << "stream version: " << stats.version << "\n"
        << "commands:       " << stats.commands << "\n"
        << "attributes:     " << stats.attributes << "\n"
        << "bytes read:     " << stats.bytes_consumed << "\n"
        << "file data:      " << stats.data_bytes << " bytes\n"
        << "command counts:\n";
    for (const auto& entry : stats.per_command)
        out << "  " << btrfs::send::protocol::to_string(entry.first) << " " << entry.second << "\n";
}

std::string json_stats(const StreamStats& stats)
{
    std::ostringstream block;
    block << "{\"version\": " << stats.version
          << ", \"commands\": " << stats.commands
          << ", \"attributes\": " << stats.attributes
          << ", \"bytes_read\": " << stats.bytes_consumed
          << ", \"data_bytes\": " << stats.data_bytes
          << ", \"per_command\": {";
    bool first = true;
    for (const auto& entry : stats.per_command)
    {
        if (!first)
            block << ", ";
        first = false;
        block << json_string(btrfs::send::protocol::to_string(entry.first)) << ": " << entry.second;
    }
    block << "}}";
    return block.str();
}

struct Input
{
    std::ifstream file;
    std::unique_ptr<ByteSource> source;
};

bool open_input(const std::string& path, Input& input, std::ostream& err)
{
    if (path.empty() || path == "-")
    {
        input.source = std::make_unique<FileSource>(stdin);
        return true;
    }
    input.file.open(path, std::ios::binary);
    if (!input.file)
    {
        err << kToolName << ": cannot open " << path << "\n";
        return false;
    }
    input.source = std::make_unique<IstreamSource>(input.file);
    return true;
}

std::string describe_input(const std::string& path)
{
    return path.empty() ? std::string("-") : path;
}

//Parses a single input and writes it in the requested format. Returns false when the stream is malformed.
bool parse_input(const Options& options, const std::string& label, ByteSource& source, std::ostream& out, std::ostream& err)
{
    const bool emit_operations = !options.print_changed_extents;
    const bool json_document = options.format == Format::Json;
    const bool json_lines = options.format == Format::JsonLines;
    const bool text = options.format == Format::Text;

    StreamStats stats;
    ParseFailure failure;
    bool failed = false;

    //The document is written incrementally: fields are separated by an explicit comma, so any combination of
    //requested sections produces valid JSON.
    if (json_document)
    {
        out << "{\n  \"input\": " << json_string(label);
        if (emit_operations)
            out << ",\n  \"operations\": [";
    }

    Parser parser(source, options.parser);
    bool first_operation = true;
    for (;;)
    {
        SendOperation operation;
        const NextStatus status = parser.next(operation, failure);
        if (status == NextStatus::EndOfStream)
            break;
        if (status == NextStatus::Error)
        {
            failed = true;
            break;
        }

        accumulate(stats, operation);
        if (!emit_operations)
            continue;

        if (text)
        {
            out << operation.index << " @" << operation.stream_offset << " " << btrfs::send::summary(operation) << "\n";
            if (options.print_attributes)
            {
                for (const AttributeValue& attribute : operation.attributes)
                    out << "    " << btrfs::send::protocol::to_string(attribute.type) << "=" << text_attribute_value(attribute) << "\n";
            }
        }
        else if (json_document)
        {
            out << (first_operation ? "\n    " : ",\n    ") << operation_json(operation);
        }
        else
        {
            out << "{\"input\": " << json_string(label) << ", \"operation\": " << operation_json(operation) << "}\n";
        }
        first_operation = false;
    }

    stats.version = parser.stream_version();
    stats.commands = parser.commands_parsed();
    stats.bytes_consumed = parser.bytes_consumed();

    if (failed)
        err << label << ": " << btrfs::send::describe(failure) << "\n";

    if (text || json_lines)
    {
        if (options.print_changed_extents)
        {
            for (const ChangedExtent& extent : stats.changed_extents)
            {
                if (text)
                    out << "changed \"" << extent.path << "\" offset=" << extent.offset << " length=" << extent.length << "\n";
                else
                    out << "{\"input\": " << json_string(label) << ", \"changed_extent\": " << changed_extent_json(extent) << "}\n";
            }
        }
        if (options.print_stats)
        {
            if (text)
                print_text_stats(out, stats);
            else
                out << "{\"input\": " << json_string(label) << ", \"stats\": " << json_stats(stats) << "}\n";
        }
        return !failed;
    }

    if (emit_operations)
        out << "\n  ]";
    if (options.print_changed_extents)
    {
        out << ",\n  \"changed_extents\": [";
        bool first = true;
        for (const ChangedExtent& extent : stats.changed_extents)
        {
            if (!first)
                out << ", ";
            first = false;
            out << changed_extent_json(extent);
        }
        out << "]";
    }
    if (failed)
    {
        out << ",\n  \"error\": {\"code\": " << json_string(btrfs::send::to_string(failure.code))
            << ", \"message\": " << json_string(failure.message)
            << ", \"offset\": " << failure.stream_offset
            << ", \"command\": " << failure.command_index << "}";
    }
    out << ",\n  \"stats\": " << json_stats(stats) << "\n}\n";
    return !failed;
}

}

bool parse_arguments(int argc, char** argv, Options& options, std::vector<std::string>& inputs, std::string& error)
{
    bool only_inputs = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (only_inputs || argument.empty() || argument[0] != '-' || argument == "-")
        {
            inputs.push_back(argument);
            continue;
        }
        if (argument == "--")
        {
            only_inputs = true;
        }
        else if (argument == "-h" || argument == "--help")
        {
            options.show_help = true;
        }
        else if (argument == "-V" || argument == "--version")
        {
            options.show_version = true;
        }
        else if (argument == "-a" || argument == "--attributes")
        {
            options.print_attributes = true;
        }
        else if (argument == "-s" || argument == "--stats")
        {
            options.print_stats = true;
        }
        else if (argument == "-c" || argument == "--changed-extents")
        {
            options.print_changed_extents = true;
        }
        else if (argument == "--no-checksums")
        {
            options.parser.verify_checksums = false;
        }
        else if (argument == "--stop-after-end")
        {
            options.parser.stop_after_end_command = true;
        }
        else if (argument == "--single-stream")
        {
            options.parser.allow_concatenated_streams = false;
        }
        else if (argument == "--first-command-rule-off")
        {
            options.parser.require_subvol_or_snapshot_first = false;
        }
        else if (argument == "--allow-unknown-commands")
        {
            options.parser.allow_unknown_commands = true;
        }
        else if (argument == "--allow-unknown-attribute-types")
        {
            options.parser.allow_unknown_attribute_types = true;
        }
        else if (argument == "--allow-unexpected-attributes")
        {
            options.parser.allow_unexpected_attributes = true;
        }
        else if (argument == "-f" || argument == "--format")
        {
            if (index + 1 >= argc)
            {
                error = "missing value for " + argument;
                return false;
            }
            const std::string value = argv[++index];
            if (value == "text")
                options.format = Format::Text;
            else if (value == "json")
                options.format = Format::Json;
            else if (value == "jsonl" || value == "json-lines")
                options.format = Format::JsonLines;
            else
            {
                error = "unknown format \"" + value + "\", expected text, json or jsonl";
                return false;
            }
        }
        else if (argument == "--headerless")
        {
            //A headerless stream needs the version that the omitted header would have carried.
            options.parser.expect_stream_header = false;
            options.parser.require_subvol_or_snapshot_first = false;
        }
        else if (argument == "--assumed-version")
        {
            if (index + 1 >= argc)
            {
                error = "missing value for " + argument;
                return false;
            }
            std::istringstream value(argv[++index]);
            std::uint32_t version = 0;
            value >> version;
            if (!value || version < btrfs::send::protocol::kMinStreamVersion || version > btrfs::send::protocol::kMaxStreamVersion)
            {
                error = "stream version must be between 1 and " + std::to_string(btrfs::send::protocol::kMaxStreamVersion);
                return false;
            }
            options.parser.expect_stream_header = false;
            options.parser.assumed_stream_version = version;
            options.parser.require_subvol_or_snapshot_first = false;
        }
        else if (argument == "--max-command-size")
        {
            if (index + 1 >= argc)
            {
                error = "missing value for " + argument;
                return false;
            }
            std::istringstream value(argv[++index]);
            std::uint64_t size = 0;
            value >> size;
            if (!value || size == 0 || size > 0xffffffffull)
            {
                error = "max command size must be a positive number of bytes up to 4294967295";
                return false;
            }
            options.parser.max_command_size = static_cast<std::uint32_t>(size);
        }
        else
        {
            error = "unknown option \"" + argument + "\"";
            return false;
        }
    }
    return true;
}

void print_usage(std::ostream& out, const char* program)
{
    out << "usage: " << program << " [options] [file...]\n"
        << "\n"
        << "Parses Btrfs send streams and prints their contents. With no file, or with \"-\", the stream is\n"
        << "read from standard input, so \"btrfs send /snap | " << program << "\" works without a temporary file.\n"
        << "\n"
        << "output:\n"
        << "  -f, --format text|json|jsonl   text (default) is one line per operation, json is one document\n"
        << "                                 per input and jsonl is one object per line\n"
        << "  -a, --attributes               text format: print every attribute of every command\n"
        << "  -s, --stats                    summary block: version, command, attribute and byte counts\n"
        << "  -c, --changed-extents          list UPDATE_EXTENT entries instead of the operations\n"
        << "  -h, --help                     print this text\n"
        << "  -V, --version                  print the version of this tool\n"
        << "\n"
        << "parsing:\n"
        << "      --no-checksums             skip CRC32C verification\n"
        << "      --headerless               input has no stream header, assume version 1\n"
        << "      --assumed-version N        headerless input, assume stream version N\n"
        << "      --stop-after-end           stop at the first END command\n"
        << "      --single-stream            refuse a second stream header in the input\n"
        << "      --first-command-rule-off   do not require SUBVOL or SNAPSHOT first\n"
        << "      --allow-unknown-commands   keep undefined command ids as raw commands\n"
        << "      --allow-unknown-attribute-types\n"
        << "                                 skip attribute types this parser does not know\n"
        << "      --allow-unexpected-attributes\n"
        << "                                 keep attributes that the command does not define\n"
        << "      --max-command-size N       reject commands declaring more than N payload bytes\n"
        << "\n"
        << "exit status: 0 all inputs parsed, 1 a stream was malformed or unreadable, 2 bad arguments\n"
        << "\n"
        << "Binary values are hex in both formats; json also carries the size. Strings in json escape every\n"
        << "byte outside printable ASCII as \\u00xx, because a send stream path may not be valid UTF-8.\n";
}

int run(const Options& options, const std::vector<std::string>& inputs, std::ostream& out, std::ostream& err)
{
    if (options.show_version)
    {
        out << kToolName << " " << BTRFS_SEND_PARSER_VERSION << "\n";
        return 0;
    }

    const std::vector<std::string> actual_inputs = inputs.empty() ? std::vector<std::string>{std::string()} : inputs;
    const bool print_headers = options.format == Format::Text && actual_inputs.size() > 1;

    bool all_ok = true;
    for (const std::string& path : actual_inputs)
    {
        Input input;
        if (!open_input(path, input, err))
        {
            all_ok = false;
            continue;
        }
        const std::string label = describe_input(path);
        if (print_headers)
            out << "-- " << label << "\n";
        if (!parse_input(options, label, *input.source, out, err))
            all_ok = false;
    }
    return all_ok ? 0 : 1;
}

}
