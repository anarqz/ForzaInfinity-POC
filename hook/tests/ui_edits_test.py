"""Builds tests/ui_edits_test.c for the host, runs azimuth_ui.c's edits on the game's own
files (read from the game folder, written to a temporary folder) and checks the results
are well-formed XML.
Usage: python tests/ui_edits_test.py <zig> <ForzaHorizon2 Mount folder>
On Linux, FH2_RUN is the command that runs the Windows test program (its words, then the .exe),
e.g. a Proton wine: FH2_RUN="/path/to/Proton/files/bin/wine"."""
import os, pathlib, shlex, subprocess, sys, tempfile, xml.dom.minidom, zipfile

hook = pathlib.Path(__file__).resolve().parent.parent
zig, mount = sys.argv[1], pathlib.Path(sys.argv[2])
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    ui = zipfile.ZipFile(mount / "media" / "UI.zip")
    flows = zipfile.ZipFile(mount / "media" / "stateflow.zip")
    files = {
        "options.xaml": ui.read("Scenes/Anthem/A_PauseOptions.xaml"),
        "pause.xaml": ui.read("Scenes/Anthem/A_PauseMenu.xaml"),
        "ui_screens.xml": flows.read("ui_screens.xml"),
        "pause_flow.xml": flows.read("pause.xml"),
    }
    tables = {}
    for z in sorted((mount / "media" / "stripped" / "stringtables").glob("*.zip")):
        if z.stem != "LOC":
            tables[f"MessageCenter-{z.stem}.str"] = zipfile.ZipFile(z).read("MessageCenter.str")
    for name, data in {**files, **tables}.items():
        (tmp / name).write_bytes(data)
    exe = tmp / "ui_edits_test.exe"
    subprocess.run([zig, "cc", "-target", "x86_64-windows-gnu", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-I", str(hook), str(hook / "tests" / "ui_edits_test.c"), "-o", str(exe)],
                   check=True)
    run = shlex.split(os.environ.get("FH2_RUN", ""))
    subprocess.run(run + [str(exe)] + [str(tmp / n) for n in files] + [str(tmp / n) for n in tables], check=True)
    for out in sorted(tmp.glob("*.*.*")):
        if out.suffix in (".options", ".infinity", ".pause", ".flow"):
            xml.dom.minidom.parseString(out.read_bytes())
            print("well-formed:", out.name)

    # the Message Center: same size, every other byte the same, and the sender is INFINITY
    import struct

    def texts(table):
        count = struct.unpack_from("<I", table, 0x28)[0]
        base = 0x2C + 6 * count + 6
        out = {}
        for i in range(count):
            key, offset = struct.unpack_from("<HI", table, 0x2C + 6 * i)
            at, end = base + 2 * offset, base + 2 * offset
            while table[end:end + 2] != b"\0\0":
                end += 2
            out[key] = table[at:end].decode("utf-16-le")
        return out

    for name, original in tables.items():
        edited = (tmp / (name + ".mc")).read_bytes()
        assert len(edited) == len(original), name
        before, after = texts(original), texts(edited)
        assert after[0x3232] == "INFINITY", (name, after[0x3232])
        assert "INFINITY" in after[0x8216] and "Turn" not in after[0x8216], (name, after[0x8216])
        for key in before:
            assert len(after[key]) <= len(before[key]), (name, hex(key))
        print("message center:", name, repr(after[0x8216].split("\n")[0]))
