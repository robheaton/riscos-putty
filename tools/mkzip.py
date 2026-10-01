#!/usr/bin/env python3
"""Build the release zip with RISC OS filetypes.

A RISC OS file's type is not part of its name, so a plain zip made on another
system would unpack as untyped files that will not run.  The convention (used
by SparkFS, Info-ZIP for RISC OS and the Zip module) is an extra field on each
entry - ID 0x4341 ("AC"), holding "ARC0", the file's load address, execution
address and attributes - and that is what this script writes.  The load and
execution addresses carry the type and the date/time (centiseconds since
1900).

The application directory in the build tree names its files the way RPCEmu's
HostFS does, with the type as a ",xxx" suffix (`!RunImage,ff8`); the suffix is
turned into the extra field and removed from the name.

    tools/mkzip.py OUT.zip DIR [DIR-or-FILE[=archive-name] ...]

Every argument after the output name is added; a name written "path=Name" is
stored as Name (a ",xxx" suffix on path gives the type).
"""
import os
import struct
import sys
import time
import zipfile

RISCOS_EPOCH_OFFSET = 2208988800          # seconds from 1900 to 1970
TYPE_DIRECTORY_MARK = 0xFFD               # what SparkFS puts in directory entries
ATTR_DEFAULT = 0x13                       # owner read+write, public read


def split_type(name):
    """'!RunImage,ff8' -> ('!RunImage', 0xff8); 'ReadMe' -> ('ReadMe', 0xfff)."""
    base, sep, ext = name.rpartition(',')
    if sep and len(ext) == 3:
        try:
            return base, int(ext, 16)
        except ValueError:
            pass
    return name, 0xFFF


def acorn_extra(ftype, mtime):
    cs = int((mtime + RISCOS_EPOCH_OFFSET) * 100)
    load = 0xFFF00000 | (ftype << 8) | ((cs >> 32) & 0xFF)
    exec_ = cs & 0xFFFFFFFF
    body = b'ARC0' + struct.pack('<III', load, exec_, ATTR_DEFAULT) + b'\0\0\0\0'
    return struct.pack('<HH', 0x4341, len(body)) + body


def add_entry(zf, arcname, ftype, mtime, data=None):
    is_dir = data is None
    if is_dir:
        arcname += '/'
    zi = zipfile.ZipInfo(arcname, time.localtime(mtime)[:6])
    zi.create_system = 13                                   # Acorn RISC OS
    zi.extra = acorn_extra(TYPE_DIRECTORY_MARK if is_dir else ftype, mtime)
    if is_dir:
        zi.external_attr = ((0o40755) << 16) | 0x10
        zf.writestr(zi, b'')
    else:
        zi.external_attr = (0o100644) << 16
        zi.compress_type = zipfile.ZIP_DEFLATED
        zf.writestr(zi, data)


def add_tree(zf, root, arc_root):
    """Add directory `root` as `arc_root`, converting ,xxx suffixes."""
    add_entry(zf, arc_root, 0, os.path.getmtime(root))
    for cur, dirs, files in os.walk(root):
        dirs.sort()
        rel = os.path.relpath(cur, root)
        base = arc_root if rel == '.' else arc_root + '/' + rel.replace(os.sep, '/')
        if rel != '.':
            add_entry(zf, base, 0, os.path.getmtime(cur))
        for f in sorted(files):
            path = os.path.join(cur, f)
            name, ftype = split_type(f)
            with open(path, 'rb') as fh:
                add_entry(zf, base + '/' + name, ftype, os.path.getmtime(path), fh.read())


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = sys.argv[1]
    with zipfile.ZipFile(out, 'w') as zf:
        for arg in sys.argv[2:]:
            path, _, arcname = arg.partition('=')
            if os.path.isdir(path):
                add_tree(zf, path, arcname or os.path.basename(path.rstrip('/')))
            else:
                name, ftype = split_type(os.path.basename(path))
                with open(path, 'rb') as fh:
                    add_entry(zf, arcname or name, ftype, os.path.getmtime(path), fh.read())
    print('wrote', out)


if __name__ == '__main__':
    main()
