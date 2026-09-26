# btrfs-send-parser

[![ci](https://github.com/SnoutFirst/btrfs-send-parser/actions/workflows/ci.yml/badge.svg)](https://github.com/SnoutFirst/btrfs-send-parser/actions/workflows/ci.yml)

A standalone C++17 parser for the Btrfs send stream format. It reads the byte stream that `btrfs send`
produces and hands you typed operations: `WRITE`, `CLONE`, `UPDATE_EXTENT`, xattrs, inode flags, encoded
writes, and the rest.

No dependencies, no Btrfs headers, no kernel, no `libbtrfs`. The only requirement is a C++17 compiler. The
parser is incremental: it buffers one command at a time, so a multi gigabyte stream costs a few kilobytes of
memory.

It does **not** receive, restore or modify a filesystem. It is the reading half, useful for backup tools,
stream inspectors, differential tooling and tests.

```console
$ btrfs send -p /snap-old /snap-new | btrfs-send-dump -s
1 @17 snapshot path="snap-new" uuid=9412e3be-6877-c149-b0fb-a93ac80c3303 ctransid=12 clone_uuid=0852fed2-f4b1-9f44-a65e-e67908fecaef clone_ctransid=9
2 @100 rename path="empty" path_to="o258-9-0"
3 @131 unlink path="hard.txt"
4 @153 remove_xattr path="f1.txt" xattr_name="user.text"
5 @186 write path="f1.txt" file_offset=0 data=<27 bytes>
...
58 @39650 end
stream version: 1
commands:       58
attributes:     162
bytes read:     39660
file data:      36991 bytes
command counts:
  snapshot 1
  mkfile 3
  ...
  write 4
  clone 1
  truncate 1
  ...
```

## Contents

- [Building](#building)
- [The command line tool](#the-command-line-tool)
- [Using the library](#using-the-library)
- [Behaviour worth knowing](#behaviour-worth-knowing)
- [Tests](#tests)
- [Layout](#layout)
- [Wire format](docs/format.md)

## Building

```console
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
$ cmake --build build
$ ctest --test-dir build --output-on-failure
$ cmake --install build --prefix /usr/local
```

CMake options, all `ON` unless noted:

| Option | Meaning |
| --- | --- |
| `BTRFS_SEND_PARSER_BUILD_CLI` | build the `btrfs-send-dump` tool |
| `BTRFS_SEND_PARSER_BUILD_TESTS` | build the unit tests and register the fixture tests |
| `BTRFS_SEND_PARSER_BUILD_EXAMPLES` | build the three example programs |
| `BTRFS_SEND_PARSER_BUILD_INTEGRATION_TESTS` | register the tests that need root and a Btrfs filesystem (`OFF`) |
| `BTRFS_SEND_PARSER_INSTALL` | generate install rules |
| `BTRFS_SEND_PARSER_WARNINGS_AS_ERRORS` | add `-Werror` to the library and tool (`OFF`) |

Installed as a CMake package:

```cmake
find_package(btrfs-send-parser CONFIG REQUIRED)
target_link_libraries(your-tool PRIVATE btrfs::send)
```

## The command line tool

```
btrfs-send-dump [options] [file...]
```

With no file, or with `-`, the stream is read from standard input, so it composes with `btrfs send` without a
temporary file.

Output formats:

- `-f text` (default): one line per operation, `<index> @<offset> <summary>`.
- `-f json`: one JSON document per input, with every attribute of every command.
- `-f jsonl`: one JSON object per line, for streams too large to hold one document in memory.

Useful options:

| Option | Effect |
| --- | --- |
| `-a`, `--attributes` | text output: print every attribute of every command |
| `-s`, `--stats` | summary block: version, command/attribute/byte counts, per command histogram |
| `-c`, `--changed-extents` | list `UPDATE_EXTENT` entries instead of the operations (the `--no-data` case) |
| `--no-checksums` | skip CRC32C verification |
| `--headerless` / `--assumed-version N` | input has no stream header |
| `--single-stream` | refuse a second stream header inside the input |
| `--stop-after-end` | stop at the first `END` |
| `--allow-unknown-commands`, `--allow-unknown-attribute-types`, `--allow-unexpected-attributes` | read forward, from a newer kernel than this build knows |
| `--max-command-size N` | reject a command declaring more than N payload bytes |

Exit status: `0` all inputs parsed, `1` a stream was malformed or unreadable, `2` bad arguments.

Reading a `--no-data` stream:

```console
$ btrfs send --no-data -p /snap-old /snap-new | btrfs-send-dump -c
changed "f1.txt" offset=0 length=27
changed "sparse.bin" offset=67108864 length=4096
```

Binary values are hex in both formats. Strings in JSON are byte strings: every byte outside printable ASCII is
escaped as `\u00xx`, because a send stream path is arbitrary bytes and may not be valid UTF-8. To recover the
exact bytes, decode the JSON and encode the string as latin-1. In text output, blobs are shown truncated to 32
bytes; in JSON they are complete.

## Using the library

### Streaming

```cpp
#include "btrfs/send.hpp"

btrfs::send::FdSource source(STDIN_FILENO);
btrfs::send::Parser parser(source);

while (std::optional<btrfs::send::SendOperation> operation = parser.next())
{
    std::cout << btrfs::send::summary(*operation) << "\n";

    if (const btrfs::send::WriteOperation* write = operation->as<btrfs::send::WriteOperation>())
        write->data.size();   //valid until the next call to next()
}
```

`Parser::next()` returns `std::nullopt` at the end of the stream and throws `ParseError` for malformed input.
`SendOperation` carries the wire command id, the typed payload, every attribute in stream order, and where the
command sits in the stream (`index`, `stream_offset`, `payload_size`). `operation.as<T>()` returns `nullptr`
when the command has another type.

Binary payloads are views into the parser's command buffer, valid until the next call. Call
`operation.materialize()` to detach them when the operation has to outlive that.

The non-throwing flavour reports the same failure by value and is safe in code built without exceptions:

```cpp
btrfs::send::SendOperation operation;
btrfs::send::ParseFailure failure;

for (;;)
{
    const btrfs::send::NextStatus status = parser.next(operation, failure);
    if (status == btrfs::send::NextStatus::EndOfStream)
        break;
    if (status == btrfs::send::NextStatus::Error)
    {
        std::cerr << btrfs::send::describe(failure) << "\n";
        break;
    }
}
```

### A whole stream in memory

```cpp
btrfs::send::SendStream stream = btrfs::send::parse_file("stream.bin");

stream.version;   //last stream version in the input
stream.count(btrfs::send::protocol::Command::Write);

if (const btrfs::send::SendOperation* operation = stream.first(btrfs::send::protocol::Command::Subvol))
{
    const btrfs::send::SubvolOperation* subvol = operation->as<btrfs::send::SubvolOperation>();
    subvol->path, subvol->uuid, subvol->ctrans_id;
}

std::vector<const btrfs::send::CloneOperation*> clones = stream.of_type<btrfs::send::CloneOperation>();

for (const btrfs::send::ChangedExtent& extent : btrfs::send::changed_extents(stream))
    extent.path, extent.offset, extent.length;
```

`parse()` materializes every payload, so the result is owned and stays valid. Also available:
`parse(ByteSource&)`, `parse(std::istream&)`, `parse_fd(int)`.

### Byte sources

| Type | Use |
| --- | --- |
| `MemorySource` | borrows a buffer or a `std::string_view` |
| `IstreamSource` | wraps a `std::istream` |
| `FdSource` | reads from a file descriptor, retries on `EINTR` |
| `FileSource` | wraps a `std::FILE*` |
| `ChunkedSource` | wraps another source and caps every read, for tests and for pipe-like consumers |

A source may return fewer bytes than requested at any time, and returning 0 means end of input. A source
reports failures by throwing; the parser turns any `std::exception` from `read()` into
`ErrorCode::SourceReadFailed`, so a throwing source never takes it by surprise.

### Parser options

| Option | Default | Effect |
| --- | --- | --- |
| `expect_stream_header` | `true` | require the `btrfs-stream` magic and version |
| `assumed_stream_version` | 1 | version to use for headerless input |
| `verify_checksums` | `true` | verify the CRC32C of every command |
| `stop_after_end_command` | `false` | stop at the first `END` |
| `require_subvol_or_snapshot_first` | `true` | the first command of a stream must be `SUBVOL` or `SNAPSHOT` |
| `allow_concatenated_streams` | `true` | accept a second stream header inside the input |
| `allow_unknown_commands` | `false` | keep undefined command ids as `UnknownCommandOperation` |
| `allow_unknown_attribute_types` | `false` | skip attribute types above the known range |
| `allow_unexpected_attributes` | `false` | keep attributes a command does not define |
| `max_command_size` | 64 MiB | reject a command declaring more than this |

### Error codes

`ErrorCode` is a closed set, so a caller can react to a class of failure instead of matching message text:
`InvalidStreamHeader`, `InvalidMagic`, `UnsupportedVersion`, `UnexpectedEndOfStream`, `SizeOverflow`,
`ChecksumMismatch`, `MalformedAttribute`, `UnknownAttributeType`, `InvalidAttributeLength`,
`MissingAttribute`, `UnexpectedAttribute`, `InvalidAttributeValue`, `UnknownCommand`,
`InvalidCommandStructure`, `SourceReadFailed`.

`ParseFailure` carries the code, a message, the byte offset and the 1-based command index.
`describe(failure)` renders `checksum_mismatch in command 3 at offset 412: ...`.

## Behaviour worth knowing

- **Checksums are not the RFC 3720 variant.** The stream computes CRC32C with no initial and no final
  inversion, so the raw register value is compared. `crc32c_send_stream()` does it that way,
  `crc32c_standard()` is the variant you want when talking to something else.
- **`DATA` has no length field from stream version 2 on.** It runs to the end of the command payload, which is
  why the parser decodes attributes before it interprets them and why a v1-shaped `WRITE` in a v2 stream would
  be misread. The test suite covers both shapes.
- **A file may hold several streams.** `btrfs send` with several subvolumes and without `-e` concatenates
  complete streams, headers included. `SendStream::version` reports the last one; use
  `allow_concatenated_streams = false` to refuse the second header instead.
- **`btrfs send -e` omits the final `END`.** The next stream header is what ends the stream, which is why the
  parser accepts a header where a command would be.
- **Paths are arbitrary bytes.** `std::string` everywhere, never `const char*`; non UTF-8 names, embedded
  control characters and newlines survive the round trip.
- **The attribute list holds every attribute the parser accepted, in stream order.** Whether an attribute is
  accepted at all depends on the options: an attribute a command does not define is an `UnexpectedAttribute`
  failure unless `allow_unexpected_attributes` is set, and an attribute type above the known range is a
  `UnknownAttributeType` failure unless `allow_unknown_attribute_types` is set, in which case it is listed with
  its raw bytes.
- **Nothing is buffered beyond one command**, and a corrupt length field cannot force a large allocation:
  `max_command_size` is checked before the payload is read.
- **Protocol 3 is implemented but rarely seen.** `ENABLE_VERITY` and its attributes are decoded like any other
  command, and the unit tests drive them from hand-built streams. No kernel here emits them: this machine runs
  6.12, whose `btrfs send --proto 3` fails with `EPROTO` even though fs-verity itself works on Btrfs and
  btrfs-progs 6.14 is compiled for stream version 3. The integration test enables verity in a subvolume of its
  own and checks the version 3 path when the kernel allows it, and says so when it does not.

## Tests

```console
$ ctest --test-dir build --output-on-failure
```

- `unit` — one binary with 100+ cases: CRC32C vectors, UUID and timestamp handling, every command type, error
  paths, short reads, sources, the whole-stream API, and the fixtures below.
- `cli-<fixture>` — the dump tool over every committed fixture, plus a check that input which is not a stream
  is refused.
- `integration` (off by default) — `tests/integration/run.sh` needs root and a mountable Btrfs image: it
  creates a filesystem, sends real streams through a pipe into the tool, and checks the good and the bad cases.
  Enable it with `-DBTRFS_SEND_PARSER_BUILD_INTEGRATION_TESTS=ON`.

CI runs all of that on every push: gcc and clang with warnings as errors, an ASan+UBSan build, an
`install` plus `find_package` consumer, and the integration test as root on a runner with loop devices.

The fixtures under `tests/fixtures` are real streams from `btrfs send`, not hand written. They cover protocol
1 and 2, full and incremental sends, `--no-data`, `--compressed-data`, clone, fallocate, fileattr,
concatenated streams, `-e`, and paths that need escaping (tab, newline, quotes, a non UTF-8 byte, multi byte
UTF-8). Regenerate them on a machine with root:

```console
$ sudo tests/integration/gen-fixtures.sh tests/fixtures
```

The fixture tests cross check the parser against a second, independent walk over the raw framing in the test
file, so the parser cannot grade its own homework: command ids, payload lengths and offsets have to agree
byte for byte, and the recorded counts are verified that way.

## Layout

```
include/btrfs/send.hpp          umbrella header
include/btrfs/send/*.hpp        protocol, operations, parser, stream, sources, error, flags, types
src/                            implementation
cli/                            btrfs-send-dump
examples/                       walk_stream, changed_extents, parse_to_memory
tests/                          unit tests, stream builder, fixtures, generator, integration run
docs/format.md                  wire format reference
```

## License

MIT, see [LICENSE](LICENSE).
