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

print(f"[AMBE] Linking Rust backend: {archive}")

env.Append(
    CPPDEFINES=["IU2VTP_TX_AMBE_RUST=1"],
    LINKFLAGS=[str(archive)],
)
