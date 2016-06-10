#!/usr/bin/python2
import sys, subprocess

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
subprocess.check_call(["objdump"] + new_args)
