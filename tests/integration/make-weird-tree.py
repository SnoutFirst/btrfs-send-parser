#!/usr/bin/env python3
"""Creates files inside the given directory whose names need escaping in any textual output."""

import os
import sys

root = os.fsencode(sys.argv[1])
directory = os.path.join(root, b'w e i r d')
os.makedirs(directory, exist_ok=True)

names = [
    b'spaces in name.txt',
    b'tab\tname.txt',
    b'newline\nname.txt',
    b'quote"name.txt',
    b'backslash\\name.txt',
    b'non-utf8-\xff-name.txt',
    'utf8-\u00e9\u65e5\u672c\u8a9e.txt'.encode('utf-8'),
    b'-leading-dash.txt',
    b'single\'quote.txt',
]

for index, name in enumerate(names):
    with open(os.path.join(directory, name), 'wb') as handle:
        handle.write(b'content of file %d\n' % index)

#An xattr value with bytes that are not valid UTF-8 and an embedded NUL.
target = os.path.join(directory, b'spaces in name.txt')
os.setxattr(target, b'user.binary', b'\x00\x01\x02\xff\x7f\x80binary\x00value')
os.setxattr(target, b'user.text', b'a value with a \xff byte')
