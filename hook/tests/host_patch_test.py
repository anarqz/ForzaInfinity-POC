"""Builds tests/host_patch_test.c for Windows and runs it on real azimuth.exe files: the build is
recognised, azimuth_offsets.c's entry matches the image (hashes and vtable slots), and the livery
and save patches apply once and then report themselves as applied.
Usage: python tests/host_patch_test.py <zig> <azimuth.exe> <build name or -> [...]
  e.g. ... azimuth-0.9.7.exe 0.9.7 azimuth-0.9.9.exe 0.9.9 azimuth-0.9.10.exe 0.9.10
On Linux, FH2_RUN is the command that runs the Windows test program (its words, then the .exe),
e.g. a Proton wine: FH2_RUN="/path/to/Proton/files/bin/wine"."""
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile

hook = pathlib.Path(__file__).resolve().parent.parent
zig = sys.argv[1]
with tempfile.TemporaryDirectory() as tmp:
    exe = pathlib.Path(tmp) / "host_patch_test.exe"
    subprocess.run([zig, "cc", "-target", "x86_64-windows-gnu", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-I", str(hook), str(hook / "tests" / "host_patch_test.c"),
                    "-o", str(exe)], check=True)
    run = shlex.split(os.environ.get("FH2_RUN", ""))
    sys.exit(subprocess.run(run + [str(exe)] + sys.argv[2:]).returncode)
