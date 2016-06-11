import argparse
import commands
import os
import re
import subprocess
import sys

def get_objdump(func):
    for l in open(args.perf_map_dir + "/index.txt"):
        addr, this_func = l.split()
        if this_func == func:
            # print ' '.join(["objdump", "-b", "binary", "-m", "i386", "-D", "perf_map/" + func, "--adjust-vma=0x%s" % addr])
            p = subprocess.Popen(["objdump", "-b", "binary", "-m", "i386:x86-64", "-D", args.perf_map_dir + "/" + func, "--adjust-vma=0x%s" % addr], stdout=subprocess.PIPE)
            r = p.communicate()[0]
            assert p.wait() == 0
            return r

    raise Exception("Couldn't find function %r to objdump" % func)

_symbols = None

def demangle(sym):
    if os.path.exists("tools/demangle"):
        demangled = commands.getoutput("tools/demangle %s" % sym)
        if demangled == "Error: unable to demangle":
            demangled = sym
    else:
        demangled = commands.getoutput("c++filt %s" % sym)
    return demangled

def lookupAsSymbol(n):
    global _symbols
    if _symbols is None:
        _symbols = {}
        nm_output = commands.getoutput("nm pyston_release")
        for l in nm_output.split('\n'):
            addr = l[:16]
            if addr.isalnum():
                _symbols[int(addr, 16)] = l[19:]
    sym = _symbols.get(n, None)
    if not sym:
        return sym

    if sym.startswith('_'):
        demangled = demangle(sym)
        # perf report does not like '<'
        return demangled.replace("<", "_")
    return sym

_heap_proc = None
heapmap_args = None
def lookupAsHeapAddr(n):
    global _heap_proc
    if _heap_proc is None:
        if heapmap_args is None:
            return None

        _heap_proc = subprocess.Popen(heapmap_args, stdout=subprocess.PIPE, stdin=subprocess.PIPE)

        while True:
            l = _heap_proc.stdout.readline()
            if l.startswith("Pyston v"):
                break

    _heap_proc.stdin.write("dumpAddr(%d)\nprint '!!!!'\n" % n)
    lines = []
    while True:
        l = _heap_proc.stdout.readline()
        assert l, "heapmap subprocess exited? code: %r" % _heap_proc.poll()
        if l == '!!!!\n':
            break
        lines.append(l)
    s = ''.join(lines[1:-1])
    if "Class: NoneType" in s:
        return "None"
    if "non-gc memory" in s:
        return "(non-gc memory)"
    if "Hidden class object" in s:
        return "(hcls object)"
    if "Class: type" in s:
        m = re.search("Type name: ([^ \n]+)", s)
        return "The '%s' class" % m.group(1)

    if "Python object" in s:
        m = re.search("Class: ([^ \n]+)", s)
        return "A '%s' object" % m.group(1)

    print s

def lookupConstant(n):
    sym = lookupAsSymbol(n)
    if sym:
        return "; " + sym

    heap = lookupAsHeapAddr(n)
    if heap:
        return "; " + heap

    return ""

def getCommentForInst(inst):
    patterns = ["movabs \\$0x([0-9a-f]+),",
                "mov    \\$0x([0-9a-f]+),",
                "cmpq   \\$0x([0-9a-f]+),",
                "callq  0x([0-9a-f]+)",
                ]

    for pattern in patterns:
        m = re.search(pattern, inst)
        if m:
            n = int(m.group(1), 16)
            if n:
                return lookupConstant(n)
    return None

if __name__ == "__main__":
    # TODO: if it's not passed, maybe default to annotating the
    # first function in the profile (the one in which the plurality of
    # the time is spent)?

    parser = argparse.ArgumentParser(formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("func_name", metavar="FUNC_NAME")
    parser.add_argument("--collapse-nops", dest="collapse_nops", action="store", default=5, type=int)
    parser.add_argument("--no-collapse-nops", dest="collapse_nops", action="store_false")
    parser.add_argument("--heap-map-args", nargs='+', help="""
Command to run that will provide heap map information.
This will typically look like:
--heap-map-args ./pyston_release -i BENCHMARK
    """.strip())
    parser.add_argument("--heap-map-target", help="""
Benchmark that was run.  '--heap-map-target BENCHMARK' is
equivalent to '--heap-map-args ./pyston_release -i BENCHMARK'.
    """.strip())
    parser.add_argument("--perf-data", default="perf.data")
    parser.add_argument("--perf-map-dir", default="perf_map")
    args = parser.parse_args()

    func = args.func_name
    if args.heap_map_args:
        heapmap_args = args.heap_map_args
    elif args.heap_map_target:
        heapmap_args = ["./pyston_release", "-i", args.heap_map_target]

    objdump = get_objdump(func)

    p = subprocess.Popen(["perf", "annotate", "-i", args.perf_data, "-v", func], stdout=subprocess.PIPE, stderr=open("/dev/null", "w"))
    annotate = p.communicate()[0]
    assert p.wait() == 0

    counts = {}
    for l in annotate.split('\n'):
        if ':' not in l:
            continue
        addr, count = l.split(':')
        addr = addr.strip()
        if addr == "h->sum":
            continue

        counts[addr] = int(count)

    nops = None # (count, num, start, end)
    for l in objdump.split('\n')[7:]:
        addr = l.split(':')[0]
        count = counts.pop(addr.strip(), 0)
        extra = getCommentForInst(l) or ""

        if args.collapse_nops and l.endswith("\tnop"):
            addr = l.split()[0][:-1]
            if not nops:
                nops = (count, 1, addr, addr)
            else:
                nops = (nops[0] + count, nops[1] + 1, nops[2], addr)
        else:
            if nops:
                if int(nops[3], 16) - int(nops[2], 16) + 1 <= args.collapse_nops:
                    nop_count = nops[0]
                    for addr in xrange(int(nops[2], 16), int(nops[3], 16) + 1):
                        print str(nop_count).rjust(8), ("    %s              nop" % ("%x:      90" % addr).ljust(29)).ljust(70)
                        nop_count = 0
                else:
                    print str(nops[0]).rjust(8), ("    %s              nop*%d" % (("%s-%s" % (nops[2], nops[3])).ljust(29), nops[1])).ljust(70)
                nops = None
            print str(count).rjust(8), l.ljust(70), extra

    assert not counts, counts
