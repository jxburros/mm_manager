"""Elcotel Series 5 and related boards.

Reads, checks and writes the configuration files the Elcotel software sends
to the phone: rate files (R94), speed dials (S94) and priority parsing
(P94, with P99). Native transfer to the phone is a separate milestone and is
not implemented, so program() raises NotSupported.
"""

import os

from .. import backend
from . import codec, r94, records

EXTENSIONS = {".r94": "R94", ".s94": "S94", ".p94": "P94"}


def read_file(path):
    kind = EXTENSIONS.get(os.path.splitext(path)[1].lower())
    if not kind:
        raise codec.FormatError("%s: not an R94, S94 or P94 file" % path)
    with open(path, "rb") as f:
        data = f.read()
    if kind == "R94":
        return r94.parse(data)
    if kind == "S94":
        return records.parse_s94(data)
    p99 = None
    for ext in (".P99", ".p99"):
        companion = os.path.splitext(path)[0] + ext
        if os.path.exists(companion):
            with open(companion, "rb") as f:
                p99 = f.read()
            break
    return records.parse_p94(data, p99)


def check_model(model):
    return {"R94": r94.check, "S94": records.check_s94, "P94": records.check_p94}[model["type"]](model)


def write_file(model, path):
    """Writes a model to path (and its .P99 companion for P94 records past 50). Returns the paths written."""
    if model["type"] == "R94":
        data = {path: r94.build(model)}
    elif model["type"] == "S94":
        data = {path: records.build_s94(model)}
    else:
        p94, p99 = records.build_p94(model)
        companion = os.path.splitext(path)[0] + (".P99" if path.endswith(".P94") else ".p99")
        data = {path: p94}
        if p99 is not None:
            data[companion] = p99
        elif os.path.exists(companion):
            os.remove(companion)
    for p, d in data.items():
        with open(p, "wb") as f:
            f.write(d)
    return list(data)


@backend.register
class ElcotelBackend(backend.Backend):
    name = "elcotel"
    family = "Elcotel"
    title = "Elcotel Series 5 (configuration files)"

    def identify(self, evidence):
        return backend.Identification(self.family, board=str(evidence.get("board", "")),
                                      firmware=str(evidence.get("firmware", "")),
                                      confidence="claimed" if evidence.get("board") else "unknown")

    def prepare(self, model, board="", firmware=""):
        return backend.PreparedConfiguration(model, problems=check_model(model))

    def program(self, phone_id, prepared, connection):
        raise backend.NotSupported("Elcotel native transfer is not implemented; load the files with the "
                                   "Elcotel software for now")
