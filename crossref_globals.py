# Cross reference list of PyObject pointers and "nm" symbol table output
import sys
import ast

# dump from gcmodule.c
refs = {}
with open('globals.txt') as fp:
    for line in fp:
        addr, _, typename = line.strip().partition(' ')
        refs[ast.literal_eval(addr)] = typename

# dump from "nm python"
syms = {}
with open('symbols.txt') as fp:
    for line in fp:
        fields = line.strip().split(' ', 3)
        if len(fields) == 3:
            addr, symtype, symname = fields
        else:
            continue
        addr = int(addr, 16)
        syms[addr] = (symtype, symname)

# this depends on how the executable is mapped into memory
offset = 94739240325120

unknown = set()
for addr, typename in sorted(refs.items()):
    addr2 = addr - offset
    if addr2 in syms:
        print(hex(addr), typename, *syms[addr2])
    else:
        print('unknown', hex(addr), typename)
