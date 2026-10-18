"""Builds tests/proxy_test.c for Windows and runs it: adapters/azimuth_proxy.c (F3) with real
sockets on this PC's LAN address (the first argument after the zig).
Usage: python tests/proxy_test.py <zig> <LAN IPv4>
On Linux, FH2_RUN is the command that runs the Windows test program (its words, then the .exe),
e.g. a Proton wine: FH2_RUN="/path/to/Proton/files/bin/wine"."""
import os, pathlib, shlex, subprocess, sys, tempfile

hook = pathlib.Path(__file__).resolve().parent.parent
zig = sys.argv[1]
with tempfile.TemporaryDirectory() as tmp:
    exe = pathlib.Path(tmp) / "proxy_test.exe"
    subprocess.run([zig, "cc", "-target", "x86_64-windows-gnu", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-cast-function-type", "-I", str(hook),
                    str(hook / "tests" / "proxy_test.c"), "-lws2_32", "-o", str(exe)], check=True)
    run = shlex.split(os.environ.get("FH2_RUN", ""))
    sys.exit(subprocess.run(run + [str(exe), sys.argv[2]]).returncode)
