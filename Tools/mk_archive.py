# Given a list of module names, make a "py.pya" archive

# You can set PYTHONARCHIVE to point to it.  Modules will be loaded
# from there, bypassing importlib.
import sys
import marshal
import importlib

HEADER_SIZE = 512

MODULE_PROPS = [
    '__package__',
    '__path__',
    ]

MODULE_EXCLUDES = {
    '_frozen_importlib_external',
    'encodings.aliases',
    'encodings',
    'encodings.utf_8',
    'encodings.latin_1',
    }

def main():
    module_code = {}
    module_names = sys.argv[1:]
    module_code = {}
    for name in module_names:
        if name in MODULE_EXCLUDES:
            print('skipping excluded', name)
            continue
        try:
            mod = importlib.import_module(name)
        except ImportError:
            print('ImportError, skip', name)
            continue
        spec = mod.__spec__
        if spec and spec.loader:
            code = spec.loader.get_code(name)
            if code:
                mod_dict = {}
                for prop in MODULE_PROPS:
                    v = getattr(mod, prop, None)
                    if v is not None:
                        mod_dict[prop] = v
                module_code[name] = (mod_dict, code)
            else:
                print('no code for', name)
        else:
            print('skip module, no spec', name)
    with open('py.pya', 'wb') as fp:
        fp.write(b'\0'*HEADER_SIZE) # header space, mostly unused
        index = {}
        for name, (mod_dict, code) in module_code.items():
            index[name] = fp.tell()
            print(name, mod_dict)
            marshal.dump(mod_dict, fp)
            marshal.dump(code, fp)
        index_offset = fp.tell()
        print('index', index)
        marshal.dump(index, fp)
        fp.flush()
        fp.seek(0)
        marshal.dump(index_offset, fp)
        if fp.tell() > HEADER_SIZE:
            raise RuntimeError('header too large')

if __name__ == '__main__':
    main()
