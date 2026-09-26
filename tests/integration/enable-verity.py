#!/usr/bin/env python3
"""Creates a file and turns on fs-verity for it, so that a protocol 3 send has something to report.

Builds the FS_IOC_ENABLE_VERITY argument itself instead of shelling out to fsverity-utils, which is not
packaged everywhere. Exits 0 when the file is verity enabled, non zero when the kernel or the filesystem
refuses, which the integration test treats as "skip this part".

    enable-verity.py <path to file> [contents]
"""

import ctypes
import fcntl
import os
import sys

#<linux/fsverity.h>: FS_IOC_ENABLE_VERITY is _IOW('f', 133, struct fsverity_enable_arg) and that struct is
#128 bytes, so the encoded request carries 128 as its size field.
kEnableVerityRequest = 0x40806685
kHashAlgorithmSha256 = 1
kVersion = 1
kBlockSize = 4096
kEnableArgumentSize = 128


class EnableArgument(ctypes.Structure):
    _fields_ = [("version", ctypes.c_uint32),
                ("hash_algorithm", ctypes.c_uint32),
                ("block_size", ctypes.c_uint32),
                ("salt_size", ctypes.c_uint32),
                ("salt_ptr", ctypes.c_uint64),
                ("sig_size", ctypes.c_uint32),
                ("reserved1", ctypes.c_uint32),
                ("sig_ptr", ctypes.c_uint64),
                ("reserved2", ctypes.c_uint64 * 11)]


def main(argv):
    if len(argv) < 2:
        print("usage: enable-verity.py <file> [contents]", file=sys.stderr)
        return 2

    path = argv[1]
    contents = argv[2] if len(argv) > 2 else "fs-verity content\n"
    with open(path, "wb") as handle:
        handle.write(contents.encode())

    argument = EnableArgument(version=kVersion, hash_algorithm=kHashAlgorithmSha256, block_size=kBlockSize)
    if ctypes.sizeof(argument) != kEnableArgumentSize:
        print("unexpected fsverity_enable_arg size: %d" % ctypes.sizeof(argument), file=sys.stderr)
        return 1

    #The kernel insists on a read-only descriptor, and the file must not be open for writing.
    descriptor = os.open(path, os.O_RDONLY)
    try:
        fcntl.ioctl(descriptor, kEnableVerityRequest, bytes(argument))
    finally:
        os.close(descriptor)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
