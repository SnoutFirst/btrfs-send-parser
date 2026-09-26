#!/bin/sh
#End to end test: real btrfs, real "btrfs send" output, the dump tool reading it through a pipe.
#
#Needs root, btrfs-progs and loop device support, so it is off by default:
#
#    cmake -S . -B build -DBTRFS_SEND_PARSER_BUILD_INTEGRATION_TESTS=ON
#    ctest --test-dir build -L integration --output-on-failure
#
#or directly:
#
#    sudo tests/integration/run.sh ./build/btrfs-send-dump
#
#The script creates and destroys its own filesystem image and leaves nothing mounted behind.

set -eu

#mkfs.btrfs and mount live in the sbin directories, which a minimal root shell does not have on PATH.
PATH="$PATH:/sbin:/usr/sbin"
export PATH

DUMP=${1:-}
if [ -z "$DUMP" ] || [ ! -x "$DUMP" ]; then
    echo "usage: $0 <path to btrfs-send-dump>" >&2
    exit 2
fi
DUMP=$(cd "$(dirname "$DUMP")" && pwd)/$(basename "$DUMP")

if [ "$(id -u)" != "0" ]; then
    echo "run.sh must run as root" >&2
    exit 2
fi
for tool in mkfs.btrfs btrfs mount umount; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        echo "run.sh needs $tool on PATH" >&2
        exit 2
    fi
done

work=$(mktemp -d)
image="$work/btrfs.img"
mounted="$work/mnt"
mkdir -p "$mounted"
failures=0

cleanup()
{
    umount "$mounted" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

fail()
{
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

pass()
{
    echo "ok: $*"
}

#Runs the dump tool on a file and returns its exit status in $status, with stdout in $out and stderr in $err.
run_dump()
{
    #Kept inside $work so the exit trap cleans them up.
    out="$work/dump.out"
    err="$work/dump.err"
    status=0
    "$DUMP" "$@" > "$out" 2> "$err" || status=$?
}

check_contains()
{
    haystack=$1
    needle=$2
    label=$3
    if grep -q -- "$needle" "$haystack"; then
        pass "$label"
    else
        fail "$label: no \"$needle\" in the output"
        sed 's/^/    /' "$haystack" >&2
    fi
}

dd if=/dev/zero of="$image" bs=1M count=256 status=none
mkfs.btrfs -q -f "$image"
mount -o loop "$image" "$mounted"
fs="$mounted"

#--- content ---------------------------------------------------------------------------------------------------------

btrfs subvolume create "$fs/src" > /dev/null
mkdir -p "$fs/src/d1"
printf 'hello integration\n' > "$fs/src/f1.txt"
head -c 65536 /dev/urandom > "$fs/src/big.bin"
ln -s f1.txt "$fs/src/link.txt"
setfattr -n user.demo -v 'xattr value' "$fs/src/f1.txt" 2>/dev/null || true
chmod 640 "$fs/src/f1.txt"
btrfs subvolume snapshot -r "$fs/src" "$fs/snap1" > /dev/null

printf 'appended\n' >> "$fs/src/f1.txt"
mv "$fs/src/d1" "$fs/src/d2"
printf 'new file\n' > "$fs/src/d2/new.txt"
truncate -s 4096 "$fs/src/big.bin"
btrfs subvolume snapshot -r "$fs/src" "$fs/snap2" > /dev/null

btrfs send "$fs/snap1" > "$work/full-v1.bin"
btrfs send -p "$fs/snap1" "$fs/snap2" > "$work/incremental-v1.bin"
btrfs send -p "$fs/snap1" --no-data "$fs/snap2" > "$work/nodata-v1.bin"

#--- the tool reads a pipe, not just a file --------------------------------------------------------------------------

if "$DUMP" < "$work/full-v1.bin" | grep -q '^1 @17 subvol '; then
    pass "reads a stream from standard input"
else
    fail "reads a stream from standard input"
fi

#--- text output ------------------------------------------------------------------------------------------------------

run_dump -s "$work/full-v1.bin"
if [ "$status" -eq 0 ]; then
    pass "full stream parses"
else
    fail "full stream parses (exit $status)"
fi
check_contains "$out" 'path="f1.txt"' "the file path shows up"
check_contains "$out" 'symlink' "the symlink shows up"
check_contains "$out" 'stream version: 1' "the stream version is reported"
check_contains "$out" 'commands:' "the summary counts commands"

run_dump -a "$work/full-v1.bin"
check_contains "$out" 'xattr_data=' "attribute listing shows payload bytes"

#--- json output ------------------------------------------------------------------------------------------------------

run_dump -f json "$work/incremental-v1.bin"
if [ "$status" -eq 0 ]; then
    pass "json output"
else
    fail "json output (exit $status)"
fi
if python3 -c '
import json, sys
document = json.load(open(sys.argv[1]))
operations = document["operations"]
assert document["stats"]["version"] == 1, document["stats"]
assert document["stats"]["commands"] == len(operations)
commands = {operation["command"] for operation in operations}
assert "snapshot" in commands, commands
assert "rename" in commands, commands
assert document["stats"]["bytes_read"] > 0
' "$out"; then
    pass "json document is valid and complete"
else
    fail "json document is valid and complete"
    sed 's/^/    /' "$out" >&2
fi

#--- the --no-data case -----------------------------------------------------------------------------------------------

run_dump -c "$work/nodata-v1.bin"
if [ "$status" -eq 0 ]; then
    pass "no-data stream parses"
else
    fail "no-data stream parses (exit $status)"
fi
check_contains "$out" 'changed "f1.txt" offset=0' "changed extent of f1.txt"

#A --no-data stream carries no file data at all.
run_dump -f jsonl "$work/nodata-v1.bin"
if grep -q '"command": "write"' "$out"; then
    fail "no-data stream must not contain WRITE commands"
else
    pass "no-data stream has no WRITE commands"
fi

#--- protocol 2 -------------------------------------------------------------------------------------------------------

if btrfs send --proto 2 "$fs/snap1" > "$work/full-v2.bin" 2>/dev/null; then
    run_dump --stats "$work/full-v2.bin"
    check_contains "$out" 'stream version: 2' "protocol 2 stream is recognised"
else
    echo "note: this btrfs-progs does not support --proto 2, skipping that part"
fi

#--- protocol 3, which is what carries fs-verity -----------------------------------------------------------------------

#In its own subvolume: a kernel able to enable verity but not to send it can refuse the older protocol versions for
#a file that has verity on, and that must not disturb the rest of the test.
btrfs subvolume create "$fs/verity" > /dev/null
if python3 "$(dirname "$0")/enable-verity.py" "$fs/verity/verity.bin" > /dev/null 2>&1; then
    btrfs subvolume snapshot -r "$fs/verity" "$fs/verity-snap" > /dev/null
    if btrfs send --proto 3 "$fs/verity-snap" > "$work/verity-v3.bin" 2>/dev/null; then
        run_dump --stats "$work/verity-v3.bin"
        check_contains "$out" 'stream version: 3' "protocol 3 stream is recognised"
        run_dump "$work/verity-v3.bin"
        check_contains "$out" 'enable_verity' "the enable_verity command is decoded"
        run_dump -f json "$work/verity-v3.bin"
        check_contains "$out" 'enable_verity' "enable_verity shows up in the json document"
    else
        echo "note: this kernel cannot send stream version 3, skipping that part"
    fi
else
    echo "note: this kernel cannot enable fs-verity on btrfs, skipping the protocol 3 part"
fi

#--- bad input is refused with a non-zero exit status ------------------------------------------------------------------

cp "$work/full-v1.bin" "$work/corrupt.bin"
#Flip a byte in the middle of the stream, which is inside some command payload.
size=$(wc -c < "$work/corrupt.bin")
offset=$((size / 2))
python3 - "$work/corrupt.bin" "$offset" <<'PY'
import sys
path, offset = sys.argv[1], int(sys.argv[2])
data = bytearray(open(path, 'rb').read())
data[offset] ^= 0x01
open(path, 'wb').write(bytes(data))
PY
run_dump "$work/corrupt.bin"
if [ "$status" -ne 0 ]; then
    pass "a corrupted stream is refused"
else
    fail "a corrupted stream is refused"
fi
check_contains "$err" 'checksum_mismatch' "the checksum failure is reported"

#With verification off the same file parses, which shows the failure came from the checksum.
run_dump --no-checksums --stats "$work/corrupt.bin"
if [ "$status" -eq 0 ]; then
    pass "checksum verification can be switched off"
else
    fail "checksum verification can be switched off (exit $status)"
fi

head -c $((size - 1)) "$work/full-v1.bin" > "$work/truncated.bin"
run_dump "$work/truncated.bin"
if [ "$status" -ne 0 ]; then
    pass "a truncated stream is refused"
else
    fail "a truncated stream is refused"
fi

run_dump --format nonsense "$work/full-v1.bin"
if [ "$status" -eq 2 ]; then
    pass "bad arguments exit with 2"
else
    fail "bad arguments exit with 2 (exit $status)"
fi

printf 'not a btrfs stream at all' > "$work/garbage.bin"
run_dump "$work/garbage.bin"
if [ "$status" -ne 0 ]; then
    pass "garbage is refused"
else
    fail "garbage is refused"
fi

#--- every fixture has to survive a pipe as well ----------------------------------------------------------------------

fixtures=$(dirname "$0")/../fixtures
for fixture in "$fixtures"/*.bin; do
    [ -e "$fixture" ] || continue
    if "$DUMP" -s < "$fixture" > /dev/null; then
        pass "fixture $(basename "$fixture") parses through a pipe"
    else
        fail "fixture $(basename "$fixture") parses through a pipe"
    fi
done

echo
if [ "$failures" -eq 0 ]; then
    echo "integration test passed"
else
    echo "integration test failed: $failures problem(s)"
fi
[ "$failures" -eq 0 ]
