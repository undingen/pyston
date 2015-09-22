import os
import sys
import subprocess
import shutil

ENV_NAME = "lxml_test_env_" + os.path.basename(sys.executable)

if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../lib/virtualenv/virtualenv.py"

    try:
        args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, ENV_NAME]
        print "Running", args
        subprocess.check_call(args)
    except:
        print "Error occurred; trying to remove partially-created directory"
        ei = sys.exc_info()
        try:
            subprocess.check_call(["rm", "-rf", ENV_NAME])
        except Exception as e:
            print e
        raise ei[0], ei[1], ei[2]

SRC_DIR = ENV_NAME
PYTHON_EXE = os.path.abspath(ENV_NAME + "/bin/python")
CYTHON_DIR = os.path.abspath(os.path.join(SRC_DIR, "Cython-0.22"))

print "\n>>>"
print ">>> Setting up Cython..."
if not os.path.exists(CYTHON_DIR):
    print ">>>"

    url = "http://cython.org/release/Cython-0.22.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "Cython-0.22.tar.gz"], cwd=SRC_DIR)

    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "Cython_0001-Pyston-change-we-don-t-support-custom-traceback-entr.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=CYTHON_DIR)
    print "Applied Cython patch"


    try:
        subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=CYTHON_DIR)
        subprocess.check_call([PYTHON_EXE, "-c", "import Cython"], cwd=CYTHON_DIR)
    except:
        subprocess.check_call(["rm", "-rf", CYTHON_DIR])
else:
    print ">>> Cython already installed."
    print ">>>"


LXML_DIR = os.path.abspath(os.path.join(SRC_DIR, "lxml"))
if not os.path.exists(LXML_DIR):
    subprocess.check_call(["git", "clone", "https://github.com/lxml/lxml.git"], cwd=SRC_DIR)
    subprocess.check_call(["git", "checkout", "lxml-3.0.1"], cwd=LXML_DIR)

    print "\n>>>"
    print ">>> Patching lxml..."
    print ">>>"
    LXML_PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "lxml_patch.patch"))
    try:
        cmd = ["patch", "-p1", "--forward", "-i", LXML_PATCH_FILE, "-d", LXML_DIR]
        subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as e:
        print e.output
        if "Reversed (or previously applied) patch detected!  Skipping patch" not in e.output:
            raise e
else:
    print ">>> lxml already downloaded."
    print ">>>"

print "\n>>>"
print ">>> Setting up lxml..."
print ">>>"
subprocess.check_call([PYTHON_EXE, "setup.py", "build_ext", "-i", "--with-cython"], cwd=LXML_DIR)
subprocess.check_call([PYTHON_EXE, "test.py", "-vv"], cwd=LXML_DIR)

