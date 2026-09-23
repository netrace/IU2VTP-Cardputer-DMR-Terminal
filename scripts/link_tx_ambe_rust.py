Import("env")

import os
import shutil
import subprocess
from pathlib import Path

project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
manifest = project_dir / "codec" / "blip25_ffi" / "Cargo.toml"
default_archive = (
    project_dir
    / "codec"
    / "blip25_ffi"
    / "target"
    / "xtensa-esp32s3-espidf"
    / "release"
    / "libiu2vtp_blip25_ffi.a"
)

explicit = os.environ.get("IU2VTP_AMBE_RUST_LIB", "").strip()

if explicit:
    archive = Path(explicit).expanduser().resolve()
else:
    archive = default_archive

    if not archive.is_file():
        cargo = shutil.which("cargo")
        if not cargo:
            print("[AMBE] cargo not found; cannot build embedded AMBE backend")
            print("[AMBE] Install the esp-rs Rust toolchain (espup), then rebuild.")
            env.Exit(1)

        print("[AMBE] Rust backend archive missing; building it automatically...")
        cmd = [
            cargo,
            "build",
            "-Zbuild-std=std,panic_abort",
            "--release",
            "--target",
            "xtensa-esp32s3-espidf",
            "--manifest-path",
            str(manifest),
        ]

        try:
            subprocess.run(cmd, cwd=project_dir, check=True)
        except subprocess.CalledProcessError as exc:
            print(f"[AMBE] Rust backend build failed with exit code {exc.returncode}")
            print("[AMBE] Make sure the esp-rs Xtensa toolchain is installed and active.")
            env.Exit(exc.returncode)

if not archive.is_file():
    print(f"[AMBE] Rust backend archive not found: {archive}")
    env.Exit(1)

if not archive.name.startswith("lib") or archive.suffix != ".a":
    print(f"[AMBE] Expected lib*.a archive, got: {archive.name}")
    env.Exit(1)

lib_name = archive.name[3:-2]

print(f"[AMBE] Linking Rust backend: {archive}")
print(f"[AMBE] LIBPATH={archive.parent} LIBS={lib_name}")

env.Append(
    CPPDEFINES=["IU2VTP_TX_AMBE_RUST=1"],
    LIBPATH=[str(archive.parent)],
    LIBS=[lib_name],
)
