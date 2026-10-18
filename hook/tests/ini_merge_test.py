"""Builds tests/ini_merge_test.c for the host and checks azimuth_ini.c on the INIs a player can
have: none, an ExtraOptions one without [Network], one with Mode=Off, a template copied by hand
(Server=__SERVER__), a [Network] without Server or Mode, and one with a BOM.
Usage: python tests/ini_merge_test.py <zig>
On Linux, FH2_RUN is the command that runs the Windows test program (its words, then the .exe),
e.g. a Proton wine: FH2_RUN="/path/to/Proton/files/bin/wine"."""
import os, pathlib, re, shlex, subprocess, sys, tempfile

hook = pathlib.Path(__file__).resolve().parent.parent
zig = sys.argv[1]
header = (hook / "adapters" / "azimuth_ini_template.h").read_text()
SERVER = re.search(r'#define AZIMUTH_INI_SERVER "([^"]+)"', header).group(1)
TEMPLATE = "".join(eval(s) for s in re.findall(r'^    ("(?:[^"\\]|\\.)*")', header, re.M))
PLAYER = ("[Global]\r\nEnabled=1\r\n\r\n[Frontend]\r\nSkipIntro=0\r\nGetInCarFromPauseMenu=1\r\n"
          "\r\n[Livery]\r\nIntermediateMSAA=2 ; mine\r\n")
OURS = f"\r\nMode=Proxy\r\nServer={SERVER}\r\n"

CASES = {
    # name: (INI before or None, INI after (None: unchanged), whether a backup is kept)
    "none": (None, TEMPLATE, False),
    "extraoptions": (PLAYER, PLAYER + "\r\n[Network]\r\n", True),
    "no-newline": (PLAYER.rstrip("\r\n"), PLAYER + "\r\n[Network]\r\n", True),
    "mode-off": (PLAYER + "[Network]\r\nMode=Off\r\nServer=10.0.0.2:8081\r\n", None, False),
    "placeholder": (PLAYER + "[Network]\r\nMode=Proxy\r\nServer=__SERVER__\r\nAutoUpdate=0\r\n",
                    PLAYER + f"[Network]\r\nMode=Proxy\r\nServer={SERVER}\r\nAutoUpdate=0\r\n", True),
    "no-server": ("[network]\r\nmode=capture\r\n" + PLAYER,
                  f"[network]\r\nServer={SERVER}\r\nmode=capture\r\n" + PLAYER, True),
    "no-keys": (PLAYER + "[Network] ; ours\r\nAutoUpdate=0\r\n",
                PLAYER + f"[Network] ; ours\r\nMode=Proxy\r\nServer={SERVER}\r\nAutoUpdate=0\r\n", True),
    "bom": ("\ufeff" + PLAYER, "\ufeff" + PLAYER + "\r\n[Network]\r\n", True),
}

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    exe = tmp / "ini_merge_test.exe"
    subprocess.run([zig, "cc", "-target", "x86_64-windows-gnu", "-municode", "-O1", "-Wall", "-Wextra",
                    "-Werror", "-I", str(hook), "-I", str(hook / "adapters"),
                    str(hook / "tests" / "ini_merge_test.c"), "-o", str(exe)], check=True)
    run = shlex.split(os.environ.get("FH2_RUN", ""))
    for name, (before, after, backup) in CASES.items():
        folder = tmp / name
        folder.mkdir()
        ini = folder / "FH2_ExtraOptions.ini"
        if before is not None:
            ini.write_bytes(before.encode("utf-8"))
        for launch in (1, 2):  # the second start must change nothing
            done = subprocess.run(run + [str(exe), str(folder)], capture_output=True, text=True)
            assert done.returncode == 0, (name, done.returncode, done.stderr[-2000:])
            log = done.stdout.strip()
            got = ini.read_bytes().decode("utf-8")
            if after is None:
                assert got == before, (name, got)
            elif after.endswith("[Network]\r\n"):  # the player's text, then our section
                assert got.startswith(after) and OURS in got[len(after):], (name, got)
            else:
                assert got == after, (name, got)
            if launch == 1:
                print(f"{name}: {log or '(unchanged)'}")
            else:
                assert not log, (name, log)
        pre = folder / "FH2_ExtraOptions.ini.pre-infinity"
        assert pre.exists() == backup, name
        if backup:
            assert pre.read_bytes() == before.encode("utf-8"), name
        assert not (folder / "FH2_ExtraOptions.ini.infinity-new").exists(), name
    print("ok:", len(CASES), "cases")
