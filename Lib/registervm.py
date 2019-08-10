import collections
import opcode
import struct
import types

DEBUG_REGALLOC = False

# Hardcoded number of registers in frameobject.h
FRAME_NREGISTER = 256

DONT_USE_STACK = {
    'NOP', 'SETUP_LOOP', 'SETUP_EXCEPT', 'SETUP_FINALLY', 'JUMP_ABSOLUTE',
    'JUMP_FORWARD', 'DELETE_NAME', 'BREAK_LOOP',
}

UNARY_REGISTER_TO_STACK = {
    'UNARY_NOT': 'UNARY_NOT_REG',
    'UNARY_POSITIVE': 'UNARY_POSITIVE_REG',
    'UNARY_NEGATIVE': 'UNARY_NEGATIVE_REG',
}

BINARY_REGISTER_TO_STACK = {
    'BINARY_ADD': 'BINARY_ADD_REG',
    'BINARY_SUBTRACT': 'BINARY_SUBTRACT_REG',
    'BINARY_MULTIPLY': 'BINARY_MULTIPLY_REG',
    'BINARY_FLOOR_DIVIDE': 'BINARY_FLOOR_DIVIDE_REG',
    'BINARY_TRUE_DIVIDE': 'BINARY_TRUE_DIVIDE_REG',
    'BINARY_MODULO': 'BINARY_MODULO_REG',
    'BINARY_SUBSCR': 'BINARY_SUBSCR_REG',

    'INPLACE_ADD': 'INPLACE_ADD_REG',
    'INPLACE_SUBTRACT': 'INPLACE_SUBTRACT_REG',
    'INPLACE_MULTIPLY': 'INPLACE_MULTIPLY_REG',
    'INPLACE_FLOOR_DIVIDE': 'INPLACE_FLOOR_DIVIDE_REG',
    'INPLACE_TRUE_DIVIDE': 'INPLACE_TRUE_DIVIDE_REG',
    'INPLACE_MODULO': 'INPLACE_MODULO_REG',
}

# Replace a register value (don't modify the value inplace)
OPCODES_REPLACE_REG = {
    'LOAD_CONST_REG', 'LOAD_GLOBAL_REG', 'LOAD_ATTR_REG', 'LOAD_CLOSURE_REG',

    'UNARY_NOT_REG', 'UNARY_POSITIVE_REG', 'UNARY_NEGATIVE_REG',

    'MOVE_REG',
    'COMPARE_REG',
    'CALL_FUNCTION_REG',
    'FOR_ITER_REG',
    'BUILD_TUPLE_REG', 'BUILD_LIST_REG', 'BUILD_SLICE2_REG', 'BUILD_SLICE3_REG', 'BUILD_MAP_REG',
    'UNPACK_SEQUENCE_REG',
    'POP_REG',
    'MAKE_CLOSURE_REG', 'MAKE_FUNCTION_REG',
    'IMPORT_NAME_REG', 'IMPORT_FROM_REG',
    'GET_ITER_REG',

    'BINARY_ADD_REG',
    'BINARY_SUBTRACT_REG',
    'BINARY_MULTIPLY_REG',
    'BINARY_FLOOR_DIVIDE_REG',
    'BINARY_TRUE_DIVIDE_REG',
    'BINARY_MODULO_REG',
    'BINARY_SUBSCR_REG',
} | set(UNARY_REGISTER_TO_STACK.values())

# Replace or modify (inplace) a register value
OPCODES_WRITE_INTO = OPCODES_REPLACE_REG | {
    'INPLACE_ADD_REG',
    'INPLACE_SUBTRACT_REG',
    'INPLACE_MULTIPLY_REG',
    'INPLACE_FLOOR_DIVIDE_REG',
    'INPLACE_TRUE_DIVIDE_REG',
    'INPLACE_MODULO_REG',
}

LOAD_REG_OPS = {
    'LOAD_CONST_REG', 'LOAD_ATTR_REG', 'LOAD_GLOBAL_REG',
}

LABEL = object()

def patch_code_obj(code, bytecode=None, const=None):
    """
    Usage: patch_code_obj(code, bytecode=new_bytecode)
    Usage: patch_code_obj(code, const={0:new_const})
    """
    if bytecode is None:
        bytecode = code.co_code
    consts = code.co_consts
    if const is not None:
        consts = list(consts)
        for index, value in const.items():
            consts[index] = value
        consts = tuple(consts)
    args = (
        code.co_argcount,
        code.co_kwonlyargcount,
        code.co_nlocals,
        code.co_stacksize,
        code.co_flags,
        bytecode,
        consts,
        code.co_names,
        code.co_varnames,
        code.co_filename,
        code.co_name,
        code.co_firstlineno,
        code.co_lnotab,
        code.co_freevars,
        code.co_cellvars,
    )
    return types.CodeType(*args)

def tag_loop(node):
    node.block_type = 'loop start'
    node.loop_start = node
    seen = set()
    _tag_loop(node, node, seen)

def _tag_loop(node, loop_start, seen):
    if node in seen:
        return
    node.loop_start = loop_start
    seen.add(node)
    for link in node.out_links:
        if link.link_type == 'except StopIteration':
            continue
        _tag_loop(link.block, loop_start, seen)

def detect_loops(node, seen):
    if node in seen:
        return
    seen.add(node)
    link = node.get_out_link('except StopIteration')
    if link is not None:
        link.block.block_type = 'end of loop #%s' % (1+node.index)
        tag_loop(node)
    for link in node.out_links:
        detect_loops(link.block, seen)

class RegisterTracker:
    def __init__(self):
        self.mapping = {}
        self.reverse = collections.defaultdict(set)
        # reg => (block, index)
        self.clear_reg = {}

    def step(self, block, index, instr):
        if instr.name == "CLEAR_REG":
            reg = instr[0]
            self.clear_reg[reg] = (block, index)

    def get(self, key):
        reg = self.mapping[key]
        if reg in self.clear_reg:
            block, index = self.clear_reg[reg]
            del self.clear_reg[reg]
            instr = block[index]
            block[index] = instr.copy('NOP')
        return reg

    def __setitem__(self, key, reg):
        self.mapping[key] = reg
        self.reverse[reg].add(key)

    def __contains__(self, key):
        return key in self.mapping

    def __repr__(self):
        return '<RegisterTracker %r>' % (self.mapping,)

class Config:
    def __init__(self):
        # safe options:
        self.merge_duplicate_load_const = True
        self.remove_dead_code = True
        self.quiet = False
        self.debug = False

        # may be slower (in the current implementation) because the constant
        # if maybe not used in a code path
        self.move_load_const = False

        # Move LOAD_ATTR_REG and LOAD_GLOBAL_REG outside loops, can be slower
        # if there are not used in a code path
        self.move_load_attr = False
        self.move_load_global = False

        self.merge_duplicate_load_attr = False
        self.merge_duplicate_load_global = False

    def enable_unsafe_optimizations(self):
        self.move_load_const = True
        self.merge_duplicate_load_attr = True
        self.merge_duplicate_load_global = True

    def enable_buggy_optimizations(self):
        # Fix moving LOAD_ATTR_REG: only do that when calling methods
        #
        #     result = Result()
        #     while 1:
        #         if result.done:
        #             break
        #         func(result)
        self.move_load_attr = True

        # Don't move globals out of if. Only out of loops?
        # subprocess.py:
        #
        #    if mswindows:
        #        if p2cwrite != -1:
        #            p2cwrite = msvcrt.open_osfhandle(p2cwrite.Detach(), 0)
        self.move_load_global = True

    def check(self):
        if self.merge_duplicate_load_const and not self.move_load_const:
            raise Exception("cannot merge LOAD_CONST_REG if there are not moved")

class Argument:
    def __init__(self, converter, value, arg_type, struct_format):
        self.converter = converter
        self.value = value
        self.arg_type = arg_type
        self.struct_format = struct_format

    def __hash__(self):
        return hash((self.__class__, self.value))

    def __eq__(self, other):
        return self.__class__ == other.__class__ and self.value == other.value

    @staticmethod
    def disassemble(converter, bytecode, offset, arg_type, extended_arg):
        if arg_type == 'reg':
            arg, offset = Register.disassemble(converter, bytecode, offset)
        elif arg_type in ('nreg8', 'nkwreg8'):
            arg, offset = Immediate8.disassemble(converter, bytecode, offset, arg_type)
        else:
            arg, offset = Immediate.disassemble(converter, bytecode, offset, arg_type)
            arg.value += extended_arg
        return arg, offset

    def __repr__(self):
        return '<%s arg_type=%r value=%r>' % (self.__class__.__name__, self.arg_type, self.value)

    def format(self, pos=None):
        raise NotImplementedError()

    def __str__(self):
        return self.format()

    def get_size(self):
        return struct.calcsize(self.struct_format)

    def compile(self):
        try:
            return struct.pack(self.struct_format, self.value)
        except struct.error:
            raise ValueError("invalid argument value: "
                             "arg=%r, value=%r, struct format=%r"
                             % (self, self.value, self.struct_format)) from None


class Immediate(Argument):
    def __init__(self, converter, value, arg_type):
        Argument.__init__(self, converter, value, arg_type, 'H')

    @staticmethod
    def disassemble(converter, bytecode, offset, arg_type):
        value = bytecode[offset] + bytecode[offset+1] * 256
        offset += 2
        arg = Immediate(converter, value, arg_type)
        return arg, offset

    def format(self, pos=None):
        arg = self.value
        code = self.converter.code
        if self.arg_type == "const":
            return '%r (const#%s)' % (code.co_consts[arg], arg)
        if self.arg_type == "name":
            return '%r (name#%s)' % (code.co_names[arg], arg)
        if self.arg_type == 'jrel':
            if isinstance(arg, Label):
                return '<relative jump to %s>' % (arg.format(pos=pos),)
            elif pos is not None:
                return '<relative jump to %s (%+i)>' % (pos + arg, arg)
            else:
                return '<relative jump %+i>' % arg
        if self.arg_type == 'jabs':
            if isinstance(arg, Label):
                return '<jump to %s>' % arg.format(pos=pos)
            else:
                return str(arg)
        if self.arg_type == "cmp":
            return repr(opcode.cmp_op[arg])
        if self.arg_type in ('nargs', 'call_nreg'):
            # FIXME: split into two Immediate8
            na = arg & 0xff
            nk = arg >> 8
            if nk:
                return '(%d positional, %d keyword pair)' % (na, nk)
            else:
                return '(%d positional)' % (na,)
        if self.arg_type == "free":
            mapping = code.co_cellvars + code.co_freevars
            return "%r (%s)" % (mapping[arg], arg)
        if self.arg_type in ('nreg', 'imm'):
            return str(arg)
        raise ValueError("unknown immediate type: %s" % self.arg_type)

    def compile(self):
        if isinstance(self.value, Label):
            raise ValueError("labels are not compilable")
        return Argument.compile(self)

class Immediate8(Argument):
    def __init__(self, converter, value, arg_type):
        Argument.__init__(self, converter, value, arg_type, 'B')

    @staticmethod
    def disassemble(converter, bytecode, offset, arg_type):
        value = bytecode[offset]
        offset += 1
        arg = Immediate8(converter, value, arg_type)
        return arg, offset

    def format(self, pos=None):
        return str(self.value)

class Label(Argument):
    def __init__(self, converter, value, arg_type, label_type):
        # arg_type of the original argument
        Argument.__init__(self, converter, value, arg_type, '')
        self.label_type = label_type

    def get_size(self):
        raise ValueError("labels are not compilable")

    def format(self, pos=None):
        return str(self.value)

class Register(Argument):
    def __init__(self, converter, value):
        Argument.__init__(self, converter, value, 'reg', 'H')

    @staticmethod
    def disassemble(converter, bytecode, offset):
        reg = bytecode[offset] + bytecode[offset+1] * 256
        offset += 2
        if reg < converter._first_register:
            arg = Local(converter, reg - converter._first_local)
        else:
            arg = Register(converter, reg - converter._first_register)
        return arg, offset

    def __repr__(self):
        return '<Register %s>' % self.format()

    def format(self, pos=None):
        return 'R%s' % self.value

    def compile(self):
        value = self.converter._first_register + self.value
        try:
            return struct.pack(self.struct_format, value)
        except struct.error:
            raise ValueError("invalid register: value=%r" % (self.value,)) from None

class Local(Argument):
    def __init__(self, converter, value):
        Argument.__init__(self, converter, value, 'local', 'H')
        index = converter._first_local + self.value
        self.name = converter.code.co_varnames[index]

    def __repr__(self):
        return '<Local name=%r>' % (self.name,)

    def format(self, pos=None):
        return repr(self.name)

    def compile(self):
        value = self.converter._first_local + self.value
        try:
            return struct.pack(self.struct_format, value)
        except struct.error:
            raise ValueError("invalid local: value=%r" % (self.value,)) from None

class Instruction:
    def __init__(self, converter, code=None, name=None):
        self.converter = converter
        if name is not None:
            self._name = name
            self._code = opcode.opmap[name]
        else:
            self._code = code # int
            self._name = opcode.opname[code] # str
        self.arguments = []

    def __getitem__(self, index):
        return self.arguments[index]

    def __setitem__(self, index, arg):
        self.arguments[index] = arg

    def copy(self, opcode_name, *arguments):
        code = opcode.opmap[opcode_name]
        instr = Instruction(self.converter, code)
        instr.arguments = list(arguments)
        return instr

    def real_copy(self):
        instr = Instruction(self.converter, self._code)
        instr.arguments = list(self.arguments)
        return instr

    @property
    def name(self):
        return self._name

    def get_registers(self):
        regs = []
        for arg in self:
            if isinstance(arg, (Register, Local)):
                regs.append(arg)
        return regs

    def is_terminal(self):
        """
        Is the instruction the last of a block?
        """
        return self.name in (
            "RETURN_VALUE", "RETURN_VALUE_REG", "RAISE_VARARGS",
            "JUMP_ABSOLUTE", "JUMP_FORWARD", "CONTINUE_LOOP", "BREAK_LOOP")

    def is_cond_jump(self):
        return self.name in (
            "POP_JUMP_IF_TRUE", "POP_JUMP_IF_FALSE",
            "JUMP_IF_TRUE_OR_POP", "JUMP_IF_FALSE_OR_POP",
            "JUMP_IF_TRUE_REG", "JUMP_IF_FALSE_REG")

    def _compile(self, only_size):
        arguments = self.arguments
        extended = None
        for iarg, arg in enumerate(arguments):
            if (isinstance(arg, Immediate)
            and not isinstance(arg.value, Label)
            and arg.value > 0xffff):
                arguments = list(arguments)
                if extended is not None:
                    raise ValueError("two arguments are extended")
                extended = arg.value >> 16
                value = arg.value & 0xffff
                arguments[iarg] = Immediate(arg.converter, value, arg.arg_type)
        if extended is not None:
            if only_size:
                datalen = 3
            else:
                data = struct.pack('=BH', opcode.EXTENDED_ARG, extended)
        else:
            if only_size:
                datalen = 0
            else:
                data = b''
        if only_size:
            datalen += 1
        else:
            data += struct.pack('B', self._code)
        for arg in arguments:
            if only_size:
                datalen += arg.get_size()
            else:
                data += arg.compile()
        if only_size:
            return datalen
        else:
            return data

    def compile(self):
        try:
            return self._compile(False)
        except ValueError as err:
            raise ValueError("failed to compile %r: %s" % (self, err)) from None

    def get_size(self):
        return self._compile(True)

    @staticmethod
    def disassemble(converter, bytecode, offset, extended_arg):
        code = bytecode[offset]
        offset += 1

        operation = opcode.OPERATION_BY_CODE[code]
        arguments = []
        for arg_type in operation.arg_types:
            arg, offset = Argument.disassemble(converter, bytecode, offset, arg_type, extended_arg)
            arguments.append(arg)

            nreg = None
            if arg_type == 'nreg':
                nreg = arg.value
            elif arg_type == 'call_nreg':
                arg = arg.value
                na = arg & 0xff
                nk = arg >> 8
                nreg = na + 2 * nk
            elif arg_type == 'nreg8':
                nreg = arg.value
            elif arg_type == 'nkwreg8':
                nreg = arg.value * 2
            if nreg is not None:
                for reg in range(nreg):
                    arg, offset = Register.disassemble(converter, bytecode, offset)
                    arguments.append(arg)

        instr = Instruction(converter, code)
        instr.arguments.extend(arguments)
        return instr, offset

    def format(self, pos=None, with_name=False):
        text = self._name
        args = []
        descr = opcode.OPERATION_BY_CODE[self._code]
        arg_names = descr.arg_names
        ireg = 0
        unamed = 0
        for arg in self.arguments:
            arg_name = None
            if unamed:
                unamed -= 1
            elif ireg < len(arg_names):
                arg_name = arg_names[ireg]
                ireg += 1
            arg_str = arg.format(pos=pos)
            args.append((arg_name, arg_str))
            if arg.arg_type == 'nreg8':
                unamed = arg.value
        if args:
            text_args = []
            for name, value in args:
                if with_name and name:
                    text_args.append("%s=%s" % (name, value))
                else:
                    text_args.append(value)
            text += ' ' + ', '.join(text_args)
        return text

    def use_stack(self):
        if self.name in DONT_USE_STACK:
            return False
        descr = opcode.OPERATION_BY_CODE[self._code]
        for arg_type in descr.arg_types:
            if arg_type == 'reg':
                return False
        return True

    def _is_reg_modified(self, reg, operation_set):
        if self.name == 'UNPACK_SEQUENCE_REG':
            for index, arg in enumerate(self.arguments[2:]):
                if arg == reg:
                    return 2+index, True
        elif self.name in operation_set:
            if self.arguments[0] == reg:
                return 0, True
        return False, None

    def is_reg_replaced(self, reg):
        """
        Check if the value of the specified register is replaced
        """
        return self._is_reg_modified(reg, OPCODES_REPLACE_REG)[1]

    def is_reg_modified(self, reg):
        """
        Check if the specified register is modified in-place or if its value is
        replaced
        """
        return self._is_reg_modified(reg, OPCODES_WRITE_INTO)[1]

    def index_modified_reg(self, reg):
        index = self._is_reg_modified(reg, OPCODES_WRITE_INTO)[0]
        if index is None:
            raise ValueError("the register is not modified")
        return index

    def is_reg_used(self, reg):
        return any(
            isinstance(arg, (Register, Local)) and arg == reg
            for arg in self.arguments)

    def __str__(self):
        return self.format()

    def __repr__(self):
        return '<%s>' % self.__str__()

def disassemble(obj):
    code = obj.code.co_code
    instructions = []
    n = len(code)
    i = 0
    extended_arg = 0
    while i < n:
        instr, i = Instruction.disassemble(obj, code, i, extended_arg)
        if instr.name == 'EXTENDED_ARG':
            extended_arg = instr[-1].value * 65536
        else:
            instructions.append(instr)
            extended_arg = 0
    return instructions


class StackItem:
    def __init__(self, instr_block, instr_index, arg):
        self.instr_block = instr_block
        self.instr_index = instr_index
        self.clear_block = None
        self.clear_index = None
        self.arg = arg

    def copy(self):
        item = StackItem(self.instr_block, self.instr_index, self.arg)
        item.clear_block = self.clear_block
        item.clear_index = self.clear_index
        return item

    def __repr__(self):
        return repr(self.arg)

class Stack:
    def __init__(self):
        self._stack = []
        self.has_yield_result = False

    def copy(self):
        stack = Stack()
        stack._stack = [item.copy() for item in self._stack]
        stack.has_yield_result = self.has_yield_result
        return stack

    def __len__(self):
        return len(self._stack)

    def __repr__(self):
        return '<Stack %s>' % self._stack

    def clear(self):
        del self._stack[:]

    def push(self, block, index, arg):
        if not isinstance(arg, (Local, Register)):
            raise TypeError("unsupported stack value: %s" % arg)
        item = StackItem(block, index, arg)
        self._stack.append(item)

    def clear_reg(self, block, index, arg):
        for item in reversed(self._stack):
            if item.arg != arg:
                continue
            item.clear_block = block
            item.clear_index = index
            break

    def pop(self):
        del self._stack[-1]

    def peek(self, index):
        if not(0 <= (index-1) <= len(self)):
            raise IndexError("index out of the stack: %s (stack length: %s)"
                             % (index, len(self)))
        return self._stack[-index+1].arg

    def read(self, nreg):
        if not nreg:
            return [], []
        if len(self._stack) < nreg or self.has_yield_result:
            raise ValueError("stack contains less than %s registers" % nreg)
        regs = []
        clear = []
        for item in  self._stack[-nreg:]:
            block = item.instr_block
            index = item.instr_index
            block[index] = block[index].copy('NOP')
            if item.clear_block is not None:
                block = item.clear_block
                index = item.clear_index
                block[index] = block[index].copy('NOP')
                clear.append(item.arg)
            regs.append(item.arg)
        del self._stack[-nreg:]
        return regs, clear

    def clear_instrs(self, regs, instr):
        if not regs:
            return ()
        return tuple(
            instr.copy('CLEAR_REG', reg)
            for reg in regs)

class Link:
    def __init__(self, link_type, block):
        self.link_type = link_type
        self.block = block

    def __repr__(self):
        return '<Link type=%r block=%r>' % (self.link_type, self.block)


class Block:
    def __init__(self, converter, instructions, block_index):
        self.converter = converter
        self.instructions = instructions
        self.code = converter.code
        self.config = converter.config
        # FIXME: don't rely on the block index?
        self.index = block_index
        self.block_type = 'unknown'
        # if the block is part of a loop: loop_start is the first block
        # of the loop, None otherwise
        self.loop_start = None

        self.in_links = []
        self.out_links = []

    # FIXME: remove this deprecated property?
    @property
    def in_loop(self):
        return (self.loop_start is not None)

    def add_link(self, link):
        self.out_links.append(link)
        reverse_link = Link(link.link_type, self)
        dest_block = link.block
        dest_block.in_links.append(reverse_link)

    def _get_link(self, links, link_type):
        for link in links:
            if link.link_type == link_type:
                return link
        return None

    def get_in_link(self, link_type):
        return self._get_link(self.in_links, link_type)

    def get_out_link(self, link_type):
        return self._get_link(self.out_links, link_type)

    def unref_block(self, removed_block):
        self.in_links = [
            link for link in self.in_links
            if link.block != removed_block]
        self.out_links = [
            link for link in self.out_links
            if link.block != removed_block]

    def __str__(self):
        text = 'block #%s %s' % (self.index+1, self.block_type)
        if self.loop_start is not None:
            text += ' (in loop #%s)' % (1 + self.loop_start.index)
        return text

    def __repr__(self):
        return '<Block index=%s type=%r>' % (self.index+1, self.block_type)

    def __len__(self):
        return len(self.instructions)

    def __getitem__(self, index):
        return self.instructions[index]

    def __setitem__(self, index, new_instr):
        self.instructions[index] = new_instr

    def __delitem__(self, index):
        del self.instructions[index]

    def get_size(self):
        return sum(instr.get_size() for instr in self)

    def dump(self, prefix=None, with_name=False, pos=0):
        if prefix:
            print("=== %s ===" % prefix)
        for instr in self.instructions:
            size = instr.get_size()
            print("%3i: %s" % (pos, instr.format(pos+size, with_name=with_name)))
            pos += size
        return pos

    def iter_instr(self, start=None, end=None, backward=False,
                   # stop before entering a try or except block
                   stop_on_try=False,
                   # ignore CLEAR_REG instructions
                   ignore_clear=False,
                   # FIXME: remove unused stop_at
                   # stop at the specified index of the current block
                   stop_at=None,
                   # if we started in the middle of a loop, don't restart from
                   # the beginning of the loop
                   loop=True,
                   # ignore conditional branches and set loop=False
                   move_instr=False,
                   # stop at the end of the current loop
                   only_loop=False):
        if only_loop and not self.in_loop:
            raise ValueError("%s is not part of a loop" % self)
        seen = set()
        if stop_at is not None:
            seen.add((self, stop_at, self[stop_at]))
        # FIXME: implement only_loop
        for block in self.converter.iter_blocks(self, backward, stop_on_try, move_instr, loop=loop):
            if block is self:
                instr_it = self._iter_instr(start=start, end=end, backward=backward)
                start = None
                end = None
            else:
                instr_it = block._iter_instr(backward=backward)
            for index, instr in instr_it:
                key = (block, index, instr)
                if key in seen:
                    break
                seen.add(key)
                if not ignore_clear or instr.name != "CLEAR_REG":
                    yield block, index, instr

    def get_block_after_loop(self):
        if not self.in_loop:
            raise ValueError("%s is not part of a loop" % self)
        start = self.loop_start
        for link in start.out_links:
            if link.link_type == 'except StopIteration':
                return link.block
        raise ValueError("cannot find the block after block %s" % self)

    def _iter_instr(self, start=None, end=None, backward=False):
        if start is None:
            start = 0
        if end is None:
            end = len(self)
        if not backward:
            it_slice = range(start, end, 1)
        else:
            it_slice = range(end-1, start-1, -1)
        for index in it_slice:
            instr = self[index]
            yield index, instr

    def is_reg_replaced(self, reg, start=0, end=None, **options):
        for block, index, instr in self.iter_instr(start, end, **options):
            if instr.is_reg_replaced(reg):
                return True
        return False

        return any(
            instr.is_reg_replaced(reg)
            for block, index, instr in self.iter_instr(start, end))

    def is_reg_modified(self, reg, start=0, end=None, **options):
        return any(
            instr.is_reg_modified(reg)
            for block, index, instr in self.iter_instr(start, end, **options))

    def is_reg_used(self, reg, start=0, end=None, **options):
        return any(
            instr.is_reg_used(reg)
            for block, index, instr in self.iter_instr(start, end, **options))

    def replace_register(self, first_block, index, oldreg, newreg):
        # Replace R2 with R1:
        #
        # MOVE_REG R2, R1
        # CLEAR_REG R2
        # BINARY_ADD_REG total, R2, R3
        # CLEAR_REG R2
        # =>
        # MOVE_REG R1, R1
        # NOP
        # BINARY_ADD_REG total, R1, R3
        # CLEAR_REG R1
        #
        # Keep the last CLEAR_REG, remove others
        previous_clear_reg = None
        instr_it = first_block.iter_instr(index)
        for block, index, instr in instr_it:
            if instr.name == "CLEAR_REG":
                if not isinstance(newreg, Local):
                    # newreg is a register
                    instr.arguments = [
                        newreg if isinstance(arg, (Register, Local)) and arg == oldreg else arg
                        for arg in instr.arguments]
                    if instr[0] == newreg:
                        if previous_clear_reg is not None:
                            prev_block, prev_index = previous_clear_reg
                            prev_block[prev_index] = prev_block[prev_index].copy("NOP")
                        previous_clear_reg = (block, index)
                else:
                    # newreg is a local
                    if instr[0] == oldreg:
                        block[index] = instr.copy("NOP")
            else:
                instr.arguments = [
                    newreg if isinstance(arg, (Register, Local)) and arg == oldreg else arg
                    for arg in instr.arguments]

    def replace_with_local(self, first_block, index, oldreg, newreg):
        # Replace R2 with 'x':
        #
        # MOVE_REG R2, R1
        # CLEAR_REG R2
        # BINARY_ADD_REG R2, 'total', R3
        # CLEAR_REG R3
        # CLEAR_REG R2
        # =>
        # MOVE_REG 'x', R1
        # NOP
        # BINARY_ADD_REG 'x', 'total', R3
        # CLEAR_REG R3
        # NOP
        for block, index, instr in first_block.iter_instr(index, loop=False):
            if instr.name == "CLEAR_REG":
                if instr[0] == oldreg:
                    block[index] = instr.copy("NOP")
            else:
                instr.arguments = [
                    newreg if isinstance(arg, (Register, Local)) and arg == oldreg else arg
                    for arg in instr.arguments]

    def new_register(self):
        return self.converter.new_register()

    def convert_step1(self):
        self._patch(self.step1)

    def convert_with_stack(self, stack):
        self.stack = stack
        self._patch_stack(self.step_with_stack)

    def convert_move_instr(self):
        self._patch(self.step_move_instr)

    def _patch(self, step):
        index = 0
        while index < len(self):
            instr = self[index]
            diff = step(instr, index)
            if diff is None:
                index += 1
            else:
                index = max(index+diff, 0)

    def _patch_stack(self, step):
        index = 0
        while index < len(self):
            instr = self[index]
            new_index = step(instr, index)
            if new_index is not None:
                index = new_index
            else:
                index += 1

    def step1(self, instr, index):
        # Rewrite stack based bytecode to register-based bytecode
        if instr.name == 'LOAD_FAST':
            instr = instr.copy('PUSH_REG', instr[0])
            self[index] = instr
            return 1

        if instr.name == 'DUP_TOP':
            reg = self.new_register()
            self[index:index+1] = (
                instr.copy('POP_REG', reg),
                instr.copy('PUSH_REG', reg),
                instr.copy('PUSH_REG', reg),
                instr.copy('CLEAR_REG', reg),
            )
            return 3

        if instr.name == 'LOAD_CONST':
            const = instr[0]
            reg = self.new_register()
            self[index:index+1] = (
                instr.copy('LOAD_CONST_REG', reg, instr[0]),
                instr.copy('PUSH_REG', reg),
                instr.copy('CLEAR_REG', reg),
            )
            return 2

        if instr.name == 'LOAD_GLOBAL':
            result = self.new_register()
            self[index:index+1] = (
                instr.copy('LOAD_GLOBAL_REG', result, instr[0]),
                instr.copy('PUSH_REG', result),
                instr.copy('CLEAR_REG', result),
            )
            return 2

        if instr.name == 'LOAD_DEREF':
            reg = self.new_register()
            self[index:index+1] = (
                instr.copy('LOAD_DEREF_REG', reg, instr[0]),
                instr.copy('PUSH_REG', reg),
                instr.copy('CLEAR_REG', reg),
            )
            return 2

        if instr.name == 'STORE_FAST':
            instr = instr.copy('POP_REG', instr[0])
            self[index] = instr
            return 1

        if instr.name == 'STORE_GLOBAL':
            reg = self.new_register()
            self[index:index+1] = (
                instr.copy('POP_REG', reg),
                instr.copy('STORE_GLOBAL_REG', instr[0], reg),
                instr.copy('CLEAR_REG', reg),
            )
            return 2

        if instr.name == 'STORE_NAME':
            reg = self.new_register()
            self[index:index+1] = (
                instr.copy('POP_REG', reg),
                instr.copy('STORE_NAME_REG', instr[0], reg),
                instr.copy('CLEAR_REG', reg),
            )
            return 2

        if instr.name == 'STORE_DEREF':
            reg = self.new_register()
            self[index:index+1] = (
                instr.copy('POP_REG', reg),
                instr.copy('STORE_DEREF_REG', reg, instr[0]),
                instr.copy('CLEAR_REG', reg),
            )
            return 2

        if instr.name == 'BUILD_MAP':
            reg = self.new_register()
            self[index:index+1] = (
                instr.copy('BUILD_MAP_REG', reg, instr[0]),
                instr.copy('PUSH_REG', reg),
                instr.copy('CLEAR_REG', reg),
            )
            return 2

        if instr.name == 'LOAD_BUILD_CLASS':
            reg = self.new_register()
            self[index:index+1] = (
                instr.copy('LOAD_BUILD_CLASS_REG', reg),
                instr.copy('PUSH_REG', reg),
                instr.copy('CLEAR_REG', reg),
            )
            return 2

        if instr.name in ('LOAD_CLOSURE', 'LOAD_NAME'):
            reg = self.new_register()
            if instr.name == 'LOAD_NAME':
                new_name = 'LOAD_NAME_REG'
            else:
                new_name = 'LOAD_CLOSURE_REG'
            self[index:index+1] = (
                instr.copy(new_name, reg, instr[0]),
                instr.copy('PUSH_REG', reg),
                instr.copy('CLEAR_REG', reg),
            )
            return 2

        if instr.name == 'ROT_TWO':
            reg1 = self.new_register()
            reg2 = self.new_register()
            self[index:index+1] = (
                instr.copy('POP_REG', reg1), # top
                instr.copy('POP_REG', reg2), # second
                instr.copy('PUSH_REG', reg1), # second
                instr.copy('PUSH_REG', reg2), # top
                instr.copy('CLEAR_REG', reg1),
                instr.copy('CLEAR_REG', reg2),
            )
            return 4

        if instr.name == 'ROT_THREE':
            reg1 = self.new_register()
            reg2 = self.new_register()
            reg3 = self.new_register()
            self[index:index+1] = (
                instr.copy('POP_REG', reg1), # top
                instr.copy('POP_REG', reg2), # second
                instr.copy('POP_REG', reg3), # third
                instr.copy('PUSH_REG', reg1), # third
                instr.copy('PUSH_REG', reg3), # second
                instr.copy('PUSH_REG', reg2), # top
                instr.copy('CLEAR_REG', reg1),
                instr.copy('CLEAR_REG', reg2),
                instr.copy('CLEAR_REG', reg3),
            )
            return 4

        if instr.name in UNARY_REGISTER_TO_STACK:
            reg1 = self.new_register()
            reg2 = self.new_register()
            new_opcode = UNARY_REGISTER_TO_STACK[instr.name]
            self[index:index+1] = (
                instr.copy('POP_REG', reg1),
                instr.copy(new_opcode, reg2, reg1),
                instr.copy('CLEAR_REG', reg1),
                instr.copy('PUSH_REG', reg2),
                instr.copy('CLEAR_REG', reg2),
            )
            return index

        if instr.name == 'GET_ITER':
            reg1 = self.new_register()
            result = self.new_register()
            self[index:index+1] = (
                instr.copy('POP_REG', reg1),
                instr.copy('GET_ITER_REG', result, reg1),
                instr.copy('CLEAR_REG', reg1),
                instr.copy('PUSH_REG', result),
                instr.copy('CLEAR_REG', result),
            )
            return index

    def step_with_stack(self, instr, index):
        if (instr.name in BINARY_REGISTER_TO_STACK
        and len(self.stack) >= 2):
            # PUSH_reg left
            # PUSH_reg right
            # BINARY_ADD
            # =>
            # BINARY_ADD_REG reg3, left, right
            # PUSH_REG reg3
            stack, clear = self.stack.read(2)
            left = stack[0]
            right = stack[1]
            new_opcode = BINARY_REGISTER_TO_STACK[instr.name]
            if not new_opcode.startswith('INPLACE_'):
                result = self.new_register()
                new_instr = instr.copy(new_opcode, result, left, right)
                self[index:index+1] = (
                    (new_instr,)
                    + self.stack.clear_instrs(clear, instr)
                    + (instr.copy('PUSH_REG', result),
                       instr.copy('CLEAR_REG', result))
                )
            else:
                result = left
                new_instr = instr.copy(new_opcode, left, right)
                self[index:index+1] = (
                    (new_instr,)
                    + (instr.copy('PUSH_REG', result),)
                    + self.stack.clear_instrs(clear, instr)
                )
            return index

        if (instr.name == 'FOR_ITER'
        and len(self.stack) >= 1):
            stack, clear = self.stack.read(1)
            # PUSH_REG iterator
            # FOR_ITER label
            # =>
            # FOR_ITER_REG value, iterator, label
            # PUSH_REG value
            # CLEAR_REG value
            # ...
            # CLEAR_REG iterator
            done_link = self.get_out_link('except StopIteration')
            if done_link is None:
                raise ValueError("%s has no StopIteration block" % self)
            iterator = stack[0]
            result = self.new_register()
            self[index:index+1] = (
                instr.copy('FOR_ITER_REG', result, iterator, instr[0]),
                instr.copy('PUSH_REG', result),
                instr.copy('CLEAR_REG', result),
            )
            clear_instrs = self.stack.clear_instrs(clear, instr)
            if clear_instrs:
                done_link.block[0:0] = clear_instrs
            return index

        if (instr.name == 'LOAD_ATTR'
        and len(self.stack) >= 1):
            # PUSH_REG reg1
            # LOAD_ATTR name
            # =>
            # LOAD_ATTR_REG reg0, reg1, name
            # PUSH_REG reg0
            stack, clear = self.stack.read(1)
            attr = instr[0]
            reg_owner = stack[0]
            result = self.new_register()
            self[index:index+1] = (
                (instr.copy('LOAD_ATTR_REG', result, reg_owner, instr[0]),)
                + self.stack.clear_instrs(clear, instr)
                + (instr.copy('PUSH_REG', result),
                   instr.copy('CLEAR_REG', result),)
            )
            return index

        if (instr.name == "POP_TOP"
        and len(self.stack) >= 1):
            # PUSH_REG reg
            # POP_TOP
            # =>
            # CLEAR_REG reg
            stack, clear = self.stack.read(1)
            reg = stack[0]
            self[index:index+1] = (
                instr.copy('CLEAR_REG', reg),
            ) + self.stack.clear_instrs(clear, instr)
            return index

        if (instr.name == "YIELD_VALUE"
        and len(self.stack) >= 1):
            stack, clear = self.stack.read(1)
            reg = stack[0]
            result = self.new_register()
            self[index:index+1] = (
                (instr.copy('YIELD_REG', reg),)
                + self.stack.clear_instrs(clear, instr)
                + (instr.copy('POP_REG', result),
                   instr.copy('PUSH_REG', result),
                   instr.copy('CLEAR_REG', result),)
            )
            return index

        if (instr.name in ("JUMP_IF_TRUE_OR_POP", "JUMP_IF_FALSE_OR_POP")
        and len(self.stack) >= 1):
            # PUSH_REG reg
            # JUMP_IF_FALSE_OR_POP label
            # =>
            # JUMP_IF_FALSE_REG reg, label
            stack, clear = self.stack.read(1)
            # FIXME: factorize with ('POP_JUMP_IF_FALSE', 'POP_JUMP_IF_TRUE')
            if index != len(self)-1:
                raise ValueError("%s is not the last instruction of %s" % (instr, self))
            else_link = self.get_out_link('jump else')
            if else_link is None:
                raise ValueError("%s is not linked to an else block" % self)

            reg = stack[0]
            if instr.name == "JUMP_IF_TRUE_OR_POP":
                new_name = 'JUMP_IF_TRUE_REG'
            else:
                new_name = 'JUMP_IF_FALSE_REG'
            self[index:index+1] = (
                instr.copy(new_name, reg, instr[0]),
            )
            clear_instrs = self.stack.clear_instrs(clear, instr)
            if clear_instrs:
                else_link.block[0:0] = clear_instrs
            return index

        if (instr.name in ('POP_JUMP_IF_FALSE', 'POP_JUMP_IF_TRUE')
        and len(self.stack) >= 1):
            stack, clear = self.stack.read(1)
            # FIXME: factorize with ("JUMP_IF_TRUE_OR_POP", "JUMP_IF_FALSE_OR_POP")
            if index != len(self)-1:
                raise ValueError("%s is not the last instruction of %s" % (instr, self))
            links = self.out_links
            if (len(links) != 2
            or set(link.link_type for link in links) != {'jump if', 'jump else'}):
                raise ValueError("%s has invalid links for %s: %s" % (self, instr, links))

            # PUSH_REG reg
            # POP_JUMP_IF_FALSE cmp
            # =>
            # JUMP_IF_FALSE_REG reg, cmp
            reg = stack[0]
            if instr.name == 'POP_JUMP_IF_TRUE':
                new_opcode = 'JUMP_IF_TRUE_REG'
            else:
                new_opcode = 'JUMP_IF_FALSE_REG'
            jump_if = instr.copy(new_opcode, reg, instr[0])
            self[index:index+1] = (
                jump_if,
            )
            clear_instrs = self.stack.clear_instrs(clear, instr)
            if clear_instrs:
                for link in links:
                    link.block[0:0] = [instr.real_copy() for instr in clear_instrs]
            return index


        if (instr.name == 'STORE_ATTR'
        and len(self.stack) >= 2):
            # PUSH_REG reg_obj
            # PUSH_REG reg_value
            # STORE_ATTR name
            # =>
            # STORE_ATTR_REG reg_obj, name, reg_value
            stack, clear = self.stack.read(2)
            owner = stack[1]
            attr = instr[0]
            value = stack[0]
            self[index:index+1] = (
                instr.copy('STORE_ATTR_REG', owner, attr, value),
            ) + self.stack.clear_instrs(clear, instr)
            return index

        if (instr.name == "STORE_LOCALS"
        and len(self.stack) >= 1):
            stack, clear = self.stack.read(1)
            self[index:index+1] = (
                instr.copy('STORE_LOCALS_REG', stack[0]),
            ) + self.stack.clear_instrs(clear, instr)
            return index

        if (instr.name == 'COMPARE_OP'
        and len(self.stack) >= 2):
            # PUSH_REG reg1
            # PUSH_REG reg2
            # COMPARE_OP cmp
            # =>
            # COMPARE_REG reg3, reg2, reg1
            # PUSH_REG reg3
            stack, clear = self.stack.read(2)
            reg1 = stack[0]
            reg2 = stack[1]
            result = self.new_register()
            self[index:index+1] = (
                (instr.copy('COMPARE_REG', result, instr[0], reg1, reg2),)
                + self.stack.clear_instrs(clear, instr)
                + (instr.copy('PUSH_REG', result),
                   instr.copy('CLEAR_REG', result),)
            )
            return index+1

        if instr.name == 'CALL_FUNCTION':
            # PUSH_REG reg_func
            # PUSH_REG reg_arg1
            # PUSH_REG reg_arg2
            # ...
            # CALL_FUNCTION narg
            # =>
            # CALL_FUNCTION_REG res_result, reg_func, narg, reg_arg1, reg_arg2, ...
            # PUSH_REG res_result
            n = instr[0].value
            na = n & 0xff
            nk = n >> 8
            narg = 1 + na + 2 * nk
            if len(self.stack) >= narg:
                stack, clear = self.stack.read(narg)
                func = stack[0]
                regs = stack[1:]
                narg_arg = Immediate(self, instr[0].value, 'call_nreg')
                if (index+1 < len(self)
                and self[index+1].name == 'POP_TOP'):
                    # PUSH_REG reg1
                    # PUSH_REG reg2
                    # ...
                    # CALL_FUNCTION n
                    # POP_TOP
                    # =>
                    # CALL_PROCEDURE_REG func, reg2, ..., regn
                    # PUSH_REG reg0
                    self[index:index+2] = (
                        instr.copy('CALL_PROCEDURE_REG', func, narg_arg, *regs),
                    ) + self.stack.clear_instrs(clear, instr)
                else:
                    result = self.new_register()
                    self[index:index+1] = (
                        (instr.copy('CALL_FUNCTION_REG', result, func, narg_arg, *regs),)
                        + self.stack.clear_instrs(clear, instr)
                        + (instr.copy('PUSH_REG', result),
                           instr.copy('CLEAR_REG', result),)
                    )
                return index+1

        if (instr.name == 'LIST_APPEND'
        and len(self.stack) >= 2
        and instr[0].value-2 < len(self.stack)-1):
            # PUSH_REG reg_list
            # ...
            # PUSH_REG reg_value
            # LIST_APPEND 2
            # =>
            # PUSH_REG reg_list
            # ...
            # LIST_APPEND_REG reg_list, reg_value
            stack, clear = self.stack.read(1)
            listreg = self.stack.peek(instr[0].value)
            self[index:index+1] = (
                instr.copy('LIST_APPEND_REG', listreg, stack[0]),
            ) + self.stack.clear_instrs(clear, instr)
            return index + 1

        if (instr.name == 'MAP_ADD'
        and len(self.stack) >= 2
        and (instr[0].value-2) < len(self.stack)-2):
            # PUSH_REG reg_mapping
            # ...
            # PUSH_REG reg_key
            # PUSH_REG reg_value
            # MAP_ADD 2
            # =>
            # PUSH_REG reg_mapping
            # ...
            # MAP_ADD_REG reg_mapping, reg_key, reg_value

            # [map, key, value] pop; pop; peek(2)
            stack, clear = self.stack.read(2)
            reg_mapping = self.stack.peek(instr[0].value)
            reg_value = stack[0]
            reg_key = stack[1]
            self[index:index+1] = (
                instr.copy('MAP_ADD_REG', reg_mapping, reg_key, reg_value),
            ) + self.stack.clear_instrs(clear, instr)
            return index + 1

        if (instr.name == 'RETURN_VALUE'
        and len(self.stack) >= 1):
            # PUSH_REG reg
            # RETURN_VALUE
            # =>
            # RETURN_VALUE_REG reg
            stack, clear = self.stack.read(1)
            self[index:index+1] = (
                instr.copy('RETURN_VALUE_REG', stack[0]),
            ) + self.stack.clear_instrs(clear, instr)
            return index+1

        if instr.name == 'POP_REG':
            if self.stack.has_yield_result:
                self.stack.has_yield_result = False
            if len(self.stack) >= 1:
                stack, clear = self.stack.read(1)
                reg1 = stack[0]
                reg2 = instr[0]
                if reg1 != reg2:
                    # PUSH_REG reg
                    # POP_REG name
                    # =>
                    # MOVE_REG name, reg
                    self[index:index+1] = (
                        instr.copy('MOVE_REG', reg2, reg1),
                    ) + self.stack.clear_instrs(clear, instr)
                    return index
                else:
                    # PUSH_REG reg
                    # POP_REG reg
                    # =>
                    # (remove useless instructions)
                    del self[index]
                    return index

        if (instr.name == 'UNPACK_SEQUENCE'
        and len(self.stack) >= 1):
            # PUSH_REG reg_sequence
            # UNPACK_SEQUENCE 2
            # POP_REG reg_x
            # POP_REG reg_y
            # =>
            # UNPACK_SEQUENCE_REG reg_sequence, 2, reg_x, reg_y
            stack, clear = self.stack.read(1)
            arg = instr[0]
            regs = [self.new_register() for ireg in range(arg.value)]
            sequence = stack[0]
            instrs = [
                instr.copy('UNPACK_SEQUENCE_REG', sequence, arg, *regs),
            ]
            instrs.extend(self.stack.clear_instrs(clear, instr))
            for reg in reversed(regs):
                instrs.append(instr.copy('PUSH_REG', reg))
                instrs.append(instr.copy('CLEAR_REG', reg))
            self[index:index+1] = instrs
            return index+1

        if (instr.name == 'UNPACK_EX'
        and len(self.stack) >= 1):
            stack, clear = self.stack.read(1)
            sequence = stack[0]
            args = [sequence]
            arg = instr[0].value

            nreg_before = arg & 0xff
            args.append(Immediate8(self, nreg_before, 'nreg8'))
            regs = [self.new_register() for ireg in range(nreg_before)]
            args.extend(regs)

            reg_list = self.new_register()
            regs.append(reg_list)
            args.append(reg_list)

            nreg_after = arg >> 8
            args.append(Immediate8(self, nreg_after, 'nreg8'))
            regs_after = [self.new_register() for ireg in range(nreg_after)]
            regs.extend(regs_after)
            args.extend(regs_after)

            instrs = [instr.copy('UNPACK_EX_REG', *args)]
            instrs.extend(self.stack.clear_instrs(clear, instr))
            for reg in reversed(regs):
                instrs.append(instr.copy('PUSH_REG', reg))
                instrs.append(instr.copy('CLEAR_REG', reg))
            self[index:index+1] = instrs
            return index+1

        if instr.name in ('MAKE_FUNCTION', 'MAKE_CLOSURE'):
            # PUSH_REG reg_code
            # PUSH_REG reg_qualname
            # MAKE_FUNCTION
            # =>
            # MAKE_FUNCTION reg_result, reg_qualname, reg_code
            # PUSH_REG reg_result
            #
            # or
            #
            # PUSH_REG reg_closure
            # PUSH_REG reg_code
            # PUSH_REG reg_qualname
            # MAKE_FUNCTION
            # =>
            # MAKE_CLOSURE reg_result, reg_qualname, reg_code, reg_closure
            # PUSH_REG reg_result
            arg = instr[0].value
            posdefaults = arg & 0xff
            kwdefaults = (arg >> 8) & 0xff
            num_annotations = (arg >> 16) & 0x7fff

            make_closure = (instr.name == 'MAKE_CLOSURE')
            if make_closure:
                mandatory_nreg = 3
            else:
                mandatory_nreg = 2
            nreg = mandatory_nreg + posdefaults + kwdefaults * 2 + num_annotations
            if len(self.stack) >= nreg:
                stack, clear = self.stack.read(nreg)
                stack = stack[::-1]
                result = self.new_register()

                args = [result]
                args.extend(stack[:mandatory_nreg])
                sp = mandatory_nreg

                args.append(Immediate8(self, posdefaults, 'nreg8'))
                args.extend(stack[sp:sp+posdefaults])
                sp += posdefaults

                args.append(Immediate8(self, kwdefaults, 'nreg8'))
                args.extend(stack[sp:sp+kwdefaults * 2])
                sp += kwdefaults * 2

                args.append(Immediate8(self, num_annotations, 'nreg8'))
                args.extend(stack[sp:sp+num_annotations])

                if make_closure:
                    call_instr = instr.copy('MAKE_CLOSURE_REG', *args)
                else:
                    call_instr = instr.copy('MAKE_FUNCTION_REG', *args)
                self[index:index+1] = (
                    (call_instr,)
                    + self.stack.clear_instrs(clear, instr)
                    + (instr.copy('PUSH_REG', result),
                       instr.copy('CLEAR_REG', result),)
                )
                return index+1

        if (instr.name == 'BUILD_SLICE'
        and instr[0].value in (2, 3)):
            # PUSH_REG reg1
            # PUSH_REG reg2
            # BUILD_SLICE 2
            # =>
            # BUILD_SLICE2 reg3, reg1, reg2
            # PUSH_REG reg3
            nreg = instr[0].value
            if len(self.stack) >= nreg:
                stack, clear = self.stack.read(nreg)
                result = self.new_register()
                if nreg == 3:
                    new_name = 'BUILD_SLICE3_REG'
                else:
                    new_name = 'BUILD_SLICE2_REG'
                self[index:index+1] = (
                    (instr.copy(new_name, result, *stack),)
                    + self.stack.clear_instrs(clear, instr)
                    + (instr.copy('PUSH_REG', result),
                       instr.copy('CLEAR_REG', result),)
                )
                return index+1

        if instr.name in ('BUILD_TUPLE', 'BUILD_LIST'):
            # PUSH_REG reg1
            # PUSH_REG reg2
            # BUILD_LIST 2
            # =>
            # BUILD_LIST_REG 3, reg0, reg1, reg2
            # PUSH_REG reg0
            nreg = instr[0].value
            if len(self.stack) >= nreg:
                stack, clear = self.stack.read(nreg)
                result = self.new_register()
                if instr.name == 'BUILD_TUPLE':
                    new_opcode = 'BUILD_TUPLE_REG'
                else:
                    new_opcode = 'BUILD_LIST_REG'
                length = Immediate(self, nreg, 'nreg')
                build_instr = instr.copy(new_opcode, result, length, *stack)
                self[index:index+1] = (
                    (build_instr,)
                    + self.stack.clear_instrs(clear, instr)
                    + (instr.copy('PUSH_REG', result),
                       instr.copy('CLEAR_REG', result),)
                )
                return index+1

        if (instr.name == 'STORE_MAP'
        and len(self.stack) >= 3):
            # PUSH_REG reg_map
            # PUSH_REG reg_value
            # PUSH_REG reg_key
            # STORE_MAP
            # =>
            # STORE_MAP_REG reg_map, reg_key, reg_value
            # PUSH_REG reg_map
            stack, clear = self.stack.read(3)
            reg_key = stack[2]
            reg_value = stack[1]
            reg_map = stack[0]
            self[index:index+1] = (
                (instr.copy('STORE_MAP_REG', reg_map, reg_key, reg_value),)
                + self.stack.clear_instrs(clear, instr)
                + (instr.copy('PUSH_REG', reg_map),)
            )
            return index+1

        if (instr.name == 'STORE_SUBSCR'
        and len(self.stack) >= 3):
            # PUSH_REG reg_sub
            # PUSH_REG reg_container
            # PUSH_REG reg_v
            # STORE_SUBSCR
            # =>
            # STORE_SUBSCR reg_container, reg_sub, reg_value
            stack, clear = self.stack.read(3)
            reg_sub = stack[2]
            container = stack[1]
            value = stack[0]
            self[index:index+1] = (
                instr.copy('STORE_SUBSCR_REG', container, reg_sub, value),
            ) + self.stack.clear_instrs(clear, instr)
            return index+1

        if (instr.name == 'DELETE_SUBSCR'
        and len(self.stack) >= 2):
            # PUSH_REG reg_container
            # PUSH_REG reg_sub
            # DELETE_SUBSCR
            # =>
            # DELETE_SUBSCR_REG reg_container, reg_sub
            stack, clear = self.stack.read(2)
            reg_container = stack[0]
            reg_sub = stack[1]
            self[index:index+1] = (
                instr.copy('DELETE_SUBSCR_REG', reg_container, reg_sub),
            ) + self.stack.clear_instrs(clear, instr)
            return index

        if (instr.name == 'IMPORT_NAME'
        and len(self.stack) >= 2):
            # PUSH_REG reg_level
            # PUSH_REG reg_from
            # IMPORT_NAME name
            # =>
            # IMPORT_NAME_REG reg_result, name, reg_from, reg_level
            # PUSH_REG reg_result
            stack, clear = self.stack.read(2)
            reg_level = stack[0]
            reg_from = stack[1]
            result = self.new_register()
            self[index:index+1] = (
                (instr.copy('IMPORT_NAME_REG', result, instr[0], reg_from, reg_level),)
                + self.stack.clear_instrs(clear, instr)
                + (instr.copy('PUSH_REG', result),
                   instr.copy('CLEAR_REG', result),)
            )
            return index+1

        if (instr.name == 'IMPORT_FROM'
        and len(self.stack) >= 1):
            # PUSH_REG reg_module
            # IMPORT_NAME name
            # =>
            # PUSH_REG reg_module
            # IMPORT_FROM_REG reg_result, reg_module, name
            # PUSH_REG reg_result
            reg_module = self.stack.peek(1)
            result = self.new_register()
            self[index:index+1] = (
                instr.copy('IMPORT_FROM_REG', result, reg_module, instr[0]),
                instr.copy('PUSH_REG', result),
                instr.copy('CLEAR_REG', result),
            )
            return index+1

        if (instr.name == 'IMPORT_STAR'
        and len(self.stack) >= 1):
            # PUSH_REG reg_module
            # IMPORT_STAR
            # =>
            # PUSH_REG reg_module
            # IMPORT_STAR_REG reg_module
            reg_module = self.stack.peek(1)
            self[index:index+1] = (
                instr.copy('IMPORT_STAR_REG', reg_module),
            )
            return index+1

        if instr.name in ("PUSH_REG",):
            self.stack.push(self, index, instr[0])
        elif instr.name in ("CLEAR_REG",):
            self.stack.clear_reg(self, index, instr[0])
        elif instr.name in ("YIELD_VALUE", 'YIELD_REG'):
            if self.stack.has_yield_result:
                raise ValueError("yield result already set")
            self.stack.has_yield_result = True
        elif instr.name in ('POP_TOP', 'STORE_NAME'):
            if self.stack.has_yield_result:
                self.stack.has_yield_result = False
            else:
                if len(self.stack) == 0:
                    raise NotImplementedError("%s whereas the stack is empty" % instr)
                self.stack.pop()
        elif instr.name == "POP_BLOCK":
            # FIXME: is it correct to clear the stack?
            self.stack.clear()
        elif instr.use_stack():
            raise NotImplementedError("unknown instruction using stack: %s" % instr)
        #elif instr.is_cond_jump():
        #    raise NotImplementedError("unable to track the stack with conditional jump: %s" % instr)

    def step_peepholer2(self, instr, index):
        if instr.name != "CLEAR_REG":
            for arg in instr:
                if not isinstance(arg, (Local, Register)):
                    continue
                reg = arg.value
                if reg in self.cleared_registers:
                    self.cleared_registers.remove(reg)

        if instr.name == 'NOP':
            # Remove NOP instructions
            del self[index]
            return 0

        if (instr.name == "CLEAR_REG"
        and 1 <= index
        and self[index-1].name == "POP_REG"
        and self[index-1][0] == instr[0]):
            # POP_REG reg
            # CLEAR_REG reg
            # =>
            # POP_TOP
            reg = instr[0]
            self[index-1:index+1] = (instr.copy('POP_TOP'),)

            while index < len(self):
                instr = self[index]
                if instr.name == "CLEAR_REG" and instr[0] == reg:
                    self[index] = instr.copy("NOP")
                index += 1
            return 0

        if (self.config.remove_dead_code
        and instr.name in ('JUMP_ABSOLUTE', 'JUMP_FORWARD')):
            block = instr[0].value.value
            if block.index == self.index + 1:
                # block1:
                # ...
                # JUMP_FORWARD block2
                # block2:
                # ...
                # =>
                # block1:
                # ...
                # block2:
                # ...
                self.converter.warning("Remove useless jump %s in %s" % (instr, self))
                del self[index]
                return 0

        if (instr.name == 'MOVE_REG'
        and instr[0] == instr[1]):
            # MOVE_REG reg1, reg1
            del self[index]
            return 0

        if instr.name == "CLEAR_REG":
            reg = instr[0].value
            if reg in self.cleared_registers:
                # CLEAR_REG reg1
                # ...
                # CLEAR_REG reg1
                # =>
                # CLEAR_REG reg1
                del self[index]
                return 0
            else:
                self.cleared_registers.add(reg)

        if instr.name in ("RETURN_VALUE", "RETURN_VALUE_REG"):
            if index != len(self)-1:
                for after in self[index+1:]:
                    self.converter.info("Remove useless %s after %s", after, instr)
                del self[index+1:]

            # FIXME: don't remove CLEAR_REG before RETURN?
            delta = -1
            while index+delta >= 0 and self[index+delta].name == "CLEAR_REG":
                self.converter.info("Remove useless %s before %s", self[index+delta], instr)
                del self[index+delta]
                delta -= 1
            if delta != -1:
                return 1 + delta

    def convert_step3(self):
        self.converter.clear_free_registers()
        self._patch(self.step_reg_alloc)
        self._patch(self.step_reg_alloc2)
        self.cleared_registers = set()
        self._patch(self.step_peepholer2)

    def step_reg_alloc2(self, instr, index):
        if instr.name == "CLEAR_REG":
            reg = instr[0]
            self.converter.add_free_register(self, index, reg)
            return

        if (instr.name == 'MOVE_REG'
        and isinstance(instr[0], Register)):
            oldreg = instr[0]   # reg
            newreg = instr[1]   # reg or local
            if not self.is_reg_modified(oldreg, index+1, loop=False):
                # MOVE_REG reg, var
                # ...
                # BINARY_ADD_REG result, var, reg
                # =>
                # ...
                # BINARY_ADD_REG result, var, var
                if DEBUG_REGALLOC:
                    self.converter.dump("Before replace 1")
                    print("REGALLOC: Remove %s: replace register %s with %s" % (instr, oldreg, newreg))
                del self[index]
                self.replace_register(self, index, oldreg, newreg)
                if DEBUG_REGALLOC:
                    self.converter.dump("After replace 1")
                    print()
                return 0

        if (instr.name in OPCODES_REPLACE_REG
        and instr.name not in ("FOR_ITER_REG", "UNPACK_SEQUENCE_REG")):
            regs = instr.get_registers()
            oldreg, regs = regs[0], regs[1:]
            loop = (instr.name == "GET_ITER_REG")
            if (isinstance(oldreg, Register)
            and oldreg not in regs
            and not self.is_reg_replaced(oldreg, index+1, loop=loop)):
                for newreg in regs:
                    if DEBUG_REGALLOC:
                        print("REGALLOC: Can reuse argument %s for %s?" % (newreg, instr))
                    if not isinstance(newreg, Register):
                        continue
                    if not self.is_reg_used(newreg, index+1, ignore_clear=True, loop=loop):
                        # LOAD_ATTR_REG result, owner, 'attr'
                        # ...
                        # PUSH_REG owner
                        # PUSH_REG result
                        # =>
                        # LOAD_ATTR_REG owner, owner, 'attr'
                        # ...
                        # PUSH_REG owner
                        # PUSH_REG owner
                        if DEBUG_REGALLOC:
                            self.converter.dump("Before replace 2")
                            print("REGALLOC: Replace %s with argument %s in %s" % (oldreg, newreg, instr))
                        instr[0] = newreg
                        self.replace_register(self, index+1, oldreg, newreg)
                        if DEBUG_REGALLOC:
                            self.converter.dump("After replace 2")
                            print()
                        return
                    if DEBUG_REGALLOC:
                        print("REGALLOC: Cannot replace %s with %s in %s" % (oldreg, newreg, instr))
                for reg_index, freereg in self.converter.iter_free_registers():
                    if DEBUG_REGALLOC:
                        print("REGALLOC: Can reuse free register %s for %s?" % (freereg, instr))
                    if not self.is_reg_used(freereg, index+1, ignore_clear=True, loop=loop):
                        # LOAD_CONST_REG reg1, const
                        # CLEAR_REG reg1
                        # ...
                        # LOAD_CONST_REG reg2, const
                        # =>
                        # LOAD_CONST_REG reg1, const
                        # ...
                        # LOAD_CONST_REG reg1, const
                        if DEBUG_REGALLOC:
                            self.dump("Before replace 3")
                            print("REGALLOC: Replace %s with free reg %s in %s" % (oldreg, freereg, instr))
                            print()
                        self.converter.remove_free_register(reg_index)
                        instr[0] = freereg
                        self.replace_register(self, index+1, oldreg, freereg)
                        return
                    if DEBUG_REGALLOC:
                        print("REGALLOC: Can reuse free register %s for %s? NO" % (freereg, instr))

    def step_reg_alloc(self, instr, index):
        if (instr.name == 'MOVE_REG'
        and isinstance(instr[0], Local)):
            # BINARY_ADD_REG reg3, reg2, reg1
            # ...
            # MOVE_REG var, reg3
            # CLEAR_REG reg3
            # =>
            # BINARY_ADD_REG var, reg2, reg1
            # ...
            oldreg = instr[1]   # local or reg
            newreg = instr[0]   # local

            found = None
            move_index = index
            for prev_block, prev_index, prev_instr in self.iter_instr(end=index, backward=True):
                if prev_instr.is_reg_replaced(oldreg):
                    found = (prev_block, prev_index, prev_instr)
                    break
                if prev_instr.is_reg_modified(oldreg):
                    break
            if found is not None:
                prev_block, prev_index, prev_instr = found
                ireg = prev_instr.index_modified_reg(oldreg)
                if DEBUG_REGALLOC:
                    self.converter.dump()
                    print("REGALLOC: Remove %s: replace %s with register %s in %s"
                          % (self[move_index], oldreg, newreg, prev_instr))
                    print()
                prev_instr[ireg] = newreg
                del self[move_index]
                self.replace_with_local(prev_block, prev_index+1, oldreg, newreg)
                return 0

    def can_move_load_const(self, instr, load_instr):
        if instr.name == 'LOAD_CONST_REG':
            return False
        if instr.is_reg_used(load_instr[0]):
            return False
        return True

    def can_move_load_attr(self, instr, load_instr):
        if instr.name in ('LOAD_CONST_REG', 'LOAD_ATTR_REG'):
            return False
        if instr.is_reg_used(load_instr[0]):
            return False
        if instr.is_reg_used(load_instr[1]):
            return False
        return True

    def can_move_load_global(self, instr, load_instr):
        if instr.name in ('LOAD_CONST_REG', 'LOAD_ATTR_REG', 'LOAD_GLOBAL_REG'):
            return False
        if instr.name == 'STORE_GLOBAL_REG' and instr[1] == load_instr[1]:
            return False
        if instr.is_reg_used(load_instr[0]):
            return False
        return True

    def may_move_instr(self, move_instr, move_index, can_move, stop_on_try=True):
        # Can we move the instruction?
        found = None
        instr_it = self.iter_instr(end=move_index, stop_on_try=stop_on_try, move_instr=True, backward=True)
        for block, index, instr in instr_it:
            if not can_move(instr, move_instr):
                break
            found = (block, index, instr)
        if found is None:
            return False

        # The instruction will be moved: check if the register is cleared below
        clear_reg = None
        if self.loop_start != found[0].loop_start:
            reg = move_instr[0]
            instr_it = self.iter_instr(move_index, stop_on_try=stop_on_try, move_instr=True, only_loop=True)
            for block, index, instr in instr_it:
                if instr.name == "CLEAR_REG" and instr[0] == reg:
                    clear_reg = (block, index, instr)
                    break

        # Move CLEAR_REG (if any)
        if clear_reg is not None:
            block, index, instr = clear_reg
            block[index] = instr.copy('NOP')
            block_after = self.get_block_after_loop()
            block_after[0:0] = (instr,)

        # Move the instruction
        block, index, instr = found
        self[move_index] = move_instr.copy('NOP')
        block[index:index+1] = (move_instr, instr)

        return True

    def step_move_instr(self, instr, index):
        # Move LOAD_CONST_REG to the beginning
        if (self.config.move_load_const
        and instr.name == 'LOAD_CONST_REG'):
            # PUSH_REG reg1
            # ...
            # LOAD_CONST_REG reg2, const
            # =>
            # LOAD_CONST_REG reg2, const
            # ...
            # PUSH_REG reg1
            if self.may_move_instr(instr, index, self.can_move_load_const, stop_on_try=False):
                return

        # Move LOAD_ATTR_REG to the beginning
        if (self.config.move_load_attr
        and instr.name == 'LOAD_ATTR_REG'):
            # PUSH_REG reg1
            # LOAD_ATTR_REG reg3, reg2, name
            # =>
            # LOAD_ATTR_REG reg3, reg2, name
            # PUSH_REG reg1
            if self.may_move_instr(instr, index, self.can_move_load_attr):
                return

        # Move LOAD_GLOBAL_REG to the beginning
        if (self.config.move_load_global
        and instr.name == 'LOAD_GLOBAL_REG'):
            # PUSH_REG reg1
            # ...
            # LOAD_GLOBAL_REG reg2, name
            # =>
            # LOAD_GLOBAL_REG reg2, name
            # ...
            # PUSH_REG reg1
            if self.may_move_instr(instr, index, self.can_move_load_global):
                return

    def convert_merge_duplicate_load(self):
        self.load_consts = RegisterTracker()
        self.load_names = RegisterTracker()
        self.load_globals = RegisterTracker()
        # FIXME: merge duplicate LOAD_ATTR_REG
        ## # name => {attr => reg} (used by step_merge_duplicate_load method)
        ## self.load_attrs = None
        self._patch(self.step_merge_duplicate_load)

    def step_merge_duplicate_load(self, instr, index):
        self.load_consts.step(self, index, instr)
        self.load_names.step(self, index, instr)
        self.load_globals.step(self, index, instr)

        if instr.name == "STORE_NAME_REG":
            self.load_names[instr[0]] = instr[1]

        if instr.name == "STORE_GLOBAL_REG":
            self.load_globals[instr[0]] = instr[1]

        if (self.config.merge_duplicate_load_const
        and instr.name == "LOAD_CONST_REG"):
            if instr[1] in self.load_consts:
                # LOAD_CONST_REG reg1, var
                # ...
                # LOAD_CONST_REG reg2, var
                # =>
                # LOAD_CONST_REG reg1, var
                # ...
                # MOVE_REG reg2, reg1
                reg = self.load_consts.get(instr[1])
                self[index:index+1] = (
                    instr.copy("MOVE_REG", instr[0], reg),
                    instr.copy("CLEAR_REG", reg),
                )
                return
            else:
                self.load_consts[instr[1]] = instr[0]

        if instr.name == "LOAD_NAME_REG":
            if instr[1] in self.load_names:
                # STORE_NAME_REG var, reg1
                # ...
                # LOAD_NAME_REG reg2, var
                # =>
                # STORE_NAME_REG var, reg1
                # ...
                # MOVE_REG reg2, reg1
                reg = self.load_names.get(instr[1])
                self[index:index+1] = (
                    instr.copy("MOVE_REG", instr[0], reg),
                    instr.copy("CLEAR_REG", reg),
                )
                return
            else:
                self.load_names[instr[1]] = instr[0]

        if (self.config.merge_duplicate_load_global
        and instr.name == "LOAD_GLOBAL_REG"):
            if instr[1] in self.load_globals:
                # STORE_GLOBAL_REG var, reg1
                # ...
                # LOAD_GLOBAL_REG reg2, var
                # =>
                # STORE_GLOBAL_REG var, reg1
                # ...
                # MOVE_REG reg2, reg1
                reg = self.load_globals.get(instr[1])
                self[index:index+1] = (
                    instr.copy("MOVE_REG", instr[0], reg),
                    instr.copy("CLEAR_REG", reg)
                )
                return
            else:
                self.load_globals[instr[1]] = instr[0]

        ## if (instr.name in OPCODES_WRITE_INTO
        ## and isinstance(instr[0], Local)):
        ##     # BINARY_ADD_REG var, reg1, reg2
        ##     # => don't merge attributes of var
        ##     reg = instr[0]
        ##     if reg in self.load_attrs:
        ##         del self.load_attrs[reg]
        ## if instr.name == "STORE_ATTR_REG":
        ##     # STORE_ATTR_REG owner, attr, value
        ##     owner = instr[0]
        ##     if owner in self.load_attrs:
        ##         del self.load_attrs[owner]

        ## if (self.config.merge_duplicate_load_attr
        ## and instr.name == 'LOAD_ATTR_REG'):
        ##     result = instr[0]
        ##     owner = instr[1]
        ##     attr = instr[2]
        ##     key = (owner, attr)

        ##     try:
        ##         reg2 = self.load_attrs[owner][attr]
        ##     except KeyError:
        ##         if owner not in self.load_attrs:
        ##             self.load_attrs[owner] = {}
        ##         self.load_attrs[owner][attr] = result
        ##     else:
        ##         self[index] = instr.copy('MOVE_REG', result, reg2)
        ##         return


class Converter:
    """
    Rewrite the bytecode of a code object to use registers instead
    of the stack. Main methods:

     - convert(): rewrite the bytecode
     - compile(): return a new code object
    """
    def __init__(self, code, config=None):
        self.code = code
        if config is not None:
            self.config = config
        else:
            self.config = Config()

        self._first_local = code.co_stacksize
        self._first_register = code.co_stacksize + code.co_nlocals + len(code.co_cellvars) + len(code.co_freevars) + 1
        self._next_register = 0
        self._free_registers = collections.deque()

        self.instructions = disassemble(self)
        self.blocks = None

    def new_register(self):
        reg = self._next_register
        self._next_register += 1
        return Register(self, reg)

    def clear_free_registers(self):
        self._free_registers.clear()

    def add_free_register(self, block, index, reg):
        if not isinstance(reg, Register):
            return
        item = (reg, block, index)
        self._free_registers.append(item)

    def iter_free_registers(self):
        for index, item in enumerate(self._free_registers):
            yield (index, item[0])

    def remove_free_register(self, reg_index):
        reg, block, index = self._free_registers[reg_index]
        block[index] = Instruction(self.code, name='NOP')
        del self._free_registers[reg_index]

    def _iter_instr(self):
        for block in self.blocks:
            for instr in block:
                yield instr

    def recompute_labels(self):
        # Compute absolute position of labels and remove labels
        labels = {}
        abs_pos = 0
        for block in self.blocks:
            labels[block] = abs_pos
            abs_pos += block.get_size()

        # Replace label arguments by their value
        pos = 0
        for instr in self._iter_instr():
            pos += instr.get_size()
            for arg in instr.arguments:
                if not(isinstance(arg, Immediate) and isinstance(arg.value, Label)):
                    continue
                block = arg.value.value
                abs_pos = labels[block]
                if arg.arg_type == 'jrel':
                    arg.value = abs_pos - pos
                else:
                    arg.value = abs_pos
                # an instruction cannot have more than one label
                break

    def renumber_registers(self):
        self._next_register = 0
        mapping = {}
        for instr in self.instructions:
            for index, arg in enumerate(instr.arguments):
                if not isinstance(arg, Register):
                    continue
                if arg.value not in mapping:
                    mapping[arg.value] = self.new_register()
                arg = mapping[arg.value]
                instr.arguments[index] = arg

        nreg = self._next_register - self._first_register
        if nreg >= FRAME_NREGISTER:
            raise Exception("too much registers!")

    def create_blocks(self):
        # compute labels
        pos = 0
        labels = set()
        for instr in self.instructions:
            pos += instr.get_size()
            for arg in instr.arguments:
                if arg.arg_type == 'jabs':
                    labels.add(arg.value)
                elif arg.arg_type == 'jrel':
                    labels.add(pos + arg.value)

        # split blocks
        pos = 0
        index = 0
        start = 0
        blocks = []
        block_labels = {}
        previous = None
        previous_instr = None
        previous_pos = None
        while index < len(self.instructions):
            instr = self.instructions[index]
            pos += instr.get_size()

            link = True
            if pos in labels:
                split = True
            elif index == len(self.instructions)-1:
                split = True
            elif instr.is_terminal():
                split = True
            else:
                split = False
                for arg in instr.arguments:
                    if arg.arg_type in ('jabs', 'jrel'):
                        split = True
                        break

            if split:
                if instr.is_terminal():
                    link = False

                block = Block(self, self.instructions[start:index+1], len(blocks))
                if not blocks:
                    block.block_type = 'start'
                blocks.append(block)
                if previous is not None:
                    if previous_instr.is_cond_jump():
                        link_type = 'jump if'
                        block.block_type = 'if (#%s)' % (1 + previous.index)
                    else:
                        link_type = 'consecutive'
                    previous.add_link(Link(link_type, block))
                if link:
                    previous = block
                else:
                    previous = None
                previous_instr = instr
                if previous_pos is not None:
                    block_labels[previous_pos] = block
                previous_pos = pos
                start = index + 1

            index += 1
        block_labels[previous_pos] = previous

        # link blocks
        labels = {}
        pos = 0
        for block_index, block in enumerate(blocks):
            for instr in block.instructions:
                pos += instr.get_size()
                jump_arg = None
                for arg in instr.arguments:
                    if arg.arg_type in ('jabs', 'jrel'):
                        jump_arg = arg
                        break
                if jump_arg is None:
                    continue
                if jump_arg.arg_type == 'jabs':
                    abs_pos = jump_arg.value
                elif jump_arg.arg_type == 'jrel':
                    abs_pos = pos + jump_arg.value
                else:
                    continue

                dest_block = block_labels[abs_pos]

                if instr.name == "SETUP_EXCEPT":
                    name = 'except_%s' % (1 + len(labels))
                    label_type = 'except'
                    next_block = blocks[block_index+1]
                    next_block.block_type = 'try'
                    dest_block.block_type = 'except'
                elif instr.name == "SETUP_LOOP":
                    name = 'setup_loop_%s' % (1 + len(labels))
                    label_type = 'setup_loop'
                elif instr.name in ("FOR_ITER", "FOR_ITER_REG"):
                    name = 'for_iter_%s' % (1 + len(labels))
                    label_type = 'except StopIteration'
                elif instr.is_cond_jump():
                    name = 'cond_%s' % (1 + len(labels))
                    label_type = 'jump else'
                    dest_block.block_type = 'else (#%s)' % (1 + block.index)
                elif instr.name == "SETUP_FINALLY":
                    name = 'finally_%s' % (1 + len(labels))
                    label_type = 'finally'
                elif instr.name in ("JUMP_ABSOLUTE", "JUMP_FORWARD"):
                    name = 'label_%s' % (1 + len(labels))
                    label_type = 'jump'
                else:
                    name = 'label_%s' % (1 + len(labels))
                    label_type = 'unknown'

                block.add_link(Link(label_type, dest_block))
                for arg in instr.arguments:
                    if arg.arg_type in ('jrel', 'jabs'):
                        arg.value = Label(self.code, dest_block, arg.arg_type, label_type)
                        break
        return blocks

    def iter_blocks(self, block=None, backward=False, stop_on_try=False, move_instr=False, loop=True):
        seen = set()
        loops = set()
        blocks = collections.deque()
        if move_instr:
            loop = False

        blocks.append(block)
        while blocks:
            ignore = False
            if move_instr and len(blocks) > 1:
                # We are in a conditional branch: search a common parent
                blocks = list(blocks)
                blocks.sort(key=lambda block: block.index, reverse=True)
                blocks = collections.deque(blocks)
                ignore = True
            block = blocks.popleft()

            if stop_on_try and block.block_type in ('try', 'except'):
                return

            if not ignore:
                if block in seen:
                    if loop:
                        # emit the same block again to finish iterating on the loop
                        yield block
                    continue

                seen.add(block)
                yield block
            else:
                seen.add(block)

            if backward:
                links = block.in_links
            else:
                links = block.out_links
            for link in links:
                if move_instr and link.block.index > block.index:
                    # when moving instructions, we only want to move to the
                    # beginning (and not to the end)
                    continue
                if move_instr:
                    if link.block in seen:
                        continue
                    if link.block in blocks:
                        continue
                blocks.append(link.block)

    def info(self, message, *args):
        if self.config.debug:
            if args:
                message %= args
            print(message)

    def warning(self, message):
        if not self.config.quiet:
            print(message)

    def remove_dead_blocks(self):
        while 1:
            removed = []
            blocks = []
            for index, block in enumerate(self.blocks):
                if 1 <= index and not block.in_links:
                    text = str(block)
                    instrs = tuple(block)
                    if instrs:
                        text += " (%s)" % ', '.join(map(str, instrs))
                    self.warning("Remove dead block: %s" % text)
                    removed.append(block)
                    continue
                blocks.append(block)
            self.blocks = blocks
            if not removed:
                break
            for removed_block in removed:
                for block in blocks:
                    block.unref_block(removed_block)

    def convert(self,
                dump_original=False, dump_optimized=True,
                dump_bytecode=False, dump_blocks=False):
        if dump_original and dump_bytecode:
            self.dump(prefix="original bytecode")

        self.blocks = self.create_blocks()
        detect_loops(self.blocks[0], set())
        self.instructions = None

        if dump_original and dump_blocks:
            self.dump(prefix="original blocks")
        for block in self.blocks:
            block.convert_step1()

        stacks = {}
        for block in self.iter_blocks(self.blocks[0]):
            link = block.get_in_link('consecutive')
            if link is not None and link.block in stacks:
                stack = stacks[link.block].copy()
            else:
                stack = Stack()
            try:
                block.convert_with_stack(stack)
            except NotImplementedError as err:
                self.warning("STACK: NOT SUPPORTED: %s" % err)
            else:
                stacks[block] = block.stack

        for block in self.blocks:
            if not block.in_loop:
                continue
            block.convert_move_instr()
        for block in self.blocks:
            block.convert_merge_duplicate_load()
        for block in self.blocks:
            block.convert_step3()

        if self.config.remove_dead_code:
            self.remove_dead_blocks()

        if dump_optimized and dump_blocks:
            self.dump(prefix="optimized blocks")
        self.recompute_labels()

        self.instructions = []
        for block in self.blocks:
            self.instructions.extend(block.instructions)
        self.blocks = None

        self.renumber_registers()
        if dump_optimized and dump_bytecode:
            self.dump(prefix="optimized bytecode")

    def check_inefficient_code(self, max_warning=5):
        nwarning = 0
        for instr in self.instructions:
            if instr.name not in ('PUSH_REG', 'POP_REG'):
                continue
            if nwarning >= max_warning:
                print("!!! SUBOPTIMAL CODE !!! (...)")
                return
            print("!!! SUBOPTIMAL CODE !!! %s" % instr)
            nwarning += 1

    def dump(self, prefix=None, with_name=False):
        name = "%s:%s" % (self.code.co_filename, self.code.co_firstlineno)
        if prefix:
            name = "%s:%s" % (prefix, name)
        if self.blocks is not None:
            ninstr = len(tuple(self._iter_instr()))
            title = "============= %s: %s blocks (%s instructions) ========" % (name, len(self.blocks), ninstr)
            print(title)
            pos = 0
            for index, block in enumerate(self.blocks):
                print(block)
                for link in block.in_links:
                    print("<-- %s --- %s" % (link.link_type, link.block))
                pos = block.dump(pos=pos)
                for link in block.out_links:
                    print("--- %s --> %s" % (link.link_type, link.block))
                if index != len(self.blocks)-1:
                    print()
            print(title)
        else:
            print("%s (%s instructions):" % (name, len(self.instructions)))
            pos = 0
            for instr in self.instructions:
                size = instr.get_size()
                print("%3i: %s" % (pos, instr.format(pos+size, with_name=with_name)))
                pos += size

    def compile(self):
        # FIXME: recompute the stack size, need to recompute the index of
        # FIXME: cell and free variables (Register and Local already handles
        # FIXME: the renumbering correctly)
        bytecode = b''.join(
            instr.compile()
            for instr in self.instructions
            if instr.name is not LABEL)
        return patch_code_obj(self.code, bytecode=bytecode)


def optimize_code(code, config=None, **options):
    converter = Converter(code, config)
    converter.convert(**options)
    return converter.compile()

def optimize_func(func, config=None, **options):
    if isinstance(func, types.MethodType):
        return optimize_func(func.__func__, config)
    new_code = optimize_code(func.__code__, config, **options)
    func.__code__ = new_code

