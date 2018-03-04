import sys
import marshal

DEBUG = False


def debug(*args):
    if DEBUG:
        print(*args)


def init_module(name):
    debug('init module', name)
    mod = sys.modules[name]
    # global dict holding marshal data, keyed by attribute name
    mod.__lazy_data = {}
    lazy_locks = set()
    def module_getattr(name):
        debug('getattr', name)
        data = mod.__lazy_data.get(name)
        if data is None:
            raise AttributeError(name)
        if name in lazy_locks:
            raise AttributeError(name)
        debug('exec code', repr(data))
        lazy_locks.add(name)
        code = marshal.loads(data)
        exec(code, mod.__dict__)
        lazy_locks.remove(name)
        return mod.__dict__[name]
    mod.__getattr__ = module_getattr
