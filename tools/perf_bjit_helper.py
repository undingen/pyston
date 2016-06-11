#!/usr/bin/python2
import sys, subprocess

from annotate import getCommentForInst

is_bjit = False
for arg in sys.argv:
    is_bjit = is_bjit or "/tmp/perf-" in arg
    if "--start-address=" in arg:
        start_addr = arg[len("--start-address="):]

if is_bjit:
    assert start_addr
    new_args = ["--adjust-vma=%s" % start_addr, "--start-address=%s" % start_addr,
                "-D", "-b", "binary", "-mi386:x86-64", "--no-show-raw", "bjit_dump/%s" % start_addr]
else:
    new_args = sys.argv[1:]

if not is_bjit:
    subprocess.check_call(["objdump"] + new_args)
else:
    p = subprocess.Popen(["objdump"] + new_args, stdout=subprocess.PIPE, stderr=open("/dev/null", "w"))
    objdump = p.communicate()[0]
    assert p.wait() == 0

    for l in objdump.split('\n')[7:]:
        comment = getCommentForInst(l) or ""
        print l.ljust(70), comment
