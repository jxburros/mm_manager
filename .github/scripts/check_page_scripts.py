"""
Syntax-check the inline <script> blocks of the browser pages with node --check.

The pages have no build step, so a typo in their JavaScript would otherwise
only show up when someone opens them.  Usage: check_page_scripts.py <page.html>...
"""

import os
import re
import subprocess
import sys
import tempfile

failed = False
for page in sys.argv[1:]:
    with open(page, encoding="utf-8") as f:
        html = f.read()
    blocks = re.findall(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", html, re.S | re.I)
    for i, js in enumerate(blocks, 1):
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False, encoding="utf-8") as tmp:
            tmp.write(js)
        r = subprocess.run(["node", "--check", tmp.name], capture_output=True, text=True)
        os.unlink(tmp.name)
        name = f"{page} script {i}"
        if r.returncode:
            failed = True
            print(f"FAIL {name}\n{r.stderr}")
        else:
            print(f"ok   {name}")
    if not blocks:
        print(f"no inline scripts in {page}")
sys.exit(1 if failed else 0)
