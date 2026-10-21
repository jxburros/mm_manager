"""Elcotel Series 5 rate files (R94).

A 268-byte header, then a zero-run compressed body. The body is addressed by
offsets that follow from counts it holds:

    0     800      NPA defaults, one byte per area code 200-999: 0 restricted,
                   253 unlimited, 254/255 NXX-specific, else a 1-based price band
    800   64       surcharges, eight 8-byte rows (column-major)
    864   26       price plan counts; bytes 0-7 band counts per category,
                   23 NPA groups (N), 24 price bands (B), 25 NXX tables (T)
    890   6 x N    NPA group headers
          4 x B    price bands: initial rate, initial time, additional rate, additional time
          103 x T  NXX tables (100-byte bitmap and 3 bytes of metadata)

The layout of the first 864 bytes is from elcotel-playground's published
description (clean room; none of its GPL-3.0 code is used). The meaning of
the counts at 887-889, and that header bytes 1-2 hold the expanded body
length, were found here from the one public sample (208209BN.R94), where
890 + 6N + 4B + 103T equals the body length exactly. The group headers and
NXX tables are kept as raw bytes, since their fields are not yet understood.
"""

from . import codec

HEADER_LEN = 268
NPA_FIRST, NPA_LAST = 200, 999
NPA_TABLE = 0
SURCHARGES = 800
PLAN_COUNTS = 864
GROUPS = 890
GROUP_LEN, BAND_LEN, NXX_TABLE_LEN = 6, 4, 103

NPA_TOKENS = {0: "restricted", 253: "unlimited", 254: "nxx", 255: "nxx"}


def parse(data):
    if len(data) < HEADER_LEN + 2:
        raise codec.FormatError("shorter than an R94 header")
    header = data[:HEADER_LEN]
    warnings = []
    if header[0] != 0x06:
        warnings.append("first header byte is 0x%02x; every known R94 file has 0x06" % header[0])
    body_len = int.from_bytes(header[1:3], "little")
    body = codec.decompress(data[HEADER_LEN:], body_len)
    if len(body) < GROUPS:
        raise codec.FormatError("body is %d bytes, too short for an R94 file" % len(body))

    counts = body[PLAN_COUNTS:GROUPS]
    n_groups, n_bands, n_tables = counts[23], counts[24], counts[25]
    expected = GROUPS + GROUP_LEN * n_groups + BAND_LEN * n_bands + NXX_TABLE_LEN * n_tables
    if expected != len(body):
        raise codec.FormatError("body is %d bytes but its counts (%d groups, %d bands, %d tables) need %d"
                                % (len(body), n_groups, n_bands, n_tables, expected))

    npa = {}
    for i, v in enumerate(body[NPA_TABLE:NPA_TABLE + 800]):
        npa[NPA_FIRST + i] = NPA_TOKENS.get(v, v)
    o = GROUPS
    groups = [body[o + i * GROUP_LEN:o + (i + 1) * GROUP_LEN].hex() for i in range(n_groups)]
    o += GROUP_LEN * n_groups
    bands = []
    for i in range(n_bands):
        b = body[o + i * BAND_LEN:o + (i + 1) * BAND_LEN]
        bands.append({"band": i + 1, "initial_rate": b[0], "initial_time": b[1],
                      "additional_rate": b[2], "additional_time": b[3]})
    o += BAND_LEN * n_bands
    tables = [body[o + i * NXX_TABLE_LEN:o + (i + 1) * NXX_TABLE_LEN].hex() for i in range(n_tables)]

    return {
        "type": "R94",
        "header": header.hex(),
        "npa_defaults": npa,
        "surcharges": body[SURCHARGES:PLAN_COUNTS].hex(),
        "plan_counts": counts.hex(),
        "category_band_counts": list(counts[0:8]),
        "npa_groups": groups,
        "price_bands": bands,
        "nxx_tables": tables,
        "warnings": warnings,
    }


def build(model):
    counts = bytearray(bytes.fromhex(model["plan_counts"]))
    counts[23], counts[24], counts[25] = len(model["npa_groups"]), len(model["price_bands"]), len(model["nxx_tables"])
    body = bytearray()
    tokens = {"restricted": 0, "unlimited": 253, "nxx": 254}
    for code in range(NPA_FIRST, NPA_LAST + 1):
        v = model["npa_defaults"].get(code, model["npa_defaults"].get(str(code), 0))
        body.append(tokens[v] if isinstance(v, str) else int(v))
    body += bytes.fromhex(model["surcharges"])
    body += counts
    for g in model["npa_groups"]:
        body += bytes.fromhex(g)
    for b in model["price_bands"]:
        body += bytes((b["initial_rate"], b["initial_time"], b["additional_rate"], b["additional_time"]))
    for t in model["nxx_tables"]:
        body += bytes.fromhex(t)
    header = bytearray(bytes.fromhex(model["header"]))
    header[1:3] = len(body).to_bytes(2, "little")
    return bytes(header) + codec.compress(bytes(body))


def check(model):
    problems = []
    n_bands = len(model["price_bands"])
    for code, v in model["npa_defaults"].items():
        if isinstance(v, int) and not 1 <= v <= n_bands:
            problems.append("area code %s uses price band %d, but there are %d" % (code, v, n_bands))
        elif isinstance(v, str) and v not in ("restricted", "unlimited", "nxx"):
            problems.append("area code %s: unknown value %r" % (code, v))
    if sum(model["category_band_counts"]) != n_bands:
        problems.append("category band counts add up to %d, but there are %d price bands"
                        % (sum(model["category_band_counts"]), n_bands))
    for b in model["price_bands"]:
        for field in ("initial_rate", "initial_time", "additional_rate", "additional_time"):
            if not 0 <= b[field] <= 255:
                problems.append("price band %d: %s must be 0-255" % (b["band"], field))
    if len(bytes.fromhex(model["surcharges"])) != 64:
        problems.append("surcharges must be 64 bytes")
    for i, t in enumerate(model["nxx_tables"]):
        if len(bytes.fromhex(t)) != NXX_TABLE_LEN:
            problems.append("NXX table %d must be %d bytes" % (i + 1, NXX_TABLE_LEN))
    for i, g in enumerate(model["npa_groups"]):
        if len(bytes.fromhex(g)) != GROUP_LEN:
            problems.append("NPA group %d must be %d bytes" % (i + 1, GROUP_LEN))
    return problems
