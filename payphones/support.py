"""Support levels for each payphone family, board and operation.

The compatibility matrix lives in config/compatibility.csv, one row per
(family, board, firmware, operation). docs/compatibility.md is generated
from it with `mm_payphone.py matrix --write`.
"""

import csv
import io
import os

# Operations, in the order the matrix shows them.
OPERATIONS = (
    "inventory",
    "file_editing",
    "communication",
    "programming",
    "reporting",
    "call_coin",
    "firmware",
)

OPERATION_TITLES = {
    "inventory": "Inventory",
    "file_editing": "File editing",
    "communication": "Communication",
    "programming": "Programming",
    "reporting": "Reporting",
    "call_coin": "Call/coin operation",
    "firmware": "Firmware updates",
}

# "hardware_verified" may only be used with a tested setup that names the
# board, firmware and connection equipment it was verified with.
STATUSES = ("planned", "experimental", "hardware_verified", "not_applicable")

STATUS_TITLES = {
    "planned": "planned",
    "experimental": "experimental",
    "hardware_verified": "hardware verified",
    "not_applicable": "n/a",
}

FIELDS = ("family", "board", "firmware", "operation", "status", "tested_setup", "limitations", "evidence")

HERE = os.path.dirname(os.path.abspath(__file__))


def _find_matrix():
    # The source checkout keeps config/ beside payphones/; a release keeps it in share/mm_manager.
    top = os.path.dirname(HERE)
    for d in (top, os.path.join(os.path.dirname(top), "share", "mm_manager")):
        path = os.path.join(d, "config", "compatibility.csv")
        if os.path.exists(path):
            return path
    return os.path.join(top, "config", "compatibility.csv")


DEFAULT_MATRIX = _find_matrix()


class SupportEntry:
    def __init__(self, family, board, firmware, operation, status, tested_setup="", limitations="", evidence=""):
        self.family = family
        self.board = board
        self.firmware = firmware
        self.operation = operation
        self.status = status
        self.tested_setup = tested_setup
        self.limitations = limitations
        self.evidence = evidence

    def as_dict(self):
        return {f: getattr(self, f) for f in FIELDS}

    def __repr__(self):
        return "SupportEntry(%s/%s/%s %s=%s)" % (self.family, self.board, self.firmware, self.operation, self.status)


def validate_entry(entry):
    """Returns a list of problems with one matrix row (empty if it is fine)."""
    problems = []
    where = "%s / %s / %s / %s" % (entry.family, entry.board, entry.firmware, entry.operation)
    if not entry.family or not entry.board:
        problems.append("%s: family and board are required" % where)
    if entry.operation not in OPERATIONS:
        problems.append("%s: unknown operation %r" % (where, entry.operation))
    if entry.status not in STATUSES:
        problems.append("%s: unknown status %r" % (where, entry.status))
    if entry.status == "hardware_verified" and not entry.tested_setup:
        problems.append("%s: hardware_verified needs the tested setup (board, firmware, connection)" % where)
    if entry.status in ("experimental", "hardware_verified") and not entry.evidence:
        problems.append("%s: %s needs evidence (tests, captures or a report)" % (where, entry.status))
    return problems


def load_matrix(path=DEFAULT_MATRIX):
    with open(path, newline="", encoding="utf-8") as f:
        return read_matrix(f)


def read_matrix(f):
    entries = []
    for row in csv.DictReader(line for line in f if not line.startswith("#")):
        entries.append(SupportEntry(**{k: (row.get(k) or "").strip() for k in FIELDS}))
    return entries


def validate_matrix(entries):
    problems = []
    seen = set()
    for e in entries:
        problems += validate_entry(e)
        key = (e.family, e.board, e.firmware, e.operation)
        if key in seen:
            problems.append("%s: listed twice" % " / ".join(key))
        seen.add(key)
    # Every board listed must state every operation, so nothing is implied.
    boards = {}
    for e in entries:
        boards.setdefault((e.family, e.board, e.firmware), set()).add(e.operation)
    for (family, board, firmware), ops in boards.items():
        missing = [op for op in OPERATIONS if op not in ops]
        if missing:
            problems.append("%s / %s / %s: no entry for %s" % (family, board, firmware, ", ".join(missing)))
    return problems


def _md(text):
    return text.replace("|", "\\|").replace("\n", " ")


def render_markdown(entries):
    """The compatibility matrix as Markdown: a summary grid, then the details of each entry."""
    out = io.StringIO()
    out.write("# Compatibility matrix\n\n")
    out.write("<!-- Generated from config/compatibility.csv by `mm_payphone.py matrix --write`. Edit the CSV, not this file. -->\n\n")
    out.write("Each operation is **planned**, **experimental** (implemented and tested in software, "
              "not yet on a physical phone) or **hardware verified** (tested on the board, firmware and "
              "connection named in the details). Nothing is supported unless it is listed here.\n\n")

    boards = []
    by_board = {}
    for e in entries:
        key = (e.family, e.board, e.firmware)
        if key not in by_board:
            boards.append(key)
            by_board[key] = {}
        by_board[key][e.operation] = e

    out.write("| Family | Board | Firmware | " + " | ".join(OPERATION_TITLES[o] for o in OPERATIONS) + " |\n")
    out.write("|---|---|---|" + "---|" * len(OPERATIONS) + "\n")
    for key in boards:
        cells = []
        for op in OPERATIONS:
            e = by_board[key].get(op)
            cells.append(STATUS_TITLES[e.status] if e else "?")
        out.write("| %s | %s | %s | %s |\n" % (_md(key[0]), _md(key[1]), _md(key[2] or "any"), " | ".join(cells)))

    for key in boards:
        out.write("\n## %s %s%s\n\n" % (key[0], key[1], (" (" + key[2] + ")") if key[2] else ""))
        out.write("| Operation | Status | Tested setup | Limitations | Evidence |\n|---|---|---|---|---|\n")
        for op in OPERATIONS:
            e = by_board[key].get(op)
            if not e:
                continue
            out.write("| %s | %s | %s | %s | %s |\n" % (
                OPERATION_TITLES[op], STATUS_TITLES[e.status], _md(e.tested_setup or "none"),
                _md(e.limitations or ""), _md(e.evidence or "")))
    return out.getvalue()
