#!/usr/bin/env python3
# vim: set ai tw=74 sts=4 sw=4 et:
#
# Bundle compiled Python modules into a package (.pypkg)
#
# File format:
#   offset to index (marshalled int)
#   module flags (marshalled int)
#   module data (marshalled code object)
#   ... (repeated module flags and data)
#   index (marshalled dict {abs_name: offset})
#
# To use, set PYTHONPACKAGE to point to file

import sys
import os
import zipfile
import tempfile
import marshal
import struct
import importlib.util


def int8_to_bytes(n):
    return struct.pack(">Q", n)

def bytes_to_int8(v):
    return struct.unpack(">Q", v)[0]


class Package:
    def __init__(self, filename):
        self.fp = open(filename, 'wb+')
        self.fp.write(b'\0' * 1024) # space for header
        self.index = {} # {module_name: offset}

    def add_module(self, module_name, module_data, flags):
        self.index[module_name] = self.fp.tell()
        marshal.dump(flags, self.fp)
        self.fp.write(module_data)

    def close(self):
        index_offset = self.fp.tell()
        marshal.dump(self.index, self.fp)
        self.fp.flush()
        self.fp.seek(0)
        marshal.dump(index_offset, self.fp)
        self.fp.close()


def _walk_dir(dn):
    names = os.listdir(dn)
    names.sort()
    for name in names:
        fullname = os.path.join(dn, name)
        if not os.path.isdir(fullname):
            yield fullname
        elif os.path.isdir(fullname) and not os.path.islink(fullname):
            yield from _walk_dir(fullname)


def get_module_path(top_pkg, dn, fn):
    flags = 0
    fn = fn[len(dn)+1:]
    parts = fn.split(os.path.sep)
    assert parts[-1].endswith('.py')
    if parts[-1] == '__init__.py':
        flags |= 1 # is package
        del parts[-1]
    else:
        parts[-1] = parts[-1][:-3]
    path = '.'.join(parts)
    if top_pkg:
        path = top_pkg + '.' + path
    return path, flags


def get_module_data(fn):
    pyc_filename = importlib.util.cache_from_source(fn)
    if not os.path.exists(pyc_filename):
        return None
    with open(pyc_filename, 'rb') as fp:
        module_data = fp.read()
    # strip pyc header
    module_data = module_data[16:]
    return module_data


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--file', '-f', default=None)
    parser.add_argument('-v', '--verbose', action='count', default=0,
                        help="enable extra status output")
    parser.add_argument('dirs', nargs='+')
    args = parser.parse_args()
    print('creating package', args.file)
    pkg = Package(args.file)
    for dn in args.dirs:
        top_pkg, sep, dn = dn.partition(':')
        if not sep:
            dn = top_pkg
            top_pkg = ''
        for fn in _walk_dir(dn):
            if fn.endswith('.py'):
                module_path, flags = get_module_path(top_pkg, dn, fn)
                module_data = get_module_data(fn)
                if module_data is None:
                    print('no data for', fn)
                    continue
                print('add', module_path, len(module_data), flags)
                pkg.add_module(module_path, module_data, flags)
    pkg.close()

if __name__ == '__main__':
    main()
