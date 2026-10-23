import os
import re
import subprocess
import tempfile

ansi_re = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")

def strip_ansi(s: str) -> str:
    return ansi_re.sub("", s)

LEAF1_CPP = """
#include <pielog.h>
#include <unistd.h>
int main()
{
    pielog_start_terminal();
    trace_scope("S leaf1");
    for (int i = 0; i < 10; i++) {
        pr("working on item {}", i);
        trace_progress(i+1, 10);
        usleep(50000);
    }
    pr("done leaf 1");
}
"""
LEAF2_CPP = """
#include <pielog.h>
#include <unistd.h>
int main()
{
    trace_scope("S leaf2");
    for (int i = 0; i < 10; i++) {
        pr("working on item {}", i);
        trace_progress(i+1, 10);
        usleep(50000);
    }
    pr("done leaf2");
}
"""
NODE1_CPP = """
#include <pielog.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    pielog_start_terminal();
    trace_scope("S node1");
    for (int i = 1; i < argc; i++) {
        pr("running item {}", i);
        trace_progress(i, argc);
        int r = system(argv[i]);
        (void)r;
    }
    pr("done node1");
}
"""
NODE2_CPP = """
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        int r = system(argv[i]);
        (void)r;
    }
}
"""

def test_cpp_mix():
    env = {k: v for k, v in os.environ.items() if k != "PIELOG"}
    with tempfile.TemporaryDirectory() as tmp:
        waitlist = []
        for name, src in [
            ("leaf1", LEAF1_CPP),
            ("leaf2", LEAF2_CPP),
            ("node1", NODE1_CPP),
            ("node2", NODE2_CPP),
        ]:
            with open(os.path.join(tmp, f"{name}.cpp"), "w") as f:
                f.write(src)
            p = subprocess.Popen([
                "c++", "-std=c++17", "-O2",
                *(["-Ipielog", "pielog/pielog.cpp"] if "pielog" in src else []),
                os.path.join(tmp, f"{name}.cpp"),
                "-o", os.path.join(tmp, name),
            ])
            waitlist.append(p)
        for p in waitlist:
            p.wait()
            assert p.returncode == 0
        p = subprocess.Popen(
            ["./node1", "./node2 ./leaf1 ./leaf2"],
            cwd=tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env
        )
        out, err = p.communicate()
        log = [
            line
            for line in strip_ansi(err).splitlines()
            if line and "S" not in line
        ]
        print(log)
        assert len([line for line in log if "running item" in line]) == 1
        assert len([line for line in log if "working on item" in line]) == 20
        assert len([line for line in log if "done" in line]) == 3
        assert len(log) == 1 + 20 + 3
        trace = [
            line
            for line in strip_ansi(err).splitlines()
            if "S" in line
        ]
        print(trace)
        assert len([line for line in trace if "node1" in line]) > 0
        assert len([line for line in trace if "leaf1" in line]) > 0
        assert len([line for line in trace if "leaf2" in line]) > 0

