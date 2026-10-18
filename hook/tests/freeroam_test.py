"""Builds tests/freeroam_test.c for Windows and runs it: adapters/azimuth_freeroam.c's world
address, its WinRT stand-ins (on fakes of Azimuth's objects) and its Winsock redirection.
Usage: python tests/freeroam_test.py <zig>
On Linux, FH2_RUN is the command that runs the Windows test program (its words, then the .exe),
e.g. a Proton wine: FH2_RUN="/path/to/Proton/files/bin/wine"."""
import os, pathlib, shlex, subprocess, sys, tempfile

hook = pathlib.Path(__file__).resolve().parent.parent
zig = sys.argv[1]
with tempfile.TemporaryDirectory() as tmp:
    exe = pathlib.Path(tmp) / "freeroam_test.exe"
    subprocess.run([zig, "cc", "-target", "x86_64-windows-gnu", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-cast-function-type", "-I", str(hook),
                    str(hook / "tests" / "freeroam_test.c"), "-lws2_32", "-o", str(exe)], check=True)
    run = shlex.split(os.environ.get("FH2_RUN", ""))
    sys.exit(subprocess.run(run + [str(exe)]).returncode)
