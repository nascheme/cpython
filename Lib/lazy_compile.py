#!/usr/bin/env python3
# Alernative compile tool for .py files, like compile_all.py.
#
# Neil Schemenauer <nas@python.ca>, Sept 2017
#
# Examine top-level functions, classes, maybe other things.  Don't load
# them into memory unless the module global is touched (e.g. __getattr__
# hook).
#
# This could potentially save vast amounts of memory and make startup time
# much faster.  I would guess that for most modules, only a fraction of
# their global names are ever accessed.  Credit to Larry Hastings for this
# idea (PHP did something similar for a big gain).
# 
# Store the code for the thing in memory when the module is awoken. Could keep
# it on disk but probably much slower due to disk IO.  Maybe hybrid scheme,
# keep really large marshal data on disk, smaller stuff in memory.  Could also
# compress marshal data.  Pack it together into a contiguous bytes object, have
# offets when we need to awaken? 
#
# We can use lazy analyzer AST walking tool to look for things that are
# safe to keep as marshal data.  I.e. anything that potentially has global
# side-effects will get loaded as per normal.
#
# TODO:
#   - tool is currently broken:
#       - should look at only top-level functions, classes
#       - use AST analyzer to see if lazy behavior is safe
#       - only looks at functions, should look at classes
#       - compile() probably called with wrong flags
#       - exec() probably not quite right

import sys
import os
import stat
import imp
import struct
import ast
import marshal
import importlib.util
import py_compile
from importlib.machinery import FileFinder, SourceFileLoader

import lazy_analyze

PY_EXT = ".py"

class FileLoader(SourceFileLoader):
    @staticmethod
    def source_to_code(data, path, *, _optimize=-1):
        node = parse(data, path)
        code = compile(node, path, 'exec', dont_inherit=True,
                       optimize=_optimize)
        print(code.co_names)
        return code


class Transformer(ast.NodeTransformer):
    def __init__(self, fn, *args, **kwargs):
        ast.NodeTransformer.__init__(self, *args, **kwargs)
        self.fn = fn
        self.lazy_defs = set()

    def _is_lazy_assign(self, node):
        return (len(node.targets) == 1 and
                isinstance(node.targets[0], ast.Name))

    def visit_Module(self, node):
        for stmt in node.body:
            stmt_name = stmt.__class__.__name__
            if stmt_name in {'FunctionDef', 'ClassDef', 'Assign'}:
                if stmt_name == 'Assign' and not self._is_lazy_assign(stmt):
                    continue
                if lazy_analyze.is_lazy_safe(stmt):
                    self.lazy_defs.add(stmt)
        if not self.lazy_defs:
            # nothing to do, skip __class__ and other stuff
            return self.generic_visit(node)
        # add import of our helper function
        if 0:
            imp = ast.ImportFrom(module='lazy_helper',
                    names=[ast.alias(name='lazy_init',
                        asname='__lazy_init'),
                        ],
                    level=0)
            ast.fix_missing_locations(imp)
            # call the helper function with module __name__
            n = ast.Name(id='__lazy_init', ctx=ast.Load())
            name = ast.Name(id='__name__', ctx=ast.Load())
            call = ast.Call(func=n, args=[name], keywords=[], starargs=None,
                    kwargs=None)
            assign_name = ast.Name(id='_lazy_def', ctx=ast.Store())
            assign = ast.Assign(targets=[assign_name], value=call)
            ast.fix_missing_locations(assign)
        # skip __future__ statements
        idx = 0
        for i, stmt in enumerate(node.body):
            if isinstance(stmt, ast.ImportFrom) and stmt.module == '__future__':
                idx = i + 1
        # insert our new code into the start of module body
        if 0:
            node.body[idx:idx] = [imp, assign]
        return self.generic_visit(node)

    def _compile_stmt(self, node):
        code = compile(ast.Module(body=[node]), self.fn, 'exec')
        return marshal.dumps(code)

    def _store_code(self, def_name, code):
        # call the helper function with marshal data and def name
        name = ast.Str(def_name)
        code = ast.Bytes(code)
        n = ast.Name(id='_lazy_def', ctx=ast.Load())
        call = ast.Call(func=n, args=[name, code], keywords=[],
                starargs=None, kwargs=None)
        call = ast.Expr(call)
        ast.fix_missing_locations(call)
        return call

    def visit_FunctionDef(self, node):
        if node not in self.lazy_defs:
            return self.generic_visit(node) # compile as normal
        code = self._compile_stmt(node)
        return self._store_code(node.name, code)

    def visit_Assign(self, node):
        if node not in self.lazy_defs:
            return self.generic_visit(node)
        else:
            code = self._compile_stmt(node)
            return self._store_code(node.targets[0].id, code)

    def visit_ClassDef(self, node):
        # bases, docstring, body, decorator_list keywords name
        if node not in self.lazy_defs:
            return self.generic_visit(node)
        else:
            code = self._compile_stmt(node)
            return self._store_code(node.name, code)


def parse(buf, filename='<string>'):
    if isinstance(buf, bytes):
        buf = importlib.util.decode_source(buf)
    try:
        node = ast.parse(buf, filename)
    except SyntaxError as e:
        # set the filename attribute
        raise SyntaxError(str(e), (filename, e.lineno, e.offset, e.text))
    t = Transformer(filename)
    return t.visit(node)


def do_compile(file, cfile, dfile=None, doraise=False, optimize=-1):
    """Byte-compile one source file to Python bytecode.

    :param file: The source file name.
    :param cfile: The target byte compiled file name.
    :param dfile: Purported file name, i.e. the file name that shows up in
        error messages.  Defaults to the source file name.
    :param doraise: Flag indicating whether or not an exception should be
        raised when a compile error is found.  If an exception occurs and this
        flag is set to False, a string indicating the nature of the exception
        will be printed, and the function will return to the caller. If an
        exception occurs and this flag is set to True, a PyCompileError
        exception will be raised.
    :param optimize: The optimization level for the compiler.  Valid values
        are -1, 0, 1 and 2.  A value of -1 means to use the optimization
        level of the current interpreter, as given by -O command line options.

    :return: Path to the resulting byte compiled file.

    Do note that FileExistsError is raised if cfile ends up pointing at a
    non-regular file or symlink. Because the compilation uses a file renaming,
    the resulting file would be regular and thus not the same type of file as
    it was previously.
    """
    if 'lazy_help' in file:
        return # skip
    # derived from py_compile.compile
    if os.path.islink(cfile):
        msg = ('{} is a symlink and will be changed into a regular file if '
               'import writes a byte-compiled file to it')
        raise FileExistsError(msg.format(cfile))
    elif os.path.exists(cfile) and not os.path.isfile(cfile):
        msg = ('{} is a non-regular file and will be changed into a regular '
               'one if import writes a byte-compiled file to it')
        raise FileExistsError(msg.format(cfile))
    loader = FileLoader('<lazy_compile>', file)
    source_bytes = loader.get_data(file)
    try:
        code = loader.source_to_code(source_bytes, dfile or file,
                                     _optimize=optimize)
    except Exception as err:
        raise # FIXME, remove
        py_exc = py_compile.PyCompileError(err.__class__, err, dfile or file)
        if doraise:
            raise py_exc
        else:
            sys.stderr.write(py_exc.msg + '\n')
            return
    try:
        dirname = os.path.dirname(cfile)
        if dirname:
            os.makedirs(dirname)
    except FileExistsError:
        pass
    source_stats = loader.path_stats(file)
    bytecode = importlib._bootstrap_external._code_to_bytecode(
            code, source_stats['mtime'], source_stats['size'])
    mode = importlib._bootstrap_external._calc_mode(file)
    importlib._bootstrap_external._write_atomic(cfile, bytecode, mode)
    return cfile

# derived from compileall.py
def _walk_dir(dir, ddir=None, maxlevels=10, quiet=0):
    if not quiet:
        print('Listing {!r}...'.format(dir))
    try:
        names = os.listdir(dir)
    except OSError:
        if quiet < 2:
            print("Can't list {!r}".format(dir))
        names = []
    names.sort()
    for name in names:
        if name == '__pycache__':
            continue
        fullname = os.path.join(dir, name)
        if ddir is not None:
            dfile = os.path.join(ddir, name)
        else:
            dfile = None
        if not os.path.isdir(fullname):
            yield fullname
        elif (maxlevels > 0 and name != os.curdir and name != os.pardir and
              os.path.isdir(fullname) and not os.path.islink(fullname)):
            yield from _walk_dir(fullname, ddir=dfile,
                                 maxlevels=maxlevels - 1, quiet=quiet)

# derived from compileall.py
def compile_dir(dir, maxlevels=10, ddir=None, force=False, rx=None,
                quiet=0, legacy=False, optimize=-1, workers=1):
    """Byte-compile all modules in the given directory tree.

    Arguments (only dir is required):

    dir:       the directory to byte-compile
    maxlevels: maximum recursion level (default 10)
    ddir:      the directory that will be prepended to the path to the
               file as it is compiled into each byte-code file.
    force:     if True, force compilation, even if timestamps are up-to-date
    quiet:     full output with False or 0, errors only with 1,
               no output with 2
    legacy:    if True, produce legacy pyc paths instead of PEP 3147 paths
    optimize:  optimization level or -1 for level of the interpreter
    workers:   maximum number of parallel workers
    """
    files = _walk_dir(dir, quiet=quiet, maxlevels=maxlevels, ddir=ddir)
    success = 1
    for file in files:
        if not compile_file(file, ddir, force, rx, quiet,
                            legacy, optimize):
            success = 0
    return success

# derived from compileall.py
def compile_file(fullname, ddir=None, force=False, rx=None, quiet=0,
                 legacy=False, optimize=-1):
    """Byte-compile one file.

    Arguments (only fullname is required):

    fullname:  the file to byte-compile
    ddir:      if given, the directory name compiled in to the
               byte-code file.
    force:     if True, force compilation, even if timestamps are up-to-date
    quiet:     full output with False or 0, errors only with 1,
               no output with 2
    legacy:    if True, produce legacy pyc paths instead of PEP 3147 paths
    optimize:  optimization level or -1 for level of the interpreter
    """
    success = 1
    name = os.path.basename(fullname)
    if ddir is not None:
        dfile = os.path.join(ddir, name)
    else:
        dfile = None
    if rx is not None:
        mo = rx.search(fullname)
        if mo:
            return success
    if os.path.isfile(fullname):
        if legacy:
            cfile = fullname[:-len(PY_EXT)] + '.pyc'
        else:
            if optimize >= 0:
                opt = optimize if optimize >= 1 else ''
                cfile = importlib.util.cache_from_source(
                                fullname, optimization=opt)
            else:
                cfile = importlib.util.cache_from_source(fullname)
            cache_dir = os.path.dirname(cfile)
        head, tail = name[:-3], name[-3:]
        if tail == PY_EXT:
            if not force:
                try:
                    mtime = int(os.stat(fullname).st_mtime)
                    expect = struct.pack('<4sl', importlib.util.MAGIC_NUMBER,
                                         mtime)
                    with open(cfile, 'rb') as chandle:
                        actual = chandle.read(8)
                    if expect == actual:
                        return success
                except OSError:
                    pass
            if not quiet:
                print('Compiling {!r}...'.format(fullname))
            try:
                ok = do_compile(fullname, cfile, dfile, True,
                                 optimize=optimize)
            except py_compile.PyCompileError as err:
                success = 0
                if quiet >= 2:
                    return success
                elif quiet:
                    print('*** Error compiling {!r}...'.format(fullname))
                else:
                    print('*** ', end='')
                # escape non-printable characters in msg
                msg = err.msg.encode(sys.stdout.encoding,
                                     errors='backslashreplace')
                msg = msg.decode(sys.stdout.encoding)
                print(msg)
            except (SyntaxError, UnicodeError, OSError) as e:
                success = 0
                if quiet >= 2:
                    return success
                elif quiet:
                    print('*** Error compiling {!r}...'.format(fullname))
                else:
                    print('*** ', end='')
                print(e.__class__.__name__ + ':', e)
            else:
                if ok == 0:
                    success = 0
    return success

# derived from compileall.py
def compile_path(skip_curdir=1, maxlevels=0, force=False, quiet=0,
                 legacy=False, optimize=-1):
    """Byte-compile all module on sys.path.

    Arguments (all optional):

    skip_curdir: if true, skip current directory (default True)
    maxlevels:   max recursion level (default 0)
    force: as for compile_dir() (default False)
    quiet: as for compile_dir() (default 0)
    legacy: as for compile_dir() (default False)
    optimize: as for compile_dir() (default -1)
    """
    success = 1
    for dir in sys.path:
        if (not dir or dir == os.curdir) and skip_curdir:
            if quiet < 2:
                print('Skipping current directory')
        else:
            success = success and compile_dir(dir, maxlevels, None,
                                              force, quiet=quiet,
                                              legacy=legacy, optimize=optimize)
    return success


def compile_package(paths, force=0, verbose=0):
    """Compile all .py files in a package.  'path' should be a list
    of directory names containing the files of the package (i.e. __path__).
    """
    for path in paths:
        compile_dir(path, quiet=0 if verbose else 1, force=force, legacy=1)


def main():
    """Script main program."""
    import argparse

    parser = argparse.ArgumentParser(
        description='Utilities to support installing Python libraries.')
    parser.add_argument('-l', action='store_const', const=0,
                        default=10, dest='maxlevels',
                        help="don't recurse into subdirectories")
    parser.add_argument('-r', type=int, dest='recursion',
                        help=('control the maximum recursion level. '
                              'if `-l` and `-r` options are specified, '
                              'then `-r` takes precedence.'))
    parser.add_argument('-f', action='store_true', dest='force',
                        help='force rebuild even if timestamps are up to date')
    parser.add_argument('-q', action='count', dest='quiet', default=0,
                        help='output only error messages; -qq will suppress '
                             'the error messages as well.')
    parser.add_argument('-b', action='store_true', dest='legacy',
                        help='use legacy (pre-PEP3147) compiled file locations')
    parser.add_argument('-d', metavar='DESTDIR',  dest='ddir', default=None,
                        help=('directory to prepend to file paths for use in '
                              'compile-time tracebacks and in runtime '
                              'tracebacks in cases where the source file is '
                              'unavailable'))
    parser.add_argument('-x', metavar='REGEXP', dest='rx', default=None,
                        help=('skip files matching the regular expression; '
                              'the regexp is searched for in the full path '
                              'of each file considered for compilation'))
    parser.add_argument('-i', metavar='FILE', dest='flist',
                        help=('add all the files and directories listed in '
                              'FILE to the list considered for compilation; '
                              'if "-", names are read from stdin'))
    parser.add_argument('compile_dest', metavar='FILE|DIR', nargs='*',
                        help=('zero or more file and directory names '
                              'to compile; if no arguments given, defaults '
                              'to the equivalent of -l sys.path'))
    parser.add_argument('-j', '--workers', default=1,
                        type=int, help='Run compileall concurrently')

    args = parser.parse_args()
    compile_dests = args.compile_dest

    if (args.ddir and (len(compile_dests) != 1
            or not os.path.isdir(compile_dests[0]))):
        parser.exit('-d destdir requires exactly one directory argument')
    if args.rx:
        import re
        args.rx = re.compile(args.rx)


    if args.recursion is not None:
        maxlevels = args.recursion
    else:
        maxlevels = args.maxlevels

    # if flist is provided then load it
    if args.flist:
        try:
            with (sys.stdin if args.flist=='-' else open(args.flist)) as f:
                for line in f:
                    compile_dests.append(line.strip())
        except OSError:
            if args.quiet < 2:
                print("Error reading file list {}".format(args.flist))
            return False

    if args.workers is not None:
        args.workers = args.workers or None

    success = True
    try:
        if compile_dests:
            for dest in compile_dests:
                if os.path.isfile(dest):
                    if not compile_file(dest, args.ddir, args.force, args.rx,
                                        args.quiet, args.legacy):
                        success = False
                else:
                    if not compile_dir(dest, maxlevels, args.ddir,
                                       args.force, args.rx, args.quiet,
                                       args.legacy, workers=args.workers):
                        success = False
            return success
        else:
            return compile_path(legacy=args.legacy, force=args.force,
                                quiet=args.quiet)
    except KeyboardInterrupt:
        if args.quiet < 2:
            print("\n[interrupted]")
        return False
    return True

def alt_main():
    # simpler interface, just compile the files in args
    for fn in sys.argv[1:]:
        if fn.endswith('.py'):
            compile_file(fn, force=True)


if __name__ == '__main__':
    exit_status = int(not alt_main())
    sys.exit(exit_status)
