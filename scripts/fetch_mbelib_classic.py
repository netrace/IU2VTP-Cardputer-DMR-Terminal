Import("env")

from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError
import ssl

ssl_context = ssl._create_unverified_context()

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
DST = PROJECT_DIR / "lib" / "mbelib_classic" / "src"
BASE = "https://raw.githubusercontent.com/szechyjs/mbelib/master/"

FILES = [
    "mbelib.c",
    "mbelib.h",
    "mbelib_const.h",
    "ambe3600x2450.c",
    "ambe3600x2450_const.h",
    "ecc.c",
    "ecc_const.h",
]

for rel in FILES:
    out = DST / rel
    if out.exists() and out.stat().st_size > 0:
        continue

    out.parent.mkdir(parents=True, exist_ok=True)
    req = Request(BASE + rel, headers={"User-Agent": "PlatformIO-Cardputer-mbelib-classic"})
    try:
        with urlopen(req, timeout=30, context=ssl_context) as r:
            out.write_bytes(r.read())
    except (HTTPError, URLError, TimeoutError) as e:
        print("ERRORE download:", BASE + rel)
        print(e)
        env.Exit(1)

print("[mbelib classic] sources ready; fast-cos embedded exactly once")
