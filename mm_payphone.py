#!/usr/bin/env python3
# Tools for the payphone families other than the Nortel Millennium, and for
# the compatibility matrix. Python 3 standard library only.
#
#   mm_payphone.py matrix [--check] [--write]       the compatibility matrix (docs/compatibility.md)
#   mm_payphone.py backends                          backends and what each supports
#   mm_payphone.py protel check CONFIG.json [--firmware DD] [--model 7000]
#   mm_payphone.py protel free-calling OUT.json      a configuration with every area band free
#   mm_payphone.py protel error CODE...              explain ExpressNet communication errors
#   mm_payphone.py protel import FILE.imp...         read ExpressNet exports into inventory entries
#   mm_payphone.py elcotel read FILE [--json OUT]    read an R94, S94 or P94 file
#   mm_payphone.py elcotel check FILE|JSON           check a file or an edited JSON dump
#   mm_payphone.py elcotel write JSON OUT            write a JSON dump back to a file
#   mm_payphone.py capture import TRANSCRIPT OUT.json key=value...
#   mm_payphone.py capture check CAPTURE.json
#   mm_payphone.py capture compare A.json B.json

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from payphones import backend, capture, support  # noqa: E402
from payphones import elcotel, protel  # noqa: E402
from payphones.elcotel import codec  # noqa: E402
from payphones.protel import catalog, config, xnet_import  # noqa: E402

DOC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "docs", "compatibility.md")


def report(problems, unsupported=(), losses=()):
    for p in problems:
        print("error: " + p)
    for u in unsupported:
        print("not applied: " + u)
    for loss in losses:
        print("changed: " + loss)
    if not (problems or unsupported or losses):
        print("ok")
    return 1 if problems else 0


def cmd_matrix(args):
    entries = support.load_matrix(args.file)
    problems = support.validate_matrix(entries)
    if problems or args.check:
        rc = report(problems)
        if problems or not args.write:
            return rc
    text = support.render_markdown(entries)
    if args.write:
        with open(DOC, "w", encoding="utf-8") as f:
            f.write(text)
        print("wrote " + DOC)
    elif not args.check:
        sys.stdout.write(text)
    return 0


def cmd_backends(args):
    matrix = support.load_matrix()
    for name, b in sorted(backend.backends().items()):
        print("%s: %s" % (name, b.title))
        rows = b.capabilities(matrix)
        for op in support.OPERATIONS:
            statuses = sorted({support.STATUS_TITLES[e.status] for e in rows if e.operation == op})
            print("  %-14s %s" % (support.OPERATION_TITLES[op], ", ".join(statuses) or "not listed"))
    return 0


def cmd_protel(args):
    if args.action == "check":
        cfg = config.load(args.files[0])
        prepared = protel.ProtelBackend().prepare(cfg, board=args.model or "", firmware=args.firmware or "")
        return report(prepared.problems, prepared.unsupported, prepared.losses)
    if args.action == "free-calling":
        config.save(config.free_calling(), args.files[0])
        print("wrote " + args.files[0])
        return 0
    if args.action == "error":
        for code in args.files:
            e = catalog.explain_error(code)
            print("%s: %s [%s]" % (e["code"], e["description"], e.get("phase_title") or "unknown phase"))
        return 0
    if args.action == "import":
        results = []
        for path in args.files:
            try:
                results.append(xnet_import.read(path))
            except xnet_import.ImportFormatError as e:
                print("error: %s" % e)
                return 1
        print(json.dumps({"files": results, "inventory": xnet_import.inventory_entries(results)}, indent=2))
        return 0
    return 2


def _load_elcotel(path):
    if path.lower().endswith(".json"):
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    return elcotel.read_file(path)


def cmd_elcotel(args):
    try:
        if args.action == "read":
            model = elcotel.read_file(args.files[0])
            text = json.dumps(model, indent=2)
            if args.json:
                with open(args.json, "w", encoding="utf-8") as f:
                    f.write(text + "\n")
                print("wrote " + args.json)
            else:
                print(text)
            for w in model.get("warnings", []):
                print("warning: " + w, file=sys.stderr)
            return 0
        if args.action == "check":
            return report(elcotel.check_model(_load_elcotel(args.files[0])))
        if args.action == "write":
            model = _load_elcotel(args.files[0])
            problems = elcotel.check_model(model)
            if problems:
                return report(problems)
            for p in elcotel.write_file(model, args.files[1]):
                print("wrote " + p)
            return 0
    except codec.FormatError as e:
        print("error: %s" % e)
        return 1
    return 2


def cmd_capture(args):
    if args.action == "import":
        transcript, out = args.files[0], args.files[1]
        meta = dict(kv.split("=", 1) for kv in args.files[2:] if "=" in kv)
        with open(transcript, encoding="utf-8") as f:
            cap = capture.import_transcript(f, meta)
        problems = capture.check(cap)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(cap, f, indent=2)
        print("wrote " + out)
        return report(problems)
    if args.action == "check":
        with open(args.files[0], encoding="utf-8") as f:
            return report(capture.check(json.load(f)))
    if args.action == "compare":
        a, b = capture.load(args.files[0]), capture.load(args.files[1])
        print(json.dumps(capture.compare(a, b), indent=2))
        return 0
    return 2


def main(argv=None):
    p = argparse.ArgumentParser(description="Payphone backends and the compatibility matrix")
    sub = p.add_subparsers(dest="command", required=True)

    m = sub.add_parser("matrix", help="show, check or write the compatibility matrix")
    m.add_argument("--check", action="store_true", help="only check config/compatibility.csv")
    m.add_argument("--write", action="store_true", help="write docs/compatibility.md")
    m.add_argument("--file", default=support.DEFAULT_MATRIX)
    m.set_defaults(func=cmd_matrix)

    sub.add_parser("backends", help="list the backends").set_defaults(func=cmd_backends)

    pr = sub.add_parser("protel", help="Protel configurations, ExpressNet errors and exports")
    pr.add_argument("action", choices=("check", "free-calling", "error", "import"))
    pr.add_argument("files", nargs="+")
    pr.add_argument("--firmware", help="firmware type or version, e.g. DD or CD8715-2412R")
    pr.add_argument("--model", help="board, e.g. 7000")
    pr.set_defaults(func=cmd_protel)

    el = sub.add_parser("elcotel", help="Elcotel R94, S94 and P94 files")
    el.add_argument("action", choices=("read", "check", "write"))
    el.add_argument("files", nargs="+")
    el.add_argument("--json", help="with read: write the JSON dump to this file")
    el.set_defaults(func=cmd_elcotel)

    c = sub.add_parser("capture", help="session captures for protocol research")
    c.add_argument("action", choices=("import", "check", "compare"))
    c.add_argument("files", nargs="+")
    c.set_defaults(func=cmd_capture)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
