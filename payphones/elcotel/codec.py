"""Encodings shared by the Elcotel Series 5 files (R94, S94, P94/P99).

Clean-room implementation from the published format description in
elcotel-playground's README (https://github.com/vekejsn/elcotel-playground);
no code from that GPL-3.0 project is used.
"""

# A 0x00 byte is followed by the number of zeros it stands for.
MAX_RUN = 255

# Token values used in rate and time fields.
UNUSED = 0xF4
UNLIMITED = 0xFE
RESTRICTED = 0xFF

DIGIT_CODES = {str(d): d for d in range(10)}
DIGIT_CODES.update({"*": 0x0B, "#": 0x0C, "A": 0x0D, "B": 0x0E, "C": 0x0F, "D": 0x10,
                    "@": 0xF0, "$": 0xF1, "?": 0xF2, "+": 0xF3, "!": 0xF4})
CODE_DIGITS = {v: k for k, v in DIGIT_CODES.items()}


class FormatError(ValueError):
    pass


def decompress(data, expected_len=None):
    """Expands zero runs. A lone 0x00 at the very end is padded out to expected_len (or one zero)."""
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0:
            if i + 1 < len(data):
                out += bytes(data[i + 1])
                i += 2
            else:
                missing = expected_len - len(out) if expected_len is not None else 1
                out += bytes(max(missing, 0))
                i += 1
        else:
            out.append(b)
            i += 1
    if expected_len is not None and len(out) != expected_len:
        raise FormatError("body expands to %d bytes, expected %d" % (len(out), expected_len))
    return bytes(out)


def compress(data, max_run=MAX_RUN):
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 0:
            j = i
            while j < len(data) and data[j] == 0 and j - i < max_run:
                j += 1
            out += bytes((0, j - i))
            i = j
        else:
            out.append(data[i])
            i += 1
    return bytes(out)


def encode_digits(text, width=None):
    try:
        codes = bytes(DIGIT_CODES[c] for c in text.upper())
    except KeyError as e:
        raise FormatError("%r cannot be dialed (allowed: 0-9 * # A-D @ $ ? + !)" % e.args[0])
    if width is not None:
        if len(codes) > width:
            raise FormatError("%r is longer than %d digits" % (text, width))
        codes += bytes(width - len(codes))
    return codes


def decode_digits(codes):
    return "".join(CODE_DIGITS.get(c, "<%02x>" % c) for c in codes)
