"""
The control panel's update center: mm_gui only installs newer versions, only
when their package matches the release's SHA256SUMS.txt, and leaves the
user's own tables and config alone.

A copy of mm_gui is put in a scratch install folder (bin/ and share/mm_manager/,
as a release package unpacks), and "GitHub" is a local HTTP server.  The
packages it serves are tiny: a bin/mm_gui that only has to run, and a file
to see whether the install happened.

mm_gui is found as in test_gui_accounts.py.  The install tests need a computer
there are release packages for (Linux x86-64 here; Windows and macOS packages
are zips and installers this test doesn't make).

Part of mmumblephone, a fork of mm_manager by Howard M. Harte
(www.github.com/hharte/mm_manager).

Copyright (c) 2026, mmumblephone contributors
"""

import functools
import hashlib
import http.server
import io
import os
import platform
import shutil
import sys
import tarfile
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_gui_accounts import Panel, find_gui  # noqa: E402

LINUX_X64 = sys.platform.startswith("linux") and platform.machine() in ("x86_64", "AMD64")


class Releases:
    """A stand-in for GitHub's release downloads, on 127.0.0.1."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="mm_gui_releases_")
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=self.dir)
        handler.log_message = lambda *a: None
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def publish(self, version, files, damage=False):
        """A release with a Linux package holding files ({path: bytes}) and its SHA256SUMS.txt."""
        top = f"mmumblephone-{version}-Linux"
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            for path, data in files.items():
                info = tarfile.TarInfo(f"{top}/{path}")
                info.size = len(data)
                info.mode = 0o755 if path.startswith("bin/") else 0o644
                tar.addfile(info, io.BytesIO(data))
        package = buf.getvalue()
        where = os.path.join(self.dir, "download", f"v{version}")
        os.makedirs(where)
        with open(os.path.join(where, f"{top}.tar.gz"), "wb") as f:
            f.write(package)
        digest = hashlib.sha256(package + (b"x" if damage else b"")).hexdigest()
        with open(os.path.join(where, "SHA256SUMS.txt"), "w") as f:
            f.write(f"{digest}  {top}.tar.gz\n")

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        shutil.rmtree(self.dir, ignore_errors=True)


@unittest.skipUnless(find_gui(), "mm_gui isn't built")
class UpdateTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="mm_gui_update_")
        self.prefix = os.path.join(self.tmp, "mmumblephone")
        self.work = os.path.join(self.tmp, "work")
        os.makedirs(os.path.join(self.prefix, "bin"))
        os.makedirs(os.path.join(self.prefix, "share", "mm_manager", "config"))
        os.makedirs(os.path.join(self.work, "tables", "default"))
        self.gui = os.path.join(self.prefix, "bin", os.path.basename(find_gui()))
        shutil.copy2(find_gui(), self.gui)
        with open(os.path.join(self.prefix, "share", "mm_manager", "config", "mine.csv"), "w") as f:
            f.write("my own\n")
        self.releases = Releases()
        env = dict(os.environ, MM_UPDATE_API=f"{self.releases.url}/releases.json",
                   MM_UPDATE_DOWNLOAD=f"{self.releases.url}/download")
        self.panel = Panel(self.work, gui=self.gui, env=env)
        self.current = self.call("GET", "/api/update")[1]["current"]

    def tearDown(self):
        self.panel.stop()
        self.releases.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def call(self, method, path):
        return self.panel.request(method, path)

    def newer(self, bump=1):
        major, minor, patch = (self.current.split("-")[0].split(".") + ["0", "0"])[:3]
        return f"{major}.{minor}.{int(patch) + bump}"

    def wait_for_update(self):
        for _ in range(300):
            status, r = self.call("GET", "/api/output?job=update&from=0")
            if not r["running"]:
                return r
            time.sleep(0.1)
        self.fail("the update didn't finish")

    def test_only_newer_versions(self):
        for version in ("0.0.1", self.current):
            status, r = self.call("POST", f"/api/update/start?version={version}")
            self.assertEqual(status, 409, version)
            self.assertIn("newer", r["error"])
        self.assertEqual(self.call("POST", "/api/update/start?version=not-a-version")[0], 400)
        self.assertEqual(self.call("POST", "/api/update/start?version=1.2.3;rm")[0], 400)
        # No restart without an installed update.
        self.assertEqual(self.call("POST", "/api/update/restart")[0], 409)
        # The update job can't be used to run anything else.
        self.assertEqual(self.call("POST", "/api/run?job=update")[0], 400)

    def test_a_build_isnt_updated(self):
        self.panel.stop()
        self.panel = Panel(self.work)   # mm_gui as built, not in an install's bin/
        status, r = self.call("GET", "/api/update")
        self.assertFalse(r["updatable"])
        self.assertIn("release package", r["why"])
        self.assertEqual(self.call("POST", f"/api/update/start?version={self.newer()}")[0], 409)

    @unittest.skipUnless(LINUX_X64, "release packages are made for Linux x86-64, Windows and macOS")
    def test_install(self):
        status, r = self.call("GET", "/api/update")
        self.assertTrue(r["updatable"], r)
        self.assertEqual(r["asset"], "mmumblephone-{version}-Linux.tar.gz")
        files = {
            "bin/mm_gui": b"#!/bin/sh\necho new mm_gui\n",
            "share/mm_manager/config/mine.csv": b"the release's copy\n",
            "share/mm_manager/config/new.csv": b"new in this release\n",
            "README.md": b"new readme\n",
        }

        def installed(path):
            with open(os.path.join(self.prefix, path), "rb") as f:
                return f.read()

        # A package that doesn't match its checksum changes nothing.
        bad = self.newer(1)
        self.releases.publish(bad, files, damage=True)
        self.assertEqual(self.call("POST", f"/api/update/start?version={bad}")[0], 200)
        r = self.wait_for_update()
        self.assertNotEqual(r["exitCode"], 0)
        self.assertIn("doesn't match its checksum", r["text"])
        self.assertFalse(os.path.exists(os.path.join(self.prefix, "README.md")))
        self.assertEqual(self.call("GET", "/api/update")[1]["ready"], "")

        # A good one is installed, keeping the user's own config.
        good = self.newer(2)
        self.releases.publish(good, files)
        self.assertEqual(self.call("POST", f"/api/update/start?version={good}")[0], 200)
        r = self.wait_for_update()
        self.assertEqual(r["exitCode"], 0, r["text"])
        self.assertEqual(installed("README.md"), b"new readme\n")
        self.assertEqual(installed("bin/mm_gui"), files["bin/mm_gui"])
        self.assertTrue(os.access(os.path.join(self.prefix, "bin", "mm_gui"), os.X_OK))
        self.assertEqual(installed("share/mm_manager/config/mine.csv"), b"my own\n")
        self.assertEqual(installed("share/mm_manager/config/new.csv"), b"new in this release\n")
        self.assertEqual(self.call("GET", "/api/update")[1]["ready"], "restart")
        self.assertFalse([n for n in os.listdir(os.path.join(self.prefix, "bin")) if n.endswith((".old", ".new"))])


if __name__ == "__main__":
    unittest.main()
