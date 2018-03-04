# This is an example of how the compiler could generate code for a lazy module.
# It would be enabled by a compiler directive like __lazy_module__ = 1.  This
# makes use of my mod_getattr branch of CPython, see:
#
#       https://github.com/nascheme/cpython/tree/mod_getattr

import sys
import marshal
import lazy_module.helper

mod_name = 'demo_mod'
mod = type(__builtins__)(mod_name)
sys.modules[mod_name] = mod
lazy_module.helper.init_module(mod_name)

source = '''\
a = 1
del a
a = 2
'''
c = compile(source, 'none', 'exec')
# create lazy version of 'a', could also be function def, class def, import
# use AST analysis to determine which are safe for laziness
mod.__lazy_data['a'] = marshal.dumps(c)

print('lazy data', mod.__lazy_data)
print('mod dict', mod.__dict__)
print('mod dir()', dir(mod))
print('mod.a is', mod.a)

# now make some code that uses 'a', will fail in standard Python because
# LOAD_NAME does not trigger getattr hook.
source = '''\
print(f'a is {a}')
'''
c = compile(source, 'none', 'exec')
exec(c, mod.__dict__)

# another example of using global defs.
source = '''\
c = [a]
print(f'c is {c}')
'''
c = compile(source, 'none', 'exec')
exec(c, mod.__dict__)
