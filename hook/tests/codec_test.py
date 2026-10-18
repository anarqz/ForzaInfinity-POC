"""Builds tests/codec_test.c for the host and runs it on real cloud save bundles: the server's
starter templates (*.bundle) and the saves kept in server databases (*.sqlite). Each file
must decode to the SHA-256 and size its commit lists, and survive a method-2 round trip.
Usage: python tests/codec_test.py <zig> <bundle or sqlite> [...]"""
import hashlib, pathlib, sqlite3, subprocess, sys, tempfile

hook = pathlib.Path(__file__).resolve().parent.parent
zig, inputs = sys.argv[1], [pathlib.Path(p) for p in sys.argv[2:]]
failed = 0
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    exe = tmp / "codec_test"
    subprocess.run([zig, "cc", "-O1", "-Wall", "-Wextra", "-Werror", str(hook / "tests" / "codec_test.c"), "-o", str(exe)],
                   check=True)
    bundles = []
    for p in inputs:
        if p.suffix == ".sqlite":
            db = sqlite3.connect(f"file:{p}?mode=ro", uri=True)
            for sid, xuid, rev, data in db.execute("SELECT id, xuid, revision, data FROM saves"):
                bundles.append((f"{p.name} save #{sid} (xuid {xuid}, revision {rev})", data))
        else:
            bundles.append((p.name, p.read_bytes()))
    for label, data in bundles:
        print("==", label)
        work = tmp / "out"
        work.mkdir(exist_ok=True)
        for old in work.iterdir():
            old.unlink()
        (tmp / "bundle").write_bytes(data)
        run = subprocess.run([str(exe), str(tmp / "bundle"), str(work)], capture_output=True, text=True)
        print(run.stdout, end="")
        if run.returncode:
            print("   codec FAILED")
            failed += 1
            continue
        commit = (work / "commit").read_text()
        for line in commit.splitlines():
            parts = line.split(" ", 2)
            if len(parts) != 3:
                continue
            digest, size, name = parts
            got = work / ("data_" + name)
            if not got.exists():
                continue
            body = got.read_bytes()
            ok = hashlib.sha256(body).hexdigest() == digest and len(body) == int(size)
            print(f"   commit {name:22} {'sha256 ok' if ok else 'MISMATCH'}")
            failed += not ok
print("FAILED" if failed else "all ok")
sys.exit(1 if failed else 0)
