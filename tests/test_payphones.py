#!/usr/bin/env python3
# Tests for the payphones package (Protel and Elcotel backends, captures and
# the compatibility matrix). Run: python3 -m unittest tests/test_payphones.py

import json
import os
import shutil
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from payphones import backend, capture, support  # noqa: E402
from payphones import elcotel  # noqa: E402
from payphones.elcotel import codec, r94, records  # noqa: E402
from payphones.protel import catalog, config, xnet_import  # noqa: E402


class TestElcotelCodec(unittest.TestCase):
    def test_zero_runs(self):
        data = b"\x01" + bytes(300) + b"\x02\x00\x03" + bytes(5)
        packed = codec.compress(data)
        self.assertEqual(codec.decompress(packed, len(data)), data)
        self.assertEqual(packed[:5], b"\x01\x00\xff\x00\x2d")

    def test_lone_trailing_zero(self):
        self.assertEqual(codec.decompress(b"\x05\x00", 4), b"\x05\x00\x00\x00")
        with self.assertRaises(codec.FormatError):
            codec.decompress(b"\x05\x00\x02", 4)

    def test_digits(self):
        self.assertEqual(codec.encode_digits("1*#A@"), bytes((1, 0x0B, 0x0C, 0x0D, 0xF0)))
        self.assertEqual(codec.decode_digits(codec.encode_digits("9#B$?+")), "9#B$?+")
        with self.assertRaises(codec.FormatError):
            codec.encode_digits("12x")
        with self.assertRaises(codec.FormatError):
            codec.encode_digits("123456789012", 11)


class TestElcotelRecords(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_s94_round_trip(self):
        model = {"type": "S94", "header": {"model": "SERIES5", "description": "Test speed dials"},
                 "entries": [{"slot": 1, "number": "18005551212", "initial_rate": 0, "initial_time": 254,
                              "additional_rate": 0, "additional_time": 0, "lata_type": 0},
                             {"slot": 50, "number": "911", "initial_rate": 0, "initial_time": 254,
                              "additional_rate": 0, "additional_time": 0, "lata_type": 1}]}
        self.assertEqual(records.check_s94(model), [])
        path = os.path.join(self.dir, "TEST.S94")
        elcotel.write_file(model, path)
        back = elcotel.read_file(path)
        self.assertEqual(back["warnings"], [])
        self.assertEqual(back["header"]["model"], "SERIES5")
        self.assertEqual(back["header"]["description"], "Test speed dials")
        self.assertEqual([(e["slot"], e["number"], e["lata_type"]) for e in back["entries"]],
                         [(1, "18005551212", 0), (50, "911", 1)])
        # Writing what was read reproduces the file.
        with open(path, "rb") as f:
            original = f.read()
        self.assertEqual(records.build_s94(back), original)

    def test_s94_check(self):
        model = {"type": "S94", "entries": [
            {"slot": 51, "number": "12x", "initial_rate": 300, "initial_time": 1,
             "additional_rate": 0, "additional_time": 0, "lata_type": 9}]}
        problems = records.check_s94(model)
        self.assertEqual(len(problems), 4, problems)

    def test_p94_with_p99(self):
        entry = {"pattern": "1800", "macro": 1, "completion_timer": 30, "initial_rate": 0,
                 "initial_time": 254, "additional_rate": 0, "additional_time": 0, "lata_type": 0}
        model = {"type": "P94", "header": {"model": "SERIES5", "description": "P"},
                 "entries": [dict(entry, record=1), dict(entry, record=60, pattern="011")]}
        path = os.path.join(self.dir, "TEST.P94")
        written = elcotel.write_file(model, path)
        self.assertEqual([os.path.basename(p) for p in written], ["TEST.P94", "TEST.P99"])
        back = elcotel.read_file(path)
        self.assertEqual([(e["record"], e["pattern"]) for e in back["entries"]], [(1, "18"), (60, "011")])
        # "1800" ends in zeros, which look like padding, but its raw bytes keep it intact.
        p94, _ = records.build_p94(back)
        with open(path, "rb") as f:
            self.assertEqual(p94, f.read())
        # Without records past 50 the companion is removed.
        model["entries"] = model["entries"][:1]
        self.assertEqual(len(elcotel.write_file(model, path)), 1)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "TEST.P99")))


def synthetic_r94():
    """A small R94 file built here (not from any published file): 3 bands, 1 NPA group, 1 NXX table."""
    body = bytearray(800)
    for i in range(800):
        body[i] = 2
    body[208 - 200] = 254
    body[800 - 200] = 253
    body[900 - 200] = 0
    body += bytes(64)
    counts = bytearray(26)
    counts[0:3] = b"\x01\x01\x01"
    counts[23], counts[24], counts[25] = 1, 3, 1
    body += counts
    body += bytes((208, 0, 1, 1, 0, 1))
    body += bytes((5, 0, 0, 1)) + bytes((7, 3, 7, 3)) + bytes((20, 3, 20, 3))
    body += bytes(103)
    header = bytearray(268)
    header[0] = 0x06
    header[1:3] = len(body).to_bytes(2, "little")
    return bytes(header) + codec.compress(bytes(body))


class TestR94(unittest.TestCase):
    def test_round_trip(self):
        data = synthetic_r94()
        model = r94.parse(data)
        self.assertEqual(model["warnings"], [])
        self.assertEqual(r94.check(model), [])
        self.assertEqual(model["npa_defaults"][208], "nxx")
        self.assertEqual(model["npa_defaults"][800], "unlimited")
        self.assertEqual(model["npa_defaults"][900], "restricted")
        self.assertEqual(model["npa_defaults"][212], 2)
        self.assertEqual(model["price_bands"][1], {"band": 2, "initial_rate": 7, "initial_time": 3,
                                                   "additional_rate": 7, "additional_time": 3})
        self.assertEqual(r94.build(model), data)
        # Through JSON too, as the command line tool does.
        self.assertEqual(r94.build(json.loads(json.dumps(model))), data)

    def test_edit_and_check(self):
        model = r94.parse(synthetic_r94())
        model["npa_defaults"][212] = 9
        self.assertTrue(any("212" in p for p in r94.check(model)))
        model["npa_defaults"][212] = 3
        model["price_bands"][0]["initial_rate"] = 10
        again = r94.parse(r94.build(model))
        self.assertEqual(again["npa_defaults"][212], 3)
        self.assertEqual(again["price_bands"][0]["initial_rate"], 10)

    def test_bad_counts(self):
        # An NXX table more than the counts say: the body no longer adds up.
        data = synthetic_r94()
        body = codec.decompress(data[268:], int.from_bytes(data[1:3], "little")) + bytes(103)
        header = bytearray(data[:268])
        header[1:3] = len(body).to_bytes(2, "little")
        with self.assertRaises(codec.FormatError):
            r94.parse(bytes(header) + codec.compress(body))


class TestProtel(unittest.TestCase):
    def test_free_calling_is_valid(self):
        cfg = config.free_calling()
        self.assertEqual(config.check(cfg, firmware="DD", model="7000"), ([], [], []))

    def test_limits(self):
        cfg = {"cost_bands": {"0": {"initial_rate": 13.0, "overtime_period": 16, "keypad": 0, "route": 16,
                                    "answer_detection": 2, "lata": "x", "colour": 1},
                              "200": {}}}
        problems, unsupported, _ = config.check(cfg)
        self.assertEqual(len(problems), 7, problems)
        self.assertEqual(unsupported, ["cost band 0: unknown field 'colour'"])

    def test_coin_line_rules(self):
        cfg = {"options": {"127": "coin"},
               "cost_bands": {"0": {"overtime_rate": 0.25}, "88": {"initial_rate": 0.25}}}
        problems, _, _ = config.check(cfg, firmware="DD")
        self.assertEqual(len(problems), 2, problems)

    def test_losses_and_unsupported(self):
        cfg = {"cost_bands": {"5": {"initial_rate": 0.27, "overtime_period": 0}},
               "options": {"167": "Type1", "999": 1}, "modem": "protel1200"}
        problems, unsupported, losses = config.check(cfg, firmware="CA", model="4000")
        self.assertEqual(problems, [])
        self.assertEqual(len(unsupported), 3, unsupported)
        self.assertEqual(len(losses), 2, losses)

    def test_assignments(self):
        cfg = {"npa_bands": {"212": 0, "21": 80}, "nxx_bands": {"718": {"555": 79, "5x5": 1}}}
        problems, _, _ = config.check(cfg)
        self.assertEqual(len(problems), 3, problems)

    def test_errors(self):
        self.assertEqual(catalog.explain_error(14)["phase"], "security")
        self.assertIs(catalog.explain_error(6)["reached_phone"], False)
        self.assertIs(catalog.explain_error(21)["reached_phone"], True)
        self.assertIsNone(catalog.explain_error(1)["reached_phone"])
        self.assertIsNone(catalog.explain_error(200)["phase"])
        phases = {p for p, _ in catalog.PHASES}
        self.assertTrue(all(phase in phases for _, phase in catalog.COMM_ERRORS.values()))

    def test_bands(self):
        self.assertEqual(catalog.band_kind(0), "area/exchange")
        self.assertEqual(catalog.band_kind(88), "incoming")
        self.assertEqual(catalog.band_kind(137), "*1 to *7")
        with self.assertRaises(ValueError):
            catalog.band_kind(138)
        covered = [b for r in catalog.SPECIAL_BANDS.values() for b in r]
        self.assertEqual(sorted(covered), list(range(80, 138)))

    def test_backend(self):
        b = backend.get("protel")
        ident = b.identify({"model": "7000", "firmware": "CD8715-2412R"})
        self.assertEqual(ident.details["firmware_type"], "CD")
        self.assertIn("note", b.identify({"model": "Elite"}).details)
        self.assertFalse(b.prepare({}, board="Elite").ok)
        with self.assertRaises(backend.NotSupported):
            b.program("2125551212", b.prepare(config.free_calling()), None)
        ok = b.session_result("2125551212", "programming")
        self.assertEqual((ok.outcome, ok.verification), ("complete", "acknowledged"))
        bad = b.session_result("2125551212", "programming", 21)
        self.assertEqual((bad.outcome, bad.phase), ("failed", "full_download"))
        self.assertEqual(b.session_result("x", "programming", 75).outcome, "partial")


def synthetic_sites_imp():
    rec = bytearray(0x200)
    rec[0], rec[1] = ord("g"), 3

    def put(off, text, width):
        rec[off:off + width + 1] = text.ljust(width).encode() + b"\0"
    put(0x03, "2125551212", 10)
    put(0x0E, "LOBBY PHONE", 60)
    put(0x58, "NYC.RTS", 12)
    put(0x72, "OPTIONS1", 12)
    put(0x181, "GRP001", 6)
    return bytes(128) + bytes(rec)


class TestXnetImport(unittest.TestCase):
    def test_sites(self):
        r = xnet_import.parse(synthetic_sites_imp(), "sites.imp")
        self.assertEqual(r["records"], [{"phone_number": "2125551212", "description": "LOBBY PHONE",
                                         "costing_record": "NYC.RTS", "options_record": "OPTIONS1",
                                         "group": "GRP001"}])
        inv = xnet_import.inventory_entries([r])
        self.assertEqual(inv[0]["phone_number"], "2125551212")

    def test_rejects(self):
        with self.assertRaises(xnet_import.ImportFormatError):
            xnet_import.parse(b"\x01" + synthetic_sites_imp()[1:], "x")
        with self.assertRaises(xnet_import.ImportFormatError):
            xnet_import.parse(bytes(128) + b"Z\x01", "x")
        broken = bytearray(synthetic_sites_imp())
        broken[128 + 0x03 + 10] = 0x41
        with self.assertRaises(xnet_import.ImportFormatError):
            xnet_import.parse(bytes(broken), "x")
        empty = xnet_import.parse(bytes(128) + b"w\x02", "coin.imp")
        self.assertEqual((empty["type"], empty["records"]), ("coin", []))
        other = bytearray(synthetic_sites_imp())
        other[129] = 4
        self.assertEqual(xnet_import.parse(bytes(other), "x")["records"], [])


class TestCapture(unittest.TestCase):
    META = {"family": "Protel", "board": "7000", "firmware": "DD", "connection": "test",
            "modem_mode": "bell103", "direction": "phone_to_computer", "change": "none", "outcome": "unknown"}

    def test_import_and_compare(self):
        a = capture.import_transcript(["0.0 phone 01 02 03", "# comment", "0.5 computer 06"], self.META)
        b = capture.import_transcript(["0.0 phone 01 09 03 04", "0.5 computer 06"],
                                      dict(self.META, change="band 0 rate"))
        self.assertEqual(capture.check(a), [])
        diff = capture.compare(a, b)
        self.assertEqual(diff["phone"]["first_difference"], 1)
        self.assertEqual(diff["phone"]["length"], (3, 4))
        self.assertIsNone(diff["computer"]["first_difference"])
        self.assertEqual(diff["metadata_differences"], {"change": ("none", "band 0 rate")})

    def test_problems(self):
        with self.assertRaises(capture.CaptureError):
            capture.import_transcript(["0.0 modem 01"], self.META)
        cap = capture.import_transcript(["1.0 phone 01", "0.5 phone 02"], {"direction": "sideways"})
        problems = capture.check(cap)
        self.assertTrue(any("earlier" in p for p in problems))
        self.assertTrue(any("direction" in p for p in problems))
        self.assertTrue(any("board is required" in p for p in problems))


class TestMatrix(unittest.TestCase):
    def test_matrix_is_valid(self):
        self.assertEqual(support.validate_matrix(support.load_matrix()), [])

    def test_doc_is_current(self):
        with open(os.path.join(ROOT, "docs", "compatibility.md"), encoding="utf-8") as f:
            self.assertEqual(f.read(), support.render_markdown(support.load_matrix()),
                             "run: python3 mm_payphone.py matrix --write")

    def test_rules(self):
        e = support.SupportEntry("X", "Y", "", "programming", "hardware_verified", evidence="report")
        self.assertTrue(any("tested setup" in p for p in support.validate_entry(e)))
        e = support.SupportEntry("X", "Y", "", "programming", "experimental")
        self.assertTrue(any("evidence" in p for p in support.validate_entry(e)))
        missing = support.validate_matrix([support.SupportEntry("X", "Y", "", "inventory", "planned")])
        self.assertTrue(any("no entry for" in p for p in missing))

    def test_backends_listed(self):
        families = {e.family for e in support.load_matrix()}
        for b in backend.backends().values():
            self.assertIn(b.family, families)


if __name__ == "__main__":
    unittest.main()
