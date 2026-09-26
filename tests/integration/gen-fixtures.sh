#!/bin/sh
#Generates the real send streams kept in tests/fixtures.
#
#Needs root (loop device, mount, mkfs.btrfs) and btrfs-progs. The streams it produces come from the
#installed kernel and btrfs-progs, so the fixture tests exercise the parser against bytes this project
#did not write. Run it from the repository root:
#
#    sudo tests/integration/gen-fixtures.sh tests/fixtures
#
#Existing .bin files in the output directory are replaced.

set -eu

OUT=${1:-tests/fixtures}
IMAGE_SIZE_MB=${IMAGE_SIZE_MB:-768}
MOUNT_POINT=${MOUNT_POINT:-/mnt/btrfs-send-parser-fixtures}
IMAGE=${IMAGE:-/tmp/btrfs-send-parser-fixtures.img}

if [ "$(id -u)" != "0" ]; then
    echo "gen-fixtures.sh must run as root" >&2
    exit 1
fi

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

cleanup()
{
    umount "$MOUNT_POINT" 2>/dev/null || true
    rmdir "$MOUNT_POINT" 2>/dev/null || true
}
trap cleanup EXIT

cleanup
mkdir -p "$MOUNT_POINT"
dd if=/dev/zero of="$IMAGE" bs=1M count="$IMAGE_SIZE_MB" status=none
mkfs.btrfs -q -f "$IMAGE"
echo "mount $IMAGE on $MOUNT_POINT"
mount -o loop,compress=zstd "$IMAGE" "$MOUNT_POINT"

fs="$MOUNT_POINT"

#--- source content for the "many operations" stream -----------------------------------------------------------------

btrfs subvolume create "$fs/src" > /dev/null
mkdir -p "$fs/src/d1" "$fs/src/empty"
printf 'hello world\n' > "$fs/src/f1.txt"
head -c 16384 /dev/urandom > "$fs/src/big.bin"
ln -s f1.txt "$fs/src/link.txt"
ln "$fs/src/f1.txt" "$fs/src/hard.txt"
mkfifo "$fs/src/fifo"
printf 'to be renamed\n' > "$fs/src/d1/x"
printf 'removed again\n' > "$fs/src/empty/removed.txt"
setfattr -n user.text -v 'plain value' "$fs/src/f1.txt"
setfattr -n user.binary -v 0x00010203ff7f "$fs/src/f1.txt"
chmod 640 "$fs/src/f1.txt"
chown 2:3 "$fs/src/f1.txt"
touch -d '2020-01-02 03:04:05' "$fs/src/f1.txt"
#A sparse file whose only data sits far into the file, to exercise large offsets and lengths.
head -c 4096 /dev/urandom | dd of="$fs/src/sparse.bin" bs=4096 seek=16384 status=none
#Paths with characters that have to survive the round trip: spaces, tab, newline, quote, backslash,
#a non UTF-8 byte and multi byte UTF-8.
python3 "$(dirname "$0")/make-weird-tree.py" "$fs/src"

#--- content for the compressed/clone streams ------------------------------------------------------------------------

btrfs subvolume create "$fs/csrc" > /dev/null
mkdir -p "$fs/csrc/comp"
chattr +c "$fs/csrc/comp" 2>/dev/null || true
python3 - "$fs/csrc/comp/repeat.txt" <<'PYEOF'
import sys
with open(sys.argv[1], 'wb') as handle:
    handle.write(b'abcd' * 8192)
PYEOF
printf 'clone source payload\n' > "$fs/csrc/clone-source.bin"

#--- an empty subvolume, the smallest legal stream -------------------------------------------------------------------

btrfs subvolume create "$fs/minimal" > /dev/null

#--- snapshots and changes -------------------------------------------------------------------------------------------

btrfs subvolume snapshot -r "$fs/src" "$fs/snap1" > /dev/null
btrfs subvolume snapshot -r "$fs/minimal" "$fs/snap-minimal" > /dev/null
btrfs subvolume snapshot -r "$fs/csrc" "$fs/csnap1" > /dev/null

#Changes between snap1 and snap2, one per command type we want in the stream.
printf 'more data here\n' >> "$fs/src/f1.txt"
mv "$fs/src/d1/x" "$fs/src/d1/y"
rm "$fs/src/hard.txt"
printf 'brand new file\n' > "$fs/src/d1/new.txt"
truncate -s 100 "$fs/src/d1/new.txt"
mkdir "$fs/src/d2"
mknod "$fs/src/d2/zero" c 1 5
mkfifo "$fs/src/d2/pipe"
rm "$fs/src/empty/removed.txt"
rmdir "$fs/src/empty"
setfattr -x user.text "$fs/src/f1.txt"
chmod 600 "$fs/src/d1/y"
chown 4:5 "$fs/src/d1/y"
touch -d '2021-03-04 05:06:07' "$fs/src/d1/y"
#A socket is its own command type.
python3 - "$fs/src/sock" <<'PYEOF'
import socket
import sys
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.bind(sys.argv[1])
sock.close()
PYEOF
#Write deep into the sparse file so the stream carries a large offset.
head -c 2048 /dev/urandom | dd of="$fs/src/sparse.bin" bs=2048 seek=32768 conv=notrunc status=none
#A reflinked copy gives the sender something to turn into clone operations.
cp --reflink=always "$fs/src/big.bin" "$fs/src/big-copy.bin"
python3 - "$fs/src/d2/holes.bin" <<'PYEOF'
import os
import sys
with open(sys.argv[1], 'wb') as handle:
    handle.write(b'x' * 32768)
PYEOF
#Inode flags travel as FILEATTR commands, so flip a couple of them.
chattr +A "$fs/src/d1/y" 2>/dev/null || true
chattr +d "$fs/src/big-copy.bin" 2>/dev/null || true
#Shrinking an existing file is what produces a TRUNCATE command.
truncate -s 4096 "$fs/src/big.bin"
lsattr "$fs/src/d1/y" "$fs/src/big-copy.bin"
btrfs subvolume snapshot -r "$fs/src" "$fs/snap2" > /dev/null

#--- compressed and fallocate changes on the second tree -------------------------------------------------------------

python3 - "$fs/csrc/comp/repeat.txt" <<'PYEOF'
import sys
with open(sys.argv[1], 'wb') as handle:
    handle.write(b'wxyz' * 12288)
PYEOF
fallocate -p -o 4096 -l 4096 "$fs/csrc/comp/repeat.txt" 2>/dev/null || true
btrfs subvolume snapshot -r "$fs/csrc" "$fs/csnap2" > /dev/null

#--- stream generation ----------------------------------------------------------------------------------------------

emit()
{
    name=$1
    shift
    if "$@" > "$OUT/$name"; then
        printf '%s\n' "$name"
    else
        echo "skipped $name ($* failed)" >&2
        rm -f "$OUT/$name"
    fi
}

emit full-v1.bin btrfs send "$fs/snap1"
emit incremental-v1.bin btrfs send -p "$fs/snap1" "$fs/snap2"
emit nodata-v1.bin btrfs send -p "$fs/snap1" --no-data "$fs/snap2"
emit minimal-v1.bin btrfs send "$fs/snap-minimal"

emit full-v2.bin btrfs send --proto 2 "$fs/snap1"
emit incremental-v2.bin btrfs send --proto 2 -p "$fs/snap1" "$fs/snap2"
emit nodata-v2.bin btrfs send --proto 2 -p "$fs/snap1" --no-data "$fs/snap2"
emit minimal-v2.bin btrfs send --proto 2 "$fs/snap-minimal"

emit clone-v2.bin btrfs send --proto 2 -p "$fs/snap1" -c "$fs/snap1" "$fs/snap2"
emit compressed-v2.bin btrfs send --proto 2 --compressed-data -p "$fs/csnap1" "$fs/csnap2"
emit fallocate-fileattr-v2.bin btrfs send --proto 2 -p "$fs/csnap1" "$fs/csnap2"
emit multi-subvol-v1.bin btrfs send "$fs/snap-minimal" "$fs/snap1"
emit multi-subvol-omit-end-v1.bin btrfs send -e "$fs/snap-minimal" "$fs/snap1"

chmod 644 "$OUT"/*.bin
echo "fixtures written to $OUT"
