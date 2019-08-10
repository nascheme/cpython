import dis
import functools
import registervm
import sys
import time
import tokenize
from optparse import OptionParser

BENCH = False

def exec_code(code):
    # specify a namespace, or import will not work
    namespace = {}
    exec(code, namespace, namespace)

def benchmark(what, code):
    print("Benchmark %s: prepare" % what)
    for prepare in range(3):
        exec_code(code)
    best = None
    runs = 5
    loops = 100
    for run in range(runs):
        print("Benchmark %s: run %s/%s" % (what, 1+run, runs))
        t0 = time.perf_counter()
        for loop in range(loops):
            exec_code(code)
        dt = time.perf_counter() - t0
        if best is not None:
            best = min(best, dt)
        else:
            best = dt
    print("Best time (%s runs, %s loops) of %s: %.1f ms per loop"
          % (runs, loops, what, best * 1e3 / loops))

def parse_options():
    parser = OptionParser(usage="%prog [options] -f script.py|COMMAND [COMMAND2 COMMAND3 ...]")
    parser.add_option("-2", "--twice",
        help="optimize twice",
        action="store_true", default=False)
    parser.add_option("-f", "--file",
        type="str",
        action="store", default=None)
    parser.add_option("-a", "--assembler",
        help="show assembler",
        action="store_true", default=False)
    parser.add_option("-v", "--verbose",
        help="verbose mode",
        action="store_true", default=False)
    parser.add_option("-Z", "--dump-optimized",
        help="Don't dump optimized bytecode/blocks",
        action="store_false", default=True)
    parser.add_option("-O", "--dump-original",
        help="Dump original bytecode/blocks",
        action="store_true", default=False)
    parser.add_option("-C", "--dump-bytecode",
        help="Dump bytecode",
        action="store_true", default=False)
    parser.add_option("-B", "--dump-blocks",
        help="Dump blocks",
        action="store_true", default=False)
    parser.add_option("-q", "--quiet",
        help="Quiet mode",
        action="store_true", default=False)
    parser.add_option("-d", "--debug",
        help="Disable debug mode",
        action="store_false", default=True)
    parser.add_option("-b", "--benchmark",
        help="Run benchmark",
        action="store_true", default=False)
    parser.add_option("-m", "--hide-main",
        help="Hide code of the __main__ module",
        action="store_true", default=False)
    options, args = parser.parse_args()
    if options.quiet:
        options.debug = False
    command = None
    if len(args) > 1:
        command = '\n'.join(args)
        if options.file:
            parser.print_help()
            sys.exit(1)
    elif not options.file:
        parser.print_help()
        sys.exit(1)
    return options, command

def main():
    options, code_str = parse_options()
    filename = options.file
    if filename:
        with tokenize.open(filename) as f:
            code_str = f.read()
    else:
        filename = "<stdin>"

    if options.benchmark:
        stack_code = compile(code_str, filename, "exec")
    else:
        stack_code = None

    config = registervm.Config()
    config.quiet = options.quiet
    config.debug = options.debug
    config.enable_unsafe_optimizations()
    config.enable_buggy_optimizations()
    def create_code_optimizer(config, options):
        if options.twice:
            nloops = 2
        else:
            nloops = 1
        def code_optimizer(code):
            for loop in range(nloops):
                if not options.hide_main or code.co_name != '<module>':
                    kw = {
                        'dump_original': options.dump_original,
                        'dump_optimized': options.dump_optimized,
                        'dump_bytecode': options.dump_bytecode,
                        'dump_blocks': options.dump_blocks,
                    }
                else:
                    kw = {}
                code = registervm.optimize_code(
                    code, config,
                    **kw)
            return code
        return code_optimizer


    optimizer = create_code_optimizer(config, options)
    if options.verbose:
        print("Set code optimizer")
    sys.setcodeoptimizer(optimizer)

    if options.verbose:
        print("Compile")
    code = compile(code_str, filename, "exec")

    if options.verbose:
        print("Execute")
    exec_code(code)

    if options.verbose:
        print()

    if options.assembler:
        if stack_code is not None:
            print("Assembler (stack)")
            dis.dis(stack_code)

        print("Assembler (register)")
        dis.dis(code)

    if options.benchmark:
        benchmark("stack", stack_code)
        benchmark("register", code)

main()
