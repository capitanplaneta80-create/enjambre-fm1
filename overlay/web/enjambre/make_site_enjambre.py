#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# ENJAMBRE: arma el sitio (GitHub Pages), a partir de web/make_site.py de Felucca (Leo Kuroshita, Hügelton Instruments).
"""Arma el sitio de ENJAMBRE:

  index.html                       la portada (landing.html)
  firmware/enjambre-VER.fwsc       el paquete (+ LICENSE, LICENSING.md, LICENSES/)
  webapp/installer/index.html      el instalador, autocontenido (fm1pkg.js, fm1ota.js, fm1backup.js y los datos dentro)
  webapp/editor/index.html         el editor de Felucca (+ fukiai.ttf)
  webapp/sim/index.html            el prototipo en el navegador (el motor en WebAssembly)

  python3 web/enjambre/make_site_enjambre.py build/felucca-VER.fwsc VER SALIDA [URL_DEL_CODIGO]

Mismas comprobaciones que Felucca: identidad FM-1_9xx y el cargador de Felucca dentro (nunca un paquete con
archivos del fabricante). El firmware oficial de M-VAVE no se publica: el usuario lo elige en el instalador.
"""
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
WEB = HERE.parent
ROOT = WEB.parent
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def strip_module(src):
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


def main(pkg, version, out, source="https://github.com/hugelton/Felucca"):
    pkg, out = Path(pkg), Path(out)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_9\d\d", product):
        raise SystemExit(f"{pkg}: identidad {product!r}: no es un paquete de Felucca (FM-1_9xx)")
    if b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: sin el cargador de Felucca; el sitio solo publica paquetes de fm1pkg_make.py")
    sha = hashlib.sha256(raw).hexdigest()
    name = f"enjambre-{re.sub(r'[^A-Za-z0-9.-]', '-', version)}.fwsc"
    lib = "\n".join(strip_module((WEB / f).read_text(encoding="utf-8")) for f in ("fm1pkg.js", "fm1ota.js", "fm1backup.js"))
    meta = json.dumps({"version": version, "product": product, "pkg": "../../firmware/" + name, "sha256": sha,
                       "source": source})
    html = (HERE / "installer.html").read_text(encoding="utf-8")
    for mark in ("/*LIB*/", "/*META*/"):
        if html.count(mark) != 1:
            raise SystemExit(f"installer.html debe tener {mark} una vez")
    html = html.replace("/*LIB*/", lib).replace("/*META*/", meta)

    inst, ed, sim, fw = (out / "webapp" / "installer", out / "webapp" / "editor", out / "webapp" / "sim",
                         out / "firmware")
    for d in (inst, ed, sim, fw):
        d.mkdir(parents=True, exist_ok=True)
    for old in fw.glob("enjambre-*.fwsc"):
        old.unlink()
    (inst / "index.html").write_text(html, encoding="utf-8")
    shutil.copy(pkg, fw / name)
    (fw / "SHA256SUMS").write_text(f"{sha}  {name}\n")
    lic = ROOT / "LICENSES"
    (fw / "LICENSES").mkdir(exist_ok=True)
    names = sorted(f.name for f in lic.glob("*.txt"))
    for n in names:
        shutil.copy(lic / n, fw / "LICENSES" / n)
    (fw / "LICENSES" / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><title>Licencias</title><h1>Licencias</h1><ul>'
        + "".join(f'<li><a href="{n}">{n}</a></li>' for n in names)
        + '</ul><p><a href="../LICENSING.md">LICENSING.md</a> · <a href="../LICENSE">LICENSE (GPL-3.0)</a></p>\n',
        encoding="utf-8")
    for doc in ("LICENSE", "LICENSING.md"):
        shutil.copy(ROOT / doc, fw / doc)
    shutil.copy(WEB / "editor.html", ed / "index.html")
    for f in ("fukiai.ttf", "FUKIAI-LICENSE.txt", "fm1backup.js"):
        if (WEB / f).exists():
            shutil.copy(WEB / f, ed / f)
    shutil.copy(HERE / "sim.html", sim / "index.html")
    land = (HERE / "landing.html").read_text(encoding="utf-8")
    land = land.replace("__VERSION__", version).replace("__PKG__", name).replace("__SHA__", sha).replace("__SOURCE__", source)
    (out / "index.html").write_text(land, encoding="utf-8")
    (out / ".nojekyll").write_text("")
    print(f"sitio en {out}: {name} ({len(raw)} B, {product}, sha256 {sha[:16]}…)")


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
