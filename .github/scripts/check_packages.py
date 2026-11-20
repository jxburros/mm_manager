"""
Check that CPack made every package the release needs for this platform.

.release/config.json lists the assets a release must have; a release fails if
one is missing.  This catches a renamed or missing package in CI instead.
Usage: check_packages.py <build folder> <platform tag: Linux, Darwin, win64 or win32>
"""

import json
import os
import sys

build, tag = sys.argv[1], sys.argv[2]
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
with open(os.path.join(root, "VERSION"), encoding="utf-8") as f:
    version = f.read().strip()
with open(os.path.join(root, ".release", "config.json"), encoding="utf-8") as f:
    assets = json.load(f)["apps"]["mm-manager"]["assets"]

wanted = [a.replace("{version}", version) for a in assets if f"-{tag}." in a]
if not wanted:
    sys.exit(f"no release assets for platform {tag} in .release/config.json")
missing = [name for name in wanted if not os.path.isfile(os.path.join(build, name))]
for name in wanted:
    print(("missing " if name in missing else "ok      ") + name)
sys.exit(1 if missing else 0)
