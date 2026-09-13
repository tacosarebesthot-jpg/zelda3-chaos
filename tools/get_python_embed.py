#!/usr/bin/env python3
"""Fetch the embeddable Python the game uses to build randomizer rule folders on
demand (python_embed\\ next to zelda3.exe): the official python.org embeddable
zip plus PyYAML (the only third-party module randomizer_ref/dump_logic.py and
the reference randomizer need). Run once after cloning; make_dist.py ships the
folder. Windows x64 only, like the game.

    python tools/get_python_embed.py
"""
import io, os, shutil, sys, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEST = os.path.join(ROOT, "python_embed")
PY = "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip"
YAML = "https://pypi.org/simple/pyyaml/"   # the pure-python wheel is picked below


def fetch(url):
    print("fetching", url, flush=True)
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def main():
    if os.path.exists(DEST):
        shutil.rmtree(DEST)
    os.makedirs(DEST)
    zipfile.ZipFile(io.BytesIO(fetch(PY))).extractall(DEST)
    # site-packages on the path, so the yaml package below is importable
    with open(os.path.join(DEST, "python312._pth"), "w", newline="\n") as f:
        f.write("python312.zip\n.\nLib\\site-packages\nimport site\n")
    site = os.path.join(DEST, "Lib", "site-packages")
    os.makedirs(site)
    # PyYAML: take the py3-none-any wheel from the simple index and unzip it
    index = fetch(YAML).decode("utf-8", "replace")
    import re
    wheels = re.findall(r'href="([^"]+PyYAML-[^"]+py3-none-any\.whl[^"]*)"', index) or \
             re.findall(r'href="([^"]+pyyaml-[^"]+py3-none-any\.whl[^"]*)"', index, re.I)
    if wheels:
        zipfile.ZipFile(io.BytesIO(fetch(wheels[-1]))).extractall(site)
    else:
        # fall back to the yaml package of the Python running this script
        try:
            import yaml
            shutil.copytree(os.path.dirname(yaml.__file__), os.path.join(site, "yaml"),
                            ignore=shutil.ignore_patterns("*.pyd", "*.so", "__pycache__"))
        except ImportError:
            print("no PyYAML wheel found and none installed here: pip install pyyaml, then rerun")
            sys.exit(1)
    print("python_embed ready:", DEST)


if __name__ == "__main__":
    main()
