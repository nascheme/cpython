
"""
opcode module - potentially shared between dis and other modules which
operate on bytecodes (e.g. peephole optimizers).
"""

__all__ = ["cmp_op", "hasconst", "hasname", "hasjrel", "hasjabs",
           "haslocal", "hascompare", "hasfree", "opname", "opmap",
           "HAVE_ARGUMENT", "EXTENDED_ARG", "hasnargs",
           "USE_REGISTERS", "OPERATION_BY_CODE", "OPERATION_BY_NAME"]

# It's a chicken-and-egg I'm afraid:
# We're imported before _opcode's made.
# With exception unheeded
# (stack_effect is not needed)
# Both our chickens and eggs are allayed.
#     --Larry Hastings, 2013/11/23

try:
    from _opcode import stack_effect
    __all__.append('stack_effect')
except ImportError:
    pass

cmp_op = ('<', '<=', '==', '!=', '>', '>=', 'in', 'not in', 'is',
        'is not', 'exception match', 'BAD')

hasconst = []
hasname = []
hasjrel = []
hasjabs = []
haslocal = []
hascompare = []
hasfree = []
hasnargs = [] # unused

opmap = {}
opname = ['<%r>' % (op,) for op in range(256)]


HAVE_ARGUMENT = 90              # Opcodes from here have an argument:
EXTENDED_ARG = 144
USE_REGISTERS = 170             # Opcodes from here use registers:


class Operation:
    """
    Argument types:

     - 'imm': Immediate
     - 'call_nreg': Number of arguments for CALL_FUNCTION_REG or CALL_PROCEDURE_REG
     - 'cmp': Compare operator
     - 'const': Constat
     - 'free': Free variable
     - 'jabs': Jump absolute
     - 'jrel': Jump relative
     - 'local': Local variable
     - 'name': Name
     - 'nargs': Number of index and keyword arguments for CALL_FUNCTION
     - 'nreg': Number of registers
     - 'nreg8': Number of registers, 8 bit unsigned number
     - 'nkwreg8': Number of keyword pair (key-value) registers, 8 bit unsigned
       number. For example, the value 1 means 1 pair and so 2 registers.
     - 'reg': Register
    """
    def __init__(self, name, code, args=None):
        self.code = code
        self.name = name
        if args:
            args = tuple(args.split(','))
        else:
            args = ()

        arg_names = []
        arg_types = []
        for arg in args:
            arg_name, arg_type = arg.split(':', 1)
            arg_name = arg_name.strip()
            arg_type = arg_type.strip()

            if arg_name in arg_names:
                raise ValueError("duplicate argument name: %s" % (arg_name,))
            arg_names.append(arg_name)

            if arg_type == 'name':
                global hasname
                hasname.append(self.code)
            elif arg_type == 'jrel':
                global hasjrel
                hasjrel.append(self.code)
            elif arg_type == 'jabs':
                global hasjabs
                hasjabs.append(self.code)
            elif arg_type == 'free':
                global hasfree
                hasfree.append(self.code)
            elif arg_type == 'nargs':
                global hasnargs
                hasnargs.append(self.code)
            elif arg_type == 'const':
                global hasconst
                hasconst.append(self.code)
            elif arg_type == 'local':
                global haslocal
                haslocal.append(self.code)
            elif arg_type == 'cmp':
                global hascompare
                hascompare.append(self.code)
            elif arg_type not in ('imm', 'reg', 'nreg', 'call_nreg', 'nreg8', 'nkwreg8'):
                raise ValueError("invalid argument type: %r" % (arg_type,))

            if arg_type == 'local':
                arg_types.append('reg')
            else:
                arg_types.append(arg_type)

        self.arg_names = tuple(arg_names)
        self.arg_types = tuple(arg_types)

    def __repr__(self):
        return '<Operation %r>' % self.name

OPERATION_BY_NAME = {}
OPERATION_BY_CODE = {}

def def_operation(*args, **kwargs):
    operation = Operation(*args, **kwargs)
    opname[operation.code] = operation.name
    opmap[operation.name] = operation.code
    OPERATION_BY_NAME[operation.name] = operation
    OPERATION_BY_CODE[operation.code] = operation


def def_op(name, op):
    def_operation(name, op, "arg:name")

def name_op(name, op):
    def_operation(name, op, "arg:name")

def jrel_op(name, op):
    def_operation(name, op, "arg:jrel")

def jabs_op(name, op):
    def_operation(name, op, "arg:jabs")

# Instruction opcodes for compiled code
# Blank lines correspond to available opcodes

def_op('POP_TOP', 1)
def_op('ROT_TWO', 2)
def_op('ROT_THREE', 3)
def_op('DUP_TOP', 4)
def_op('DUP_TOP_TWO', 5)
def_op('ROT_FOUR', 6)

def_op('NOP', 9)
def_op('UNARY_POSITIVE', 10)
def_op('UNARY_NEGATIVE', 11)
def_op('UNARY_NOT', 12)

def_op('UNARY_INVERT', 15)

def_op('BINARY_MATRIX_MULTIPLY', 16)
def_op('INPLACE_MATRIX_MULTIPLY', 17)

def_op('BINARY_POWER', 19)
def_op('BINARY_MULTIPLY', 20)

def_op('BINARY_MODULO', 22)
def_op('BINARY_ADD', 23)
def_op('BINARY_SUBTRACT', 24)
def_op('BINARY_SUBSCR', 25)
def_op('BINARY_FLOOR_DIVIDE', 26)
def_op('BINARY_TRUE_DIVIDE', 27)
def_op('INPLACE_FLOOR_DIVIDE', 28)
def_op('INPLACE_TRUE_DIVIDE', 29)

def_op('GET_AITER', 50)
def_op('GET_ANEXT', 51)
def_op('BEFORE_ASYNC_WITH', 52)
def_op('BEGIN_FINALLY', 53)
def_op('END_ASYNC_FOR', 54)
def_op('INPLACE_ADD', 55)
def_op('INPLACE_SUBTRACT', 56)
def_op('INPLACE_MULTIPLY', 57)

def_op('INPLACE_MODULO', 59)
def_op('STORE_SUBSCR', 60)
def_op('DELETE_SUBSCR', 61)
def_op('BINARY_LSHIFT', 62)
def_op('BINARY_RSHIFT', 63)
def_op('BINARY_AND', 64)
def_op('BINARY_XOR', 65)
def_op('BINARY_OR', 66)
def_op('INPLACE_POWER', 67)
def_op('GET_ITER', 68)
def_op('GET_YIELD_FROM_ITER', 69)

def_op('PRINT_EXPR', 70)
def_op('LOAD_BUILD_CLASS', 71)
def_op('YIELD_FROM', 72)
def_op('GET_AWAITABLE', 73)

def_op('INPLACE_LSHIFT', 75)
def_op('INPLACE_RSHIFT', 76)
def_op('INPLACE_AND', 77)
def_op('INPLACE_XOR', 78)
def_op('INPLACE_OR', 79)
def_op('WITH_CLEANUP_START', 81)
def_op('WITH_CLEANUP_FINISH', 82)
def_op('RETURN_VALUE', 83)
def_op('IMPORT_STAR', 84)
def_op('SETUP_ANNOTATIONS', 85)
def_op('YIELD_VALUE', 86)
def_op('POP_BLOCK', 87)
def_op('END_FINALLY', 88)
def_op('POP_EXCEPT', 89)

name_op('STORE_NAME', 90)       # Index in name list
name_op('DELETE_NAME', 91)      # ""
def_operation('UNPACK_SEQUENCE', 92, 'arg:imm')   # Number of tuple items
jrel_op('FOR_ITER', 93)
def_operation('UNPACK_EX', 94, 'arg:imm')
name_op('STORE_ATTR', 95)       # Index in name list
name_op('DELETE_ATTR', 96)      # ""
name_op('STORE_GLOBAL', 97)     # ""
name_op('DELETE_GLOBAL', 98)    # ""
def_operation('LOAD_CONST', 100, 'arg:const')       # Index in const list
name_op('LOAD_NAME', 101)       # Index in name list
def_operation('BUILD_TUPLE', 102, 'arg:imm')      # Number of tuple items
def_operation('BUILD_LIST', 103, 'arg:imm')       # Number of list items
def_operation('BUILD_SET', 104, 'arg:imm')        # Number of set items
def_operation('BUILD_MAP', 105, 'arg:imm')        # Number of dict entries (upto 255)
name_op('LOAD_ATTR', 106)       # Index in name list
def_operation('COMPARE_OP', 107, 'arg:cmp')       # Comparison operator
name_op('IMPORT_NAME', 108)     # Index in name list
name_op('IMPORT_FROM', 109)     # Index in name list

jrel_op('JUMP_FORWARD', 110)    # Number of bytes to skip
jabs_op('JUMP_IF_FALSE_OR_POP', 111) # Target byte offset from beginning of code
jabs_op('JUMP_IF_TRUE_OR_POP', 112)  # ""
jabs_op('JUMP_ABSOLUTE', 113)        # ""
jabs_op('POP_JUMP_IF_FALSE', 114)    # ""
jabs_op('POP_JUMP_IF_TRUE', 115)     # ""

name_op('LOAD_GLOBAL', 116)     # Index in name list

jrel_op('SETUP_FINALLY', 122)   # Distance to target address

def_operation('LOAD_FAST', 124, 'arg:local')        # Local variable number
def_operation('STORE_FAST', 125, 'arg:local')       # Local variable number
def_operation('DELETE_FAST', 126, 'arg:local')      # Local variable number

def_operation('RAISE_VARARGS', 130, 'arg:imm')    # Number of raise arguments (1, 2, or 3)
def_operation('CALL_FUNCTION', 131, 'arg:nargs')    # #args + (#kwargs << 8, 'nargs')
def_operation('MAKE_FUNCTION', 132, 'arg:nargs')    # Number of args with default values
def_operation('BUILD_SLICE', 133, 'arg:imm')      # Number of items

def_operation('LOAD_CLOSURE', 135, 'arg:free')
def_operation('LOAD_DEREF', 136, 'arg:free')
def_operation('STORE_DEREF', 137, 'arg:free')
def_operation('DELETE_DEREF', 138, 'arg:free')

def_operation('CALL_FUNCTION_KW', 141, 'arg:nargs')      # #args + (#kwargs << 8
def_operation('CALL_FUNCTION_VAR_KW', 142, 'arg:nargs')  # #args + (#kwargs << 8

jrel_op('SETUP_WITH', 143)

def_operation('LIST_APPEND', 145, 'arg:imm')
def_operation('SET_ADD', 146, 'arg:imm')
def_operation('MAP_ADD', 147, 'arg:imm')

def_operation('LOAD_CLASSDEREF', 148, 'arg:free')

def_operation('EXTENDED_ARG', 144, 'arg:imm')

def_op('BUILD_LIST_UNPACK', 149)
def_op('BUILD_MAP_UNPACK', 150)
def_op('BUILD_MAP_UNPACK_WITH_CALL', 151)
def_op('BUILD_TUPLE_UNPACK', 152)
def_op('BUILD_SET_UNPACK', 153)

jrel_op('SETUP_ASYNC_WITH', 154)

def_op('FORMAT_VALUE', 155)
def_op('BUILD_CONST_KEY_MAP', 156)
def_op('BUILD_STRING', 157)
def_op('BUILD_TUPLE_UNPACK_WITH_CALL', 158)

name_op('LOAD_METHOD', 160)
def_op('CALL_METHOD', 161)
jrel_op('CALL_FINALLY', 162)
def_op('POP_FINALLY', 163)

# Register instructions

def_operation('PUSH_REG', 155, 'arg:reg')
def_operation('POP_REG', 153, 'result:reg')
def_operation('MOVE_REG', 154, 'result:reg, arg:reg')
def_operation('CLEAR_REG', 184, 'arg:reg')

def_operation('LOAD_CONST_REG', 150, 'result:reg, const:const')       # Index in const list
def_operation('LOAD_GLOBAL_REG', 157, 'result:reg, global:name')
def_operation('LOAD_CLOSURE_REG', 186, 'result:reg, closure:free')
def_operation('LOAD_ATTR_REG', 165, 'result:reg, owner:reg, attr:name')

def_operation('STORE_GLOBAL_REG', 177, 'global:name, value:reg')
def_operation('STORE_ATTR_REG', 178, 'owner:reg, attr:name, value:reg')
def_operation('STORE_MAP_REG', 180, 'mapping:reg, key:reg, value:reg')
def_operation('STORE_DEREF_REG', 187, 'value:reg, arg:free')
def_operation('STORE_SUBSCR_REG', 191, 'container:reg, sub:reg, value:reg')

def_operation('UNARY_NOT_REG', 148, 'result:reg, arg:reg')
def_operation('UNARY_NEGATIVE_REG', 168, 'result:reg, arg:reg')
def_operation('UNARY_POSITIVE_REG', 203, 'result:reg, arg:reg')

def_operation('BINARY_ADD_REG', 152, 'result:reg, left:reg, right:reg')
def_operation('BINARY_SUBTRACT_REG', 158, 'result:reg, left:reg, right:reg')
def_operation('BINARY_MULTIPLY_REG', 156, 'result:reg, left:reg, right:reg')
def_operation('BINARY_FLOOR_DIVIDE_REG', 166, 'result:reg, left:reg, right:reg')
def_operation('BINARY_SUBSCR_REG', 164, 'result:reg, left:reg, right:reg')
def_operation('BINARY_MODULO_REG', 190, 'result:reg, dividend:reg, divisor:reg')
def_operation('BINARY_TRUE_DIVIDE_REG', 192, 'result:reg, left:reg, right:reg')


def_operation('INPLACE_ADD_REG', 173, 'inplace:reg, arg:reg')
def_operation('INPLACE_SUBTRACT_REG', 175, 'inplace:reg, arg:reg')
def_operation('INPLACE_MULTIPLY_REG', 174, 'inplace:reg, arg:reg')
def_operation('INPLACE_FLOOR_DIVIDE_REG', 176, 'inplace:reg, arg:reg')
def_operation('INPLACE_TRUE_DIVIDE_REG', 193, 'inplace:reg, arg:reg')
def_operation('INPLACE_MODULO_REG', 194, 'inplace:reg, arg:reg')

def_operation('BUILD_SLICE2_REG', 169, 'result:reg, start:reg, stop:reg')
def_operation('BUILD_SLICE3_REG', 205, 'result:reg, start:reg, stop:reg, step:reg')
def_operation('BUILD_LIST_REG', 170, 'result:reg, length:nreg')   # + nreg x registers
def_operation('BUILD_TUPLE_REG', 172, 'result:reg, length:nreg')   # + nreg x registers
def_operation('BUILD_MAP_REG', 179, 'result:reg, length:imm')

def_operation('RETURN_VALUE_REG', 151, 'value:reg')
def_operation('CALL_FUNCTION_REG', 162, 'result:reg, func:reg, narg:call_nreg')  # followed by arguments
def_operation('CALL_PROCEDURE_REG', 183, 'func:reg, narg:call_nreg')  # followed by arguments
def_operation('UNPACK_SEQUENCE_REG', 171, 'seq:reg, nvar:nreg')   # + nreg x registers
def_operation('UNPACK_EX_REG', 206, 'seq:reg, narg_before:nreg8, reg_list:reg, narg_after:nreg8')

def_operation('COMPARE_REG', 160, 'result:reg, arg:cmp, left:reg, right:reg')
def_operation('JUMP_IF_TRUE_REG', 167, 'arg:reg, jump:jabs')
def_operation('JUMP_IF_FALSE_REG', 161, 'arg:reg, jump:jabs')

def_operation('FOR_ITER_REG', 159, 'result:reg, iter:reg, jump:jrel')
def_operation('GET_ITER_REG', 163, 'result:reg, arg:reg')

def_operation('MAKE_FUNCTION_REG', 181, 'result:reg, qualname:reg, code:reg, posdefaults:nreg8, kwdefaults:nkwreg8, num_annotations:nreg8')
def_operation('MAKE_CLOSURE_REG', 185, 'result:reg, qualname:reg, code:reg, closure:reg, posdefaults:nreg8, kwdefaults:nkwreg8, num_annotations:nreg8')
def_operation('LOAD_BUILD_CLASS_REG', 182, 'result:reg')

# unsorted
def_operation('DELETE_SUBSCR_REG', 188, 'container:reg, sub:reg')
def_operation('LOAD_DEREF_REG', 189, 'result:reg, deref:free')
def_operation('LOAD_NAME_REG', 195, 'result:reg, var:name')
def_operation('LIST_APPEND_REG', 196, 'list:reg, value:reg')
def_operation('MAP_ADD_REG', 204, 'mapping:reg, key: reg, value:reg')
def_operation('STORE_NAME_REG', 197, 'var:name, value:reg')
def_operation('IMPORT_NAME_REG', 198, 'result:reg, name:name, from:reg, level:reg')
def_operation('IMPORT_FROM_REG', 199, 'result:reg, module:reg, name:name')
def_operation('IMPORT_STAR_REG', 200, 'module:reg')
def_operation('YIELD_REG', 201, 'value:reg')
def_operation('STORE_LOCALS_REG', 202, 'locals:reg')

del def_op, name_op, jrel_op, jabs_op, def_operation
