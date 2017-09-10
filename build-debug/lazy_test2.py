source = '''\
c = compile('a = 1', 'none', 'exec')
'''
c = compile(source, 'none', 'exec')
_lazy_def('a', c)

source = '''\
print(f'a is {a}')
'''
c = compile(source, 'none', 'exec')
exec(c, globals())

source = '''\
# this fails, LOAD_NAME does PyDict_GetItem() which does not use property
c = [a]
print(f'c is {c}')
'''
c = compile(source, 'none', 'exec')
exec(c, mod.__dict__)
