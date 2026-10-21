"""Reads ExpressNet export files (*.imp), as an interim bridge into the inventory.

ExpressNet's File Utilities export its site, options and costing records to
*.imp files, which another ExpressNet installation imports. Their layout is
not documented. What this reads was worked out from the one public set of
exports (ExpressNet 1.44, RATEFILESv1_44 on the Internet Archive), so it is
experimental: it only decodes the first record's name fields, and refuses
files that don't look like that sample.

    128 bytes   all zero
    1 byte      record type: g = sites, o = options (miscopt), i = costing (callcost), ...
    1 byte      format number (03 sites, 02 options, 05 costing in 1.44)
    records     fixed-width fields, text NUL-terminated and space-padded
"""

import os

HEADER_LEN = 128

RECORD_TYPES = {
    ord("g"): "sites",
    ord("o"): "options",
    ord("i"): "costing",
    ord("w"): "coin",
    ord("u"): "audit_cdr",
    ord("v"): "call_accounting",
    ord("m"): "option_config",
    ord("n"): "option_descriptions",
    0x80: "option_history",
}

# Text fields of the first record: (name, offset after the type byte's position, width).
FIELDS = {
    "sites": (("phone_number", 0x03, 10), ("description", 0x0E, 60), ("costing_record", 0x58, 12),
              ("options_record", 0x72, 12), ("group", 0x181, 6)),
    "options": (("name", 0x03, 12), ("description", 0x10, 60)),
    "costing": (("name", 0x03, 12), ("description", 0x10, 40), ("date", 0x39, 8)),
}

# The format numbers seen in the sample; others may lay the fields out differently.
KNOWN_FORMATS = {"sites": 3, "options": 2, "costing": 5}


class ImportFormatError(ValueError):
    pass


def _text(data, offset, width):
    raw = data[offset:offset + width + 1]
    if len(raw) < width + 1:
        raise ImportFormatError("file ends inside a field at offset %d" % offset)
    if raw[width] != 0:
        raise ImportFormatError("field at offset %d is not NUL-terminated" % offset)
    text = raw[:width]
    if any(b and not 32 <= b < 127 for b in text):
        raise ImportFormatError("field at offset %d is not text" % offset)
    return text.split(b"\0")[0].decode("ascii").rstrip()


def parse(data, name=""):
    if len(data) < HEADER_LEN + 2:
        raise ImportFormatError("%s: too short for an ExpressNet export" % name)
    if any(data[:HEADER_LEN]):
        raise ImportFormatError("%s: the first 128 bytes are not zero" % name)
    rtype = data[HEADER_LEN]
    if rtype not in RECORD_TYPES:
        raise ImportFormatError("%s: unknown record type 0x%02x" % (name, rtype))
    kind = RECORD_TYPES[rtype]
    fmt = data[HEADER_LEN + 1]
    result = {"file": name, "type": kind, "format": fmt, "records": [], "warnings": []}
    body = data[HEADER_LEN:]
    if len(body) <= 2:
        return result                       # an empty export
    if kind not in FIELDS:
        result["warnings"].append("%s records are not decoded" % kind)
        return result
    if fmt != KNOWN_FORMATS[kind]:
        result["warnings"].append("format %d differs from the ExpressNet 1.44 sample (%d); fields not decoded"
                                  % (fmt, KNOWN_FORMATS[kind]))
        return result
    result["records"].append({field: _text(body, off, width) for field, off, width in FIELDS[kind]})
    result["warnings"].append("only the first record is decoded; the record length is not known yet")
    return result


def read(path):
    with open(path, "rb") as f:
        return parse(f.read(), os.path.basename(path))


def inventory_entries(results):
    """Inventory entries for the phones in a set of parsed sites exports."""
    entries = []
    for r in results:
        if r["type"] != "sites":
            continue
        for rec in r["records"]:
            entries.append({
                "family": "Protel",
                "phone_number": rec["phone_number"],
                "name": rec["description"],
                "group": rec["group"],
                "costing_record": rec["costing_record"],
                "options_record": rec["options_record"],
                "source": "ExpressNet export %s" % r["file"],
            })
    return entries
