Import("env")

import os
import shutil
import subprocess
import platform
from pathlib import Path

project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
home = Path.home()
cargo_home = Path(os.environ.get("CARGO_HOME", home / ".cargo")).expanduser()
cargo_bin = cargo_home / "bin"

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


def run(cmd, *, extra_env=None):
    e = os.environ.copy()
    e["PATH"] = str(cargo_bin) + os.pathsep + e.get("PATH", "")
    if extra_env:
        e.update(extra_env)
    print("[AMBE] $ " + " ".join(str(x) for x in cmd))
    subprocess.run([str(x) for x in cmd], cwd=project_dir, env=e, check=True)


def ensure_rust():
    cargo = shutil.which("cargo")
    if not cargo:
        local_cargo = cargo_bin / "cargo"
        if local_cargo.is_file():
            cargo = str(local_cargo)

    if cargo:
        return cargo

    curl = shutil.which("curl")
    sh = shutil.which("sh")
    if not curl or not sh:
        print("[AMBE] Rust bootstrap needs curl + sh")
        env.Exit(1)

    print("[AMBE] cargo not found; installing Rust automatically (rustup)...")
    try:
        subprocess.run(
            [
                curl,
                "--proto", "=https",
                "--tlsv1.2",
                "-sSf",
                "https://sh.rustup.rs",
            ],
            cwd=project_dir,
            check=True,
            stdout=subprocess.PIPE,
        )
        # Fetch again, this time piping the official installer to sh.
        p1 = subprocess.Popen(
            [
                curl,
                "--proto", "=https",
                "--tlsv1.2",
                "-sSf",
                "https://sh.rustup.rs",
            ],
            cwd=project_dir,
            stdout=subprocess.PIPE,
        )
        p2 = subprocess.run(
            [sh, "-s", "--", "-y", "--profile", "minimal"],
            cwd=project_dir,
            stdin=p1.stdout,
            check=True,
        )
        if p1.stdout:
            p1.stdout.close()
        rc = p1.wait()
        if rc != 0:
            raise subprocess.CalledProcessError(rc, "rustup download")
    except subprocess.CalledProcessError as exc:
        print(f"[AMBE] automatic Rust install failed ({exc.returncode})")
        env.Exit(exc.returncode)

    cargo = cargo_bin / "cargo"
    if not cargo.is_file():
        print(f"[AMBE] cargo still missing after rustup: {cargo}")
        env.Exit(1)

    return str(cargo)


def ensure_espup(cargo):
    espup = shutil.which("espup")
    if not espup:
        local_espup = cargo_bin / "espup"
        if local_espup.is_file():
            espup = str(local_espup)

    if not espup:
        system = platform.system().lower()
        machine = platform.machine().lower()

        # Prefer official prebuilt espup binaries. Compiling espup itself with
        # cargo is unnecessary and can fail because of host-Rust dependency
        # compatibility before we even reach the ESP toolchain.
        asset = None
        if system == "darwin":
            if machine in ("arm64", "aarch64"):
                asset = "espup-aarch64-apple-darwin"
            elif machine in ("x86_64", "amd64"):
                asset = "espup-x86_64-apple-darwin"
        elif system == "linux":
            if machine in ("arm64", "aarch64"):
                asset = "espup-aarch64-unknown-linux-gnu"
            elif machine in ("x86_64", "amd64"):
                asset = "espup-x86_64-unknown-linux-gnu"

        if not asset:
            print(f"[AMBE] no prebuilt espup mapping for {system}/{machine}")
            env.Exit(1)

        version = "v0.17.1"
        url = f"https://github.com/esp-rs/espup/releases/download/{version}/{asset}"
        local_espup = cargo_bin / "espup"
        cargo_bin.mkdir(parents=True, exist_ok=True)

        print(f"[AMBE] espup not found; downloading official prebuilt {version}...")
        print(f"[AMBE] {url}")

        curl = shutil.which("curl")
        if not curl:
            print("[AMBE] curl not found; cannot download espup")
            env.Exit(1)

        try:
            subprocess.run(
                [
                    curl,
                    "-fL",
                    "--retry", "3",
                    "--connect-timeout", "20",
                    "-o", str(local_espup),
                    url,
                ],
                cwd=project_dir,
                check=True,
            )
            local_espup.chmod(0o755)
        except subprocess.CalledProcessError as exc:
            print(f"[AMBE] espup binary download failed ({exc.returncode})")
            env.Exit(exc.returncode)

        espup = str(local_espup)

    export_file = home / "export-esp.sh"

    if not export_file.is_file():
        print("[AMBE] ESP Xtensa Rust toolchain missing; installing automatically...")
        try:
            run([espup, "install"])
        except subprocess.CalledProcessError as exc:
            print(f"[AMBE] espup toolchain install failed ({exc.returncode})")
            env.Exit(exc.returncode)

    if not export_file.is_file():
        print(f"[AMBE] expected espup environment file not found: {export_file}")
        env.Exit(1)

    return export_file

def build_archive(cargo, export_file):
    # espup writes shell exports (PATH, LIBCLANG_PATH, etc.). Use a shell only
    # for this build subprocess so PlatformIO itself does not depend on the
    # user's login-shell configuration.
    build_cmd = (
        f'. "{export_file}" && '
        f'"{cargo}" build -Zbuild-std=std,panic_abort '
        f'--release --target xtensa-esp32s3-espidf '
        f'--manifest-path "{manifest}"'
    )
    print("[AMBE] building embedded Rust AMBE backend automatically...")
    try:
        subprocess.run(
            ["bash", "-lc", build_cmd],
            cwd=project_dir,
            check=True,
        )
    except subprocess.CalledProcessError as exc:
        print(f"[AMBE] Rust backend build failed ({exc.returncode})")
        env.Exit(exc.returncode)


if explicit:
    archive = Path(explicit).expanduser().resolve()
else:
    archive = default_archive

    if not archive.is_file():
        cargo = ensure_rust()
        export_file = ensure_espup(cargo)
        build_archive(cargo, export_file)

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
