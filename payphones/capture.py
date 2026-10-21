"""Session captures for protocol research.

A capture is a JSON file: metadata describing exactly what was recorded, and
the bytes each side sent, in order. Change one setting per capture, so that
two captures can be compared to find where that setting travels.

    {
      "metadata": {
        "family": "Protel", "board": "7000", "firmware": "DD8715-2412R",
        "connection": "USR 5630G, analog line simulator", "modem_mode": "bell103",
        "direction": "phone_to_computer", "change": "band 0 initial rate 0.25 -> 0.35",
        "outcome": "complete", "audio": "capture-0001.wav"
      },
      "frames": [
        {"t": 0.000, "from": "phone", "hex": "<bytes the phone sent>"},
        {"t": 0.412, "from": "computer", "hex": "<bytes the computer sent>"}
      ]
    }

Serial transcripts can be imported from text lines "<seconds> <phone|computer> <hex bytes>".
The original audio, if any, is kept alongside and named in "audio".
"""

import json

REQUIRED = ("family", "board", "firmware", "connection", "modem_mode", "direction", "change", "outcome")
DIRECTIONS = ("phone_to_computer", "computer_to_phone")
SIDES = ("phone", "computer")
OUTCOMES = ("complete", "failed", "partial", "unknown")


class CaptureError(ValueError):
    pass


def check_metadata(meta):
    problems = ["metadata: %s is required" % k for k in REQUIRED if not str(meta.get(k, "")).strip()]
    if meta.get("direction") and meta["direction"] not in DIRECTIONS:
        problems.append("metadata: direction must be %s" % " or ".join(DIRECTIONS))
    if meta.get("outcome") and meta["outcome"] not in OUTCOMES:
        problems.append("metadata: outcome must be one of %s" % ", ".join(OUTCOMES))
    return problems


def frame_bytes(frame):
    try:
        return bytes.fromhex(frame.get("hex", ""))
    except ValueError:
        raise CaptureError("frame at t=%s has bad hex" % frame.get("t"))


def load(path):
    with open(path, encoding="utf-8") as f:
        capture = json.load(f)
    problems = check(capture)
    if problems:
        raise CaptureError("; ".join(problems))
    return capture


def check(capture):
    problems = check_metadata(capture.get("metadata", {}))
    last = None
    for frame in capture.get("frames", []):
        if frame.get("from") not in SIDES:
            problems.append("frame at t=%s: from must be phone or computer" % frame.get("t"))
        try:
            frame_bytes(frame)
        except CaptureError as e:
            problems.append(str(e))
        t = float(frame.get("t", 0))
        if last is not None and t < last:
            problems.append("frame at t=%s is earlier than the one before it" % t)
        last = t
    return problems


def import_transcript(lines, metadata):
    """A capture from transcript lines "<seconds> <phone|computer> <hex bytes>"; # starts a comment."""
    frames = []
    for n, line in enumerate(lines, 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split(None, 2)
        if len(parts) < 3 or parts[1] not in SIDES:
            raise CaptureError("line %d: expected <seconds> <phone|computer> <hex bytes>" % n)
        try:
            t = float(parts[0])
            data = bytes.fromhex(parts[2])
        except ValueError:
            raise CaptureError("line %d: bad time or hex" % n)
        frames.append({"t": t, "from": parts[1], "hex": data.hex(" ")})
    return {"metadata": dict(metadata), "frames": frames}


def stream(capture, side):
    """Everything one side sent, joined."""
    return b"".join(frame_bytes(f) for f in capture["frames"] if f["from"] == side)


def compare(a, b):
    """Where two captures' byte streams first differ, for each side."""
    result = {}
    for side in SIDES:
        x, y = stream(a, side), stream(b, side)
        first = next((i for i in range(min(len(x), len(y))) if x[i] != y[i]), None)
        if first is None and len(x) != len(y):
            first = min(len(x), len(y))
        result[side] = {"length": (len(x), len(y)), "first_difference": first,
                        "differing_bytes": sum(1 for i in range(min(len(x), len(y))) if x[i] != y[i])}
    meta_a, meta_b = a.get("metadata", {}), b.get("metadata", {})
    result["metadata_differences"] = {k: (meta_a.get(k), meta_b.get(k))
                                      for k in sorted(set(meta_a) | set(meta_b))
                                      if meta_a.get(k) != meta_b.get(k)}
    return result
