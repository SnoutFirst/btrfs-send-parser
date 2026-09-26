# Btrfs send stream format

Reference for the byte layout this parser implements, the attributes each command carries, and the places
where the kernel does something the published description does not mention.

Everything here is verified against real `btrfs send` output: the streams committed under `tests/fixtures`
were produced by the installed kernel and btrfs-progs, and the test suite decodes them field by field. Claims
that come only from the fixtures are marked **observed**.

## Framing

All integers are little endian and unaligned; nothing may be read through a cast.

```
stream  := stream_header command* [stream_header command*]*
header  := "btrfs-stream" NUL        (13 bytes, the NUL is part of the stream)
           u32 version
command := u32 payload_size
           u16 command_id
           u32 crc32c
           payload_size bytes of attributes
attr    := u16 type
           u16 length
           length bytes of value
```

- `kStreamHeaderSize` = 17 bytes, `kCommandHeaderSize` = 10 bytes, `kAttributeHeaderSize` = 4 bytes.
- `payload_size` counts the attributes only, not the 10 header bytes.
- A file may contain several complete streams back to back, header included. `btrfs send` writes that for
  several subvolumes, unless `-e` is given; with `-e` the final `END` of every stream but the last is omitted.
- A zero length payload is normal: `END` has no attributes at all.

### The `DATA` attribute is special from version 2 on

In stream version 1, `DATA` (type 19) is an ordinary TLV with a 16 bit length. From version 2 it carries no
length field and runs to the end of the command payload, so it must be the last attribute of the command:

```
version 1:  WRITE = u16(19) u16(n) data[n]
version 2+: WRITE = u16(19) data[payload_size - offset_of_data]
```

A parser that does not know the stream version before it reads attributes will decode v2 `WRITE` payloads with
the two length bytes counted as data. This parser therefore decodes the TLVs structurally first and interprets
them per command afterwards.

### Checksums

The CRC32C (Castagnoli, polynomial `0x1EDC6F41`) covers the 10 command header bytes with the `crc32c` field
replaced by four zero bytes, followed by the payload. The convention is *not* the one from RFC 3720 / iSCSI:
there is no initial inversion and no final inversion, the seed is 0 and the raw register value is compared.

```
header_zeroed = u32(payload_size) u16(command_id) u32(0)
crc = crc32c_update(0, header_zeroed)
crc = crc32c_update(crc, payload)
```

`crc32c_send_stream()` computes this; `crc32c_standard()` applies the two inversions and is what you want when
talking to anything else.

## Commands

`id`, name, lowest stream version, and the attributes a command carries. `req` means mandatory, `opt` means the
parser accepts the attribute missing.

| id | name | from | attributes |
| --- | --- | --- | --- |
| 1 | `SUBVOL` | 1 | `PATH` req, `UUID` req, `CTRANSID` req |
| 2 | `SNAPSHOT` | 1 | `PATH` req, `UUID` req, `CTRANSID` req, `CLONE_UUID` req, `CLONE_CTRANSID` req |
| 3 | `MKFILE` | 1 | `PATH` req, `INO` req |
| 4 | `MKDIR` | 1 | `PATH` req, `INO` req |
| 5 | `MKNOD` | 1 | `PATH` req, `MODE` req, `RDEV` req, `INO` opt |
| 6 | `MKFIFO` | 1 | `PATH` req, `INO` req, `RDEV` opt, `MODE` opt |
| 7 | `MKSOCK` | 1 | `PATH` req, `INO` req, `RDEV` opt, `MODE` opt |
| 8 | `SYMLINK` | 1 | `PATH` req, `INO` req, `PATH_LINK` req |
| 9 | `RENAME` | 1 | `PATH` req, `PATH_TO` req |
| 10 | `LINK` | 1 | `PATH` req, `PATH_LINK` req |
| 11 | `UNLINK` | 1 | `PATH` req |
| 12 | `RMDIR` | 1 | `PATH` req |
| 13 | `SET_XATTR` | 1 | `PATH` req, `XATTR_NAME` req, `XATTR_DATA` req |
| 14 | `REMOVE_XATTR` | 1 | `PATH` req, `XATTR_NAME` req |
| 15 | `WRITE` | 1 | `PATH` req, `FILE_OFFSET` req, `DATA` req |
| 16 | `CLONE` | 1 | `PATH` req, `FILE_OFFSET` req, `CLONE_LEN` req, `CLONE_UUID` req, `CLONE_CTRANSID` req, `CLONE_PATH` req, `CLONE_OFFSET` req |
| 17 | `TRUNCATE` | 1 | `PATH` req, `SIZE` req |
| 18 | `CHMOD` | 1 | `PATH` req, `MODE` req |
| 19 | `CHOWN` | 1 | `PATH` req, `UID` req, `GID` req |
| 20 | `UTIMES` | 1 | `PATH` req, `ATIME` req, `MTIME` req, `CTIME` req, `OTIME` opt (version 2 and later only) |
| 21 | `END` | 1 | none |
| 22 | `UPDATE_EXTENT` | 1 | `PATH` req, `FILE_OFFSET` req, `SIZE` req |
| 23 | `FALLOCATE` | 2 | `PATH` req, `FALLOCATE_MODE` req, `FILE_OFFSET` req, `SIZE` req |
| 24 | `FILEATTR` | 2 | `PATH` req, `FILEATTR` req |
| 25 | `ENCODED_WRITE` | 2 | `PATH` req, `FILE_OFFSET` req, `UNENCODED_FILE_LEN` req, `UNENCODED_LEN` req, `UNENCODED_OFFSET` req, `DATA` req, `COMPRESSION` opt, `ENCRYPTION` opt |
| 26 | `ENABLE_VERITY` | 3 | `PATH` req, `VERITY_ALGORITHM` req, `VERITY_BLOCK_SIZE` req, `VERITY_SALT_DATA` req, `VERITY_SIG_DATA` req |

`UPDATE_EXTENT` is what a `btrfs send --no-data` stream reports instead of `WRITE`: the extent changed, but the
data was not transferred, so a receiver has to read it from the sending filesystem. Streams carrying
`UPDATE_EXTENT` also carry no `WRITE` or `ENCODED_WRITE` commands.

Stream versions: 1 is the original format, 2 adds `FALLOCATE`, `FILEATTR`, `ENCODED_WRITE` and the length-less
`DATA`, 3 adds `ENABLE_VERITY` (still marked experimental upstream). `btrfs send --proto N` selects one, and the
sending kernel decides what `N` may be: the parser accepts all three, but a 6.12 kernel answers `btrfs send
--proto 3` with `EPROTO` ("send ioctl failed with -71"), so `ENABLE_VERITY` can only be observed on a kernel
that has the verity send support, and the fixtures for it in this repository are built by hand rather than
generated by `btrfs send`.

## Attributes

| id | name | value |
| --- | --- | --- |
| 1 | `UUID` | 16 bytes |
| 2 | `CTRANSID` | u64 |
| 3 | `INO` | u64 |
| 4 | `SIZE` | u64 |
| 5 | `MODE` | u64, the full `st_mode` including the file type bits |
| 6 | `UID` | u64 |
| 7 | `GID` | u64 |
| 8 | `RDEV` | u64, in the encoding `makedev(3)` produces |
| 9 | `CTIME` | 12 bytes: `i64 seconds`, `u32 nanoseconds` |
| 10 | `MTIME` | as `CTIME` |
| 11 | `ATIME` | as `CTIME` |
| 12 | `OTIME` | as `CTIME`, version 2 and later |
| 13 | `XATTR_NAME` | bytes, no terminator |
| 14 | `XATTR_DATA` | bytes, no terminator, may contain NUL |
| 15 | `PATH` | bytes, no terminator, relative to the subvolume root |
| 16 | `PATH_TO` | bytes |
| 17 | `PATH_LINK` | bytes, the symlink target for `SYMLINK`, the existing name for `LINK` |
| 18 | `FILE_OFFSET` | u64 |
| 19 | `DATA` | bytes, length-less from stream version 2 on |
| 20 | `CLONE_UUID` | 16 bytes |
| 21 | `CLONE_CTRANSID` | u64 |
| 22 | `CLONE_PATH` | bytes |
| 23 | `CLONE_OFFSET` | u64 |
| 24 | `CLONE_LEN` | u64 |
| 25 | `FALLOCATE_MODE` | u32, `FALLOC_FL_*` bits |
| 26 | `FILEATTR` | u64, `BTRFS_INODE_*` bits |
| 27 | `UNENCODED_FILE_LEN` | u64 |
| 28 | `UNENCODED_LEN` | u64 |
| 29 | `UNENCODED_OFFSET` | u64 |
| 30 | `COMPRESSION` | u32, 0 none, 1 zlib, 2 zstd, 3..6 LZO with an explicit page size |
| 31 | `ENCRYPTION` | u32, 0 none is the only value defined |
| 32 | `VERITY_ALGORITHM` | u8 |
| 33 | `VERITY_BLOCK_SIZE` | u32 |
| 34 | `VERITY_SALT_DATA` | bytes |
| 35 | `VERITY_SIG_DATA` | bytes |

Attribute type 0 is invalid. The published format allows a receiver to ignore TLVs it does not know, which is
what `Parser::Options::allow_unknown_attribute_types` and `allow_unexpected_attributes` implement.

The kernel emits attributes in a fixed order per command, but the format does not require any order, and this
parser accepts any. When an attribute is repeated the last occurrence wins, matching the reference receiver.

## Behaviour the description does not mention

### `MKNOD` carries an inode number

The documented attribute list for `MKNOD` is `PATH`, `MODE`, `RDEV`, with no `INO`. The kernel has always sent
`INO`; the parser treats it as optional (`MknodOperation::ino` is an `std::optional`) so both shapes work.

### `MKFIFO` and `MKSOCK` carry a mode and a device

The documented list is `PATH` and `INO`. **Observed**: the kernel also sends `MODE` (the full `st_mode`, so the
`S_IFIFO` or `S_IFSOCK` bits are included) and `RDEV` (usually 0). Both are optional in
`MkfifoOperation` / `MksockOperation`.

### Entries are created under a temporary name and renamed

**Observed**: `SYMLINK`, `MKNOD` and similar commands often appear with a path of the form
`o<inode>-<transid>-<n>`, followed later by a `RENAME` to the name the file actually has in the snapshot. This
is how the sender avoids name clashes with entries that are still to be deleted. A consumer that wants final
names has to track `RENAME`, not just read the path of the creation command.

### `ENCODED_WRITE` describes the file extent, not the payload

`UNENCODED_LEN` and `UNENCODED_OFFSET` describe the full uncompressed extent in the file, while
`UNENCODED_FILE_LEN` is the part of it that is actually being written by this command and `DATA` is the
compressed payload. **Observed** in a `--compressed-data` stream: the first command writes 4096 file bytes at
file offset 0 from an extent of 49152, the second writes 40960 file bytes at offset 8192 from the same extent
at unencoded offset 8192. `FILE_OFFSET` and `UNENCODED_OFFSET` agree in both, which is not something the format
guarantees.

### `FILEATTR` values

`FILEATTR` carries the inode flags as stored in the Btrfs inode item, not the `FS_*_FL` values the
`FS_IOC_GETFLAGS` ioctl uses. `chattr +A` shows up as the `noatime` bit. `flags::inode_flag_names()` maps the
bits to the names the documentation uses.

### Timestamps are wall clock, not UTC

Timestamps are stored as seconds since the epoch plus a nanosecond fraction. `Timestamp::to_iso8601_utc()`
converts to a UTC string; the fixtures therefore show times that differ from the local time used when
generating them.

### Paths are arbitrary bytes

A path is a byte string with no encoding requirement: the fixtures contain names with a tab, a newline, a
quote, a backslash, a raw `0xff` byte and multi byte UTF-8. The parser keeps them as `std::string` and never
interprets them. `btrfs-send-dump` escapes them for display.
