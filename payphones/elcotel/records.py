"""Elcotel Series 5 speed dial (S94) and priority parsing (P94, with P99) files.

Both start with a 168-byte header, followed by a zero-run compressed body of
fixed-size records:

    header  0-3    format ID (not understood; kept as found)
            4-7    data length, little-endian
            8-15   model, 8 characters
            109    description length
            110-167 description
    S94     50 records of 17 bytes: pattern length, 11 digit codes, initial rate
            (nickels), initial time (minutes), additional rate, additional time, LATA type
    P94     50 records of 31 bytes: 24-byte pattern, call macro, completion timer,
            initial rate, initial time, additional rate, additional time, LATA type
    P99     records 51-250 of the same P94 file

Experimental: written from the published description and not yet checked
against an Elcotel phone or the Elcotel software. Unknown bytes are kept
from the file read, so reading and writing back reproduces it exactly.
"""

from . import codec

HEADER_LEN = 168
S94_RECORDS, S94_RECORD_LEN = 50, 17
P94_RECORDS, P99_RECORDS, P94_RECORD_LEN = 50, 200, 31
LATA_TYPES = range(0, 9)


def parse_header(data):
    if len(data) < HEADER_LEN:
        raise codec.FormatError("shorter than the %d-byte header" % HEADER_LEN)
    h = data[:HEADER_LEN]
    desc_len = min(h[109], HEADER_LEN - 110)
    return {
        "format_id": h[0:4].hex(),
        "data_length": int.from_bytes(h[4:8], "little"),
        "model": h[8:16].decode("latin-1").rstrip("\0 "),
        "description": h[110:110 + desc_len].decode("latin-1"),
        "raw": h.hex(),
    }


def build_header(header, body_len):
    """A header for a body: from the raw header read (if any), with the length, model and description updated."""
    h = bytearray(bytes.fromhex(header["raw"]) if header.get("raw") else bytes(HEADER_LEN))
    if not header.get("raw"):
        h[0:4] = bytes.fromhex(header.get("format_id") or "00000000")
    h[4:8] = body_len.to_bytes(4, "little")
    model = header.get("model", "").encode("latin-1")[:8]
    h[8:16] = model.ljust(8, b" ")
    desc = header.get("description", "").encode("latin-1")[:HEADER_LEN - 110]
    h[109] = len(desc)
    h[110:] = desc.ljust(HEADER_LEN - 110, b"\0")
    return bytes(h)


def _split(data, expected_body):
    header = parse_header(data)
    packed = data[HEADER_LEN:]
    warnings = []
    if header["data_length"] not in (len(packed), len(data)):
        warnings.append("header data length %d matches neither the body (%d) nor the file (%d)"
                        % (header["data_length"], len(packed), len(data)))
    return header, codec.decompress(packed, expected_body), warnings


def _rate_fields(rec):
    return {"initial_rate": rec[0], "initial_time": rec[1], "additional_rate": rec[2],
            "additional_time": rec[3], "lata_type": rec[4]}


def _check_rates(where, r, problems):
    for field in ("initial_rate", "additional_rate"):
        if not 0 <= r[field] <= 255:
            problems.append("%s: %s must be 0-255 nickels" % (where, field))
    for field in ("initial_time", "additional_time"):
        if not 0 <= r[field] <= 255:
            problems.append("%s: %s must be 0-255 minutes (254 unlimited, 255 restricted)" % (where, field))
    if r["lata_type"] not in LATA_TYPES:
        problems.append("%s: lata_type must be 0-8" % where)


# --- S94 ---

def parse_s94(data):
    header, body, warnings = _split(data, S94_RECORDS * S94_RECORD_LEN)
    entries = []
    for i in range(S94_RECORDS):
        rec = body[i * S94_RECORD_LEN:(i + 1) * S94_RECORD_LEN]
        n = rec[0]
        if n == 0:
            continue
        if n > 11:
            warnings.append("speed dial %d: pattern length %d is over 11" % (i + 1, n))
            n = 11
        entry = {"slot": i + 1, "number": codec.decode_digits(rec[1:1 + n])}
        entry.update(_rate_fields(rec[12:17]))
        entries.append(entry)
    return {"type": "S94", "header": header, "entries": entries, "warnings": warnings}


def build_s94(model):
    body = bytearray(S94_RECORDS * S94_RECORD_LEN)
    for e in model["entries"]:
        slot = int(e["slot"])
        if not 1 <= slot <= S94_RECORDS:
            raise codec.FormatError("speed dial slot %d is outside 1-50" % slot)
        digits = codec.encode_digits(e["number"], 11)
        o = (slot - 1) * S94_RECORD_LEN
        body[o] = len(e["number"])
        body[o + 1:o + 12] = digits
        body[o + 12:o + 17] = bytes((e["initial_rate"], e["initial_time"], e["additional_rate"],
                                     e["additional_time"], e["lata_type"]))
    packed = codec.compress(bytes(body))
    return build_header(model.get("header", {}), len(packed)) + packed


def check_s94(model):
    problems, slots = [], set()
    for e in model["entries"]:
        where = "speed dial %s" % e.get("slot")
        if e["slot"] in slots:
            problems.append("%s is listed twice" % where)
        slots.add(e["slot"])
        if not 1 <= int(e["slot"]) <= S94_RECORDS:
            problems.append("%s: slot must be 1-50" % where)
        if not e["number"]:
            problems.append("%s: number is empty" % where)
        try:
            codec.encode_digits(e["number"], 11)
        except codec.FormatError as err:
            problems.append("%s: %s" % (where, err))
        _check_rates(where, e, problems)
    return problems


# --- P94 / P99 ---

def _parse_p_records(body, first, count, warnings):
    entries = []
    for i in range(count):
        rec = body[i * P94_RECORD_LEN:(i + 1) * P94_RECORD_LEN]
        if not any(rec):
            continue
        pattern = rec[:24]
        # Padding is zero, and so is the digit 0: a pattern ending in 0 cannot be told
        # from its padding, so the raw bytes are kept and win when writing back.
        text = codec.decode_digits(pattern.rstrip(b"\0"))
        entry = {"record": first + i, "pattern": text, "pattern_raw": pattern.hex(),
                 "macro": rec[24], "completion_timer": rec[25]}
        entry.update(_rate_fields(rec[26:31]))
        entries.append(entry)
    return entries


def parse_p94(data, p99=None):
    header, body, warnings = _split(data, P94_RECORDS * P94_RECORD_LEN)
    entries = _parse_p_records(body, 1, P94_RECORDS, warnings)
    result = {"type": "P94", "header": header, "entries": entries, "warnings": warnings}
    if p99 is not None:
        h99, body99, w99 = _split(p99, P99_RECORDS * P94_RECORD_LEN)
        result["p99_header"] = h99
        result["entries"] += _parse_p_records(body99, P94_RECORDS + 1, P99_RECORDS, warnings)
        result["warnings"] += w99
    return result


def _p_record(e):
    if e.get("pattern_raw") and codec.decode_digits(bytes.fromhex(e["pattern_raw"]).rstrip(b"\0")) == e["pattern"]:
        pattern = bytes.fromhex(e["pattern_raw"])
    else:
        pattern = codec.encode_digits(e["pattern"], 24)
    return pattern + bytes((e["macro"], e["completion_timer"], e["initial_rate"], e["initial_time"],
                            e["additional_rate"], e["additional_time"], e["lata_type"]))


def build_p94(model):
    """(P94 bytes, P99 bytes or None)."""
    body = bytearray(P94_RECORDS * P94_RECORD_LEN)
    body99 = bytearray(P99_RECORDS * P94_RECORD_LEN)
    extended = False
    for e in model["entries"]:
        n = int(e["record"])
        if 1 <= n <= P94_RECORDS:
            body[(n - 1) * P94_RECORD_LEN:n * P94_RECORD_LEN] = _p_record(e)
        elif n <= P94_RECORDS + P99_RECORDS:
            o = (n - P94_RECORDS - 1) * P94_RECORD_LEN
            body99[o:o + P94_RECORD_LEN] = _p_record(e)
            extended = True
        else:
            raise codec.FormatError("record %d is outside 1-250" % n)
    packed = codec.compress(bytes(body))
    p94 = build_header(model.get("header", {}), len(packed)) + packed
    p99 = None
    if extended:
        packed99 = codec.compress(bytes(body99))
        p99 = build_header(model.get("p99_header", model.get("header", {})), len(packed99)) + packed99
    return p94, p99


def check_p94(model):
    problems, seen = [], set()
    for e in model["entries"]:
        where = "record %s" % e.get("record")
        if e["record"] in seen:
            problems.append("%s is listed twice" % where)
        seen.add(e["record"])
        if not 1 <= int(e["record"]) <= P94_RECORDS + P99_RECORDS:
            problems.append("%s: must be 1-250" % where)
        if "<" not in e["pattern"]:
            try:
                codec.encode_digits(e["pattern"], 24)
            except codec.FormatError as err:
                problems.append("%s: %s" % (where, err))
        for field in ("macro", "completion_timer"):
            if not 0 <= e[field] <= 255:
                problems.append("%s: %s must be 0-255" % (where, field))
        _check_rates(where, e, problems)
    return problems
