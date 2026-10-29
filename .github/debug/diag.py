import os, subprocess
path = os.environ.get("PATH", "")
prefix = os.environ.get("CONDA_PREFIX", "")
entries = path.split(os.pathsep)
norm = lambda p: os.path.normcase(os.path.normpath(p)) if p else ""
libbin = norm(os.path.join(prefix, "Library", "bin"))
print(f"  PATH: {len(path)} characters, {len(entries)} entries; CONDA_PREFIX={prefix}")
print("  Library\\bin on PATH:", any(norm(e) == libbin for e in entries))
for e in entries[:9]:
    print("    " + e)
moc = os.path.join(prefix, "Library", "lib", "qt6", "moc.exe")
r = subprocess.run([moc, "-v"], capture_output=True, text=True)
print(f"  moc: exit {r.returncode & 0xffffffff:#x} {r.stdout.strip()} {r.stderr.strip()}")
