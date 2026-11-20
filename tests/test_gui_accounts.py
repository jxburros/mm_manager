"""
The control panel's accounts: start mm_gui in a scratch working folder and
check, over its HTTP API, what admins and users may do.

mm_gui is found from the MM_GUI environment variable (ctest sets it), or in
build/.  The test is skipped if it isn't there.

Part of mmumblephone, a fork of mm_manager by Howard M. Harte
(www.github.com/hharte/mm_manager).

Copyright (c) 2026, mmumblephone contributors
"""

import http.client
import os
import re
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTML = os.path.join(ROOT, "html", "table_editor.html")
MINE = "4085551234"
OTHER = "4085559999"


def find_gui():
    for path in (os.environ.get("MM_GUI"), os.path.join(ROOT, "build", "mm_gui"), os.path.join(ROOT, "build", "mm_gui.exe")):
        if path and os.path.isfile(path):
            return path
    return None


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Panel:
    """One running mm_gui."""

    def __init__(self, work, gui=None, env=None):
        self.port = free_port()
        self.proc = subprocess.Popen([gui or find_gui(), "-n", "-p", str(self.port), "-w", work, "-H", HTML],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        for _ in range(100):
            try:
                status, body = self.request("GET", "/", raw=True)
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("mm_gui didn't start")
        self.token = re.search(rb'name="mm-gui-token" content="([0-9a-f]+)"', body).group(1).decode()

    def request(self, method, path, body=None, session=None, host=None, raw=False):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        headers = {"Host": host or f"127.0.0.1:{self.port}"}
        if hasattr(self, "token"):
            headers["X-MM-Token"] = self.token
        if session:
            headers["X-MM-Session"] = session
        if isinstance(body, str):
            body = body.encode()
        conn.request(method, path, body=body, headers=headers)
        r = conn.getresponse()
        data = r.read()
        conn.close()
        if raw:
            return r.status, data
        import json
        try:
            return r.status, json.loads(data)
        except ValueError:
            return r.status, data

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


@unittest.skipUnless(find_gui(), "mm_gui isn't built")
class AccountsTest(unittest.TestCase):

    def setUp(self):
        self.work = tempfile.mkdtemp(prefix="mm_gui_test_")
        for folder in (f"tables/{MINE}", f"tables/{OTHER}", "tables/default", "configurations/lobby"):
            os.makedirs(os.path.join(self.work, folder))
        for path in (f"tables/{MINE}/mm_table_1d.bin", f"tables/{OTHER}/mm_table_1d.bin", "tables/default/mm_table_49.bin"):
            with open(os.path.join(self.work, path), "wb") as f:
                f.write(b"\0" * 8)
        self.panel = Panel(self.work)

    def tearDown(self):
        self.panel.stop()
        shutil.rmtree(self.work, ignore_errors=True)

    def call(self, method, path, body=None, session=None):
        return self.panel.request(method, path, body, session)

    def add_admin(self):
        status, r = self.call("POST", "/api/accounts/save", "name=alice\nrole=admin\npassword=correct horse")
        self.assertEqual(status, 200, r)
        return r["session"]

    def login(self, name, password):
        return self.call("POST", "/api/login", f"{name}\n{password}")

    def test_no_accounts_is_open_to_this_computer(self):
        status, r = self.call("GET", "/api/session")
        self.assertEqual((status, r["accounts"], r["user"]), (200, False, None))
        self.assertEqual(self.call("GET", "/api/accounts")[0], 200)
        self.assertEqual(self.call("GET", "/api/list?dir=tables&depth=2")[0], 200)
        # Still only for this computer, by name.
        self.assertEqual(self.panel.request("GET", "/api/session", host="evil.example:80")[0], 403)

    def test_first_account_must_be_an_admin(self):
        status, r = self.call("POST", "/api/accounts/save", "name=bob\nrole=user\npassword=12345678")
        self.assertEqual(status, 400)
        self.assertIn("admin", r["error"])
        status, r = self.call("POST", "/api/accounts/save", "name=alice\nrole=admin\npassword=short")
        self.assertEqual(status, 400)

    def test_sign_in_is_needed_once_there_are_accounts(self):
        session = self.add_admin()
        self.assertEqual(self.call("GET", "/api/info")[0], 401)
        self.assertEqual(self.call("GET", "/api/info", session=session)[0], 200)
        status, r = self.call("GET", "/api/session", session=session)
        self.assertEqual((r["accounts"], r["user"]["name"], r["user"]["admin"]), (True, "alice", True))
        self.assertEqual(self.login("alice", "wrong password")[0], 401)
        self.assertEqual(self.login("nobody", "wrong password")[0], 401)
        status, r = self.login("alice", "correct horse")
        self.assertEqual(status, 200)
        self.assertEqual(self.call("POST", "/api/logout", session=r["session"])[0], 200)
        self.assertEqual(self.call("GET", "/api/info", session=r["session"])[0], 401)
        # The accounts file can't be read through the file API.
        self.assertEqual(self.call("GET", "/api/file?path=.mm_gui/accounts", session=session)[0], 400)
        with open(os.path.join(self.work, ".mm_gui", "accounts")) as f:
            text = f.read()
        self.assertIn("user=alice", text)
        self.assertNotIn("correct horse", text)

    def test_wrong_passwords_lock_the_account_for_a_while(self):
        self.add_admin()
        for _ in range(5):
            self.assertEqual(self.login("alice", "wrong password")[0], 401)
        self.assertEqual(self.login("alice", "correct horse")[0], 429)

    def test_the_last_admin_stays(self):
        session = self.add_admin()
        status, r = self.call("POST", "/api/accounts/save", "name=alice\nrole=user", session)
        self.assertEqual(status, 409)
        self.assertEqual(self.call("POST", "/api/accounts/delete?name=alice", session=session)[0], 409)
        self.assertEqual(self.call("POST", "/api/accounts/save", "name=carol\nrole=admin\npassword=another one", session)[0], 200)
        self.assertEqual(self.call("POST", "/api/accounts/delete?name=alice", session=session)[0], 200)
        # alice's own session ended with her account.
        self.assertEqual(self.call("GET", "/api/info", session=session)[0], 401)

    def test_a_user_only_reaches_what_they_were_given(self):
        admin = self.add_admin()
        status, r = self.call("POST", "/api/accounts/save",
                              f"name=bob\nrole=user\npassword=bobs password\nphone={MINE} overview,display", admin)
        self.assertEqual(status, 200, r)
        self.assertEqual(r["user"]["phones"], [{"id": MINE, "perms": ["overview", "display"]}])
        status, r = self.login("bob", "bobs password")
        bob = r["session"]
        self.assertFalse(r["user"]["admin"])

        # Only their own phone's folder and the shared ones are listed.
        status, r = self.call("GET", "/api/list?dir=tables&depth=2", session=bob)
        paths = {e["path"] for e in r["entries"]}
        self.assertIn(f"tables/{MINE}/mm_table_1d.bin", paths)
        self.assertIn("tables/default/mm_table_49.bin", paths)
        self.assertFalse(any(OTHER in p for p in paths), paths)
        self.assertEqual(self.call("GET", "/api/list?dir=configurations&depth=1", session=bob)[1]["entries"], [])
        self.assertEqual(self.call("GET", "/api/list?dir=&depth=1", session=bob)[1]["entries"], [])

        # Reading.
        self.assertEqual(self.call("GET", f"/api/file?path=tables/{MINE}/mm_table_1d.bin", session=bob)[0], 200)
        self.assertEqual(self.call("GET", "/api/file?path=tables/default/mm_table_49.bin", session=bob)[0], 200)
        self.assertEqual(self.call("GET", f"/api/file?path=tables/{OTHER}/mm_table_1d.bin", session=bob)[0], 403)
        self.assertEqual(self.call("GET", "/api/file?path=mm_manager.db", session=bob)[0], 403)
        # Names Windows would read as another phone's folder.
        for odd in (f"{OTHER}%20", f"{OTHER}.", f"{OTHER}%20."):
            self.assertEqual(self.call("GET", f"/api/file?path=tables/{odd}/mm_table_1d.bin", session=bob)[0], 403, odd)
            self.assertEqual(self.call("GET", f"/api/list?dir=tables/{odd}", session=bob)[1]["entries"], [], odd)

        # Writing: only the groups ticked, in their own phone's folder.
        self.assertEqual(self.call("PUT", f"/api/file?path=tables/{MINE}/mm_table_1d.bin", b"\1" * 8, bob)[0], 200)
        self.assertEqual(self.call("PUT", f"/api/file?path=tables/{MINE}/phone.cfg", "name=lobby\n", bob)[0], 200)
        self.assertEqual(self.call("PUT", f"/api/file?path=tables/{MINE}/update_pending", "x\n", bob)[0], 200)
        self.assertEqual(self.call("PUT", f"/api/file?path=tables/{MINE}/mm_table_49.bin", b"\1" * 8, bob)[0], 403)  # prices
        self.assertEqual(self.call("PUT", f"/api/file?path=tables/{MINE}/phone_status", "x\n", bob)[0], 403)
        self.assertEqual(self.call("PUT", "/api/file?path=tables/default/mm_table_1d.bin", b"\1" * 8, bob)[0], 403)
        self.assertEqual(self.call("PUT", f"/api/file?path=tables/{OTHER}/mm_table_1d.bin", b"\1" * 8, bob)[0], 403)
        self.assertEqual(self.call("POST", f"/api/delete?path=tables/{MINE}/update_pending", session=bob)[0], 200)
        self.assertEqual(self.call("POST", f"/api/delete?path=tables/{OTHER}/mm_table_1d.bin", session=bob)[0], 403)
        self.assertTrue(os.path.exists(os.path.join(self.work, "tables", OTHER, "mm_table_1d.bin")))

        # Records need "records" on that phone.
        self.assertEqual(self.call("GET", "/api/db", session=bob)[0], 403)
        self.assertEqual(self.call("GET", f"/api/db?terminal={MINE}", session=bob)[0], 403)

        # Admin-only calls.
        for method, path in (("POST", "/api/run?job=manager"), ("POST", "/api/stop?job=manager"), ("GET", "/api/output?job=manager"),
                             ("GET", "/api/accounts"), ("POST", "/api/accounts/save"), ("POST", f"/api/rmdir?path=tables/{MINE}"),
                             ("POST", "/api/move?from=a&to=b"), ("POST", "/api/quit")):
            self.assertEqual(self.call(method, path, session=bob)[0], 403, path)

        # Given records, and the phone renamed: the account follows it.
        self.call("POST", "/api/accounts/save", f"name=bob\nrole=user\nphone={MINE} records", admin)
        self.assertEqual(self.call("GET", f"/api/db?terminal={MINE}", session=bob)[0], 200)
        self.assertEqual(self.call("POST", f"/api/phone/rename?dir=tables&from={MINE}&to=4085550000", session=admin)[0], 200)
        status, r = self.call("GET", "/api/session", session=bob)
        self.assertEqual(r["user"]["phones"], [{"id": "4085550000", "perms": ["records"]}])

    def test_password_change_and_restart(self):
        self.add_admin()
        status, r = self.login("alice", "correct horse")
        first = r["session"]
        second = self.login("alice", "correct horse")[1]["session"]
        self.assertEqual(self.call("POST", "/api/password", "wrong\nnew password!", first)[0], 403)
        self.assertEqual(self.call("POST", "/api/password", "correct horse\nnew password!", first)[0], 200)
        self.assertEqual(self.call("GET", "/api/info", session=first)[0], 200)
        self.assertEqual(self.call("GET", "/api/info", session=second)[0], 401)  # signed out elsewhere

        self.panel.stop()
        self.panel = Panel(self.work)
        self.assertTrue(self.call("GET", "/api/session")[1]["accounts"])
        self.assertEqual(self.login("alice", "correct horse")[0], 401)
        self.assertEqual(self.login("alice", "new password!")[0], 200)

    def test_settings_are_kept_in_the_working_folder(self):
        import json
        # No accounts: one set of settings, kept across a restart (and so another port).
        self.assertEqual(self.call("GET", "/api/prefs")[1]["prefs"], "")
        self.assertEqual(self.call("PUT", "/api/prefs", '{"mmTableEditor.theme":"dark"}')[0], 200)
        self.assertEqual(self.call("PUT", "/api/prefs?scope=manager", '{"mmTableEditor.manager":"{}"}')[0], 200)
        self.assertEqual(self.call("PUT", "/api/prefs", "not json")[0], 400)
        self.panel.stop()
        self.panel = Panel(self.work)
        self.assertEqual(json.loads(self.call("GET", "/api/prefs")[1]["prefs"]), {"mmTableEditor.theme": "dark"})

        # With accounts: each has their own; mm_manager's options are for admins only.
        admin = self.add_admin()
        self.assertEqual(self.call("GET", "/api/prefs", session=admin)[1]["prefs"], "")
        self.assertEqual(self.call("GET", "/api/prefs?scope=manager", session=admin)[1]["prefs"], '{"mmTableEditor.manager":"{}"}')
        self.call("POST", "/api/accounts/save", f"name=bob\nrole=user\npassword=bobs password\nphone={MINE} overview", admin)
        bob = self.login("bob", "bobs password")[1]["session"]
        self.assertEqual(self.call("PUT", "/api/prefs", '{"mmTableEditor.hints":"off"}', bob)[0], 200)
        self.assertEqual(self.call("GET", "/api/prefs", session=bob)[1]["prefs"], '{"mmTableEditor.hints":"off"}')
        self.assertEqual(self.call("GET", "/api/prefs", session=admin)[1]["prefs"], "")
        self.assertEqual(self.call("GET", "/api/prefs?scope=manager", session=bob)[0], 403)
        self.assertEqual(self.call("PUT", "/api/prefs?scope=manager", "{}", bob)[0], 403)
        self.assertEqual(self.call("GET", "/api/file?path=.mm_gui/prefs/user-bob.json", session=admin)[0], 400)
        # An account's settings go with it.
        self.assertEqual(self.call("POST", "/api/accounts/delete?name=bob", session=admin)[0], 200)
        self.assertFalse(os.path.exists(os.path.join(self.work, ".mm_gui", "prefs", "user-bob.json")))


if __name__ == "__main__":
    unittest.main()
