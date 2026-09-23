Import("env")

import os
from pathlib import Path

lib = os.environ.get("IU2VTP_AMBE_RUST_LIB", "").strip()

if not lib:
    print("[AMBE] Rust backend disabled (IU2VTP_AMBE_RUST_LIB not set)")
    Return()

archive = Path(lib).expanduser().resolve()
if not archive.is_file():
    print(f"[AMBE] Rust backend archive not found: {archive}")
    env.Exit(1)

if not archive.name.startswith("lib") or archive.suffix != ".a":
    print(f"[AMBE] Expected lib*.a archive, got: {archive.name}")
    env.Exit(1)

lib_name = archive.name[3:-2]

print(f"[AMBE] Linking Rust backend: {archive}")
print(f"[AMBE] LIBPATH={archive.parent} LIBS={lib_name}")

# Important: use SCons' LIBPATH/LIBS rather than putting the archive in
# LINKFLAGS. PlatformIO emits LIBS after object files, so the archive is
# searched after tx_ambe_encoder.cpp has introduced the C-ABI references.
env.Append(
    CPPDEFINES=["IU2VTP_TX_AMBE_RUST=1"],
    LIBPATH=[str(archive.parent)],
    LIBS=[lib_name],
)
