"""A Protel configuration: cost bands, area code/exchange assignments and options.

This is the small set the plan starts with: rates and free calling, keypad
(dialing) restrictions, incoming calls (band 88) and operating options. It
is stored as JSON, checked against the limits the ExpressNet manual gives,
and is what a future native backend would download. Option values are kept
as entered: how the phone encodes them is not known yet.

Example:

    {
      "firmware": "DD",
      "cost_bands": {
        "0":   {"initial_rate": 0.25, "initial_time": 0, "overtime_rate": 0.0, "overtime_period": 1, "keypad": 8},
        "88":  {"initial_rate": 0.0, "initial_time": 0, "overtime_rate": 0.0, "overtime_period": 1}
      },
      "npa_bands": {"212": 0},
      "nxx_bands": {"718": {"555": 0}},
      "options": {"127": "non-coin", "133D": 300},
      "modem": "bell103"
    }
"""

import json

from . import catalog

BAND_FIELDS = {
    # field: (default, description)
    "initial_rate": (0.0, "dollars the caller deposits before the call is processed (0 to 12.75, nickels)"),
    "initial_time": (0, "minutes granted for the initial rate (0 to 255, 0 = unlimited)"),
    "overtime_rate": (0.0, "dollars per overtime period (0 to 12.75, nickels)"),
    "overtime_period": (1, "minutes per overtime charge (0 to 15, 0 = calls in this band are denied)"),
    "keypad": (8, "digits accepted after answer (1 = none to 8 = all)"),
    "fraud": (0, "acoustic coupler anti-fraud (0 off, 1 on)"),
    "route": (0, "route (0 = dial as dialed, 1 to 15)"),
    "answer_detection": (0, "0 standard, 1 loop reversal, 3 disabled"),
    "lata": ("", "intra or inter (bands 0-79 and 110-111)"),
}

MAX_RATE = 12.75


class ConfigError(ValueError):
    pass


def _as_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _cents(value):
    return int(round(float(value) * 100))


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save(config, path):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2, sort_keys=True)
        f.write("\n")


def band(config, number):
    """One cost band with the defaults filled in."""
    raw = config.get("cost_bands", {}).get(str(number), {})
    return {k: raw.get(k, d) for k, (d, _) in BAND_FIELDS.items()}


def _check_band(n, b, coin_line, problems, losses, unsupported):
    for field in ("initial_rate", "overtime_rate"):
        cents = _cents(b[field])
        if cents < 0 or cents > _cents(MAX_RATE):
            problems.append("cost band %d: %s must be 0.00 to %.2f" % (n, field, MAX_RATE))
        elif cents % 5:
            losses.append("cost band %d: %s %.2f is rounded up to %.2f by the phone"
                          % (n, field, cents / 100, (cents + 5 - cents % 5) / 100))
    if not 0 <= int(b["initial_time"]) <= 255:
        problems.append("cost band %d: initial_time must be 0 to 255 minutes" % n)
    if not 0 <= int(b["overtime_period"]) <= 15:
        problems.append("cost band %d: overtime_period must be 0 to 15 minutes" % n)
    elif int(b["overtime_period"]) == 0:
        losses.append("cost band %d: overtime_period 0 denies every call in this band" % n)
    if int(b["keypad"]) not in catalog.KEYPAD_VALUES:
        problems.append("cost band %d: keypad must be 1 to 8" % n)
    if int(b["fraud"]) not in (0, 1):
        problems.append("cost band %d: fraud must be 0 or 1" % n)
    if not 0 <= int(b["route"]) <= 15:
        problems.append("cost band %d: route must be 0 to 15" % n)
    if int(b["answer_detection"]) not in catalog.ANSWER_DETECTION:
        problems.append("cost band %d: answer_detection must be 0, 1 or 3" % n)
    if b["lata"] not in ("", "intra", "inter"):
        problems.append("cost band %d: lata must be intra or inter" % n)
    elif b["lata"] and not (n in catalog.USER_BANDS or n in (110, 111)):
        unsupported.append("cost band %d: lata only applies to bands 0-79, 110 and 111" % n)
    if coin_line:
        if _cents(b["overtime_rate"]):
            problems.append("cost band %d: overtime_rate must be 0.00 on a coin line (option 127)" % n)
        if n == 88 and _cents(b["initial_rate"]):
            problems.append("cost band 88 (incoming) must not charge on a coin line")


def check(config, firmware=None, model=None):
    """Checks a configuration. Returns (problems, unsupported, losses), each a list of strings."""
    problems, unsupported, losses = [], [], []
    firmware = firmware or config.get("firmware") or ""
    if firmware and firmware not in catalog.FIRMWARE_TYPES:
        problems.append("firmware %r is not one of %s" % (firmware, ", ".join(catalog.FIRMWARE_TYPES)))

    options = {str(k): v for k, v in config.get("options", {}).items()}
    coin_line = str(options.get("127", "")).lower().startswith("coin")

    for key, raw in config.get("cost_bands", {}).items():
        try:
            n = int(key)
            catalog.band_kind(n)
        except ValueError:
            problems.append("cost band %r is not a number from 0 to %d" % (key, catalog.LAST_BAND))
            continue
        for field in raw:
            if field not in BAND_FIELDS:
                unsupported.append("cost band %d: unknown field %r" % (n, field))
        b = band(config, n)
        try:
            _check_band(n, b, coin_line, problems, losses, unsupported)
        except (TypeError, ValueError):
            problems.append("cost band %d: values must be numbers (lata: intra or inter)" % n)

    for npa, n in config.get("npa_bands", {}).items():
        if not (len(str(npa)) == 3 and str(npa).isdigit()):
            problems.append("npa_bands: %r is not an area code" % npa)
        if _as_int(n) not in catalog.USER_BANDS:
            problems.append("npa_bands: %s must use a band from 0 to 79" % npa)
    for npa, nxxs in config.get("nxx_bands", {}).items():
        if not (len(str(npa)) == 3 and str(npa).isdigit()):
            problems.append("nxx_bands: %r is not an area code" % npa)
        for nxx, n in nxxs.items():
            if not (len(str(nxx)) == 3 and str(nxx).isdigit()):
                problems.append("nxx_bands %s: %r is not an exchange" % (npa, nxx))
            if _as_int(n) not in catalog.USER_BANDS:
                problems.append("nxx_bands %s-%s must use a band from 0 to 79" % (npa, nxx))

    for option in options:
        if option not in catalog.OPTIONS:
            unsupported.append("option %s is not in the catalog" % option)
        elif firmware and not catalog.option_applies(option, firmware):
            unsupported.append("option %s (%s) does not apply to %s firmware"
                               % (option, catalog.OPTIONS[option][0], firmware))
    if "133D" in options and str(options["133D"]) not in ("300", "1200"):
        problems.append("option 133D must be 300 or 1200")
    modem = config.get("modem", "")
    if modem and modem not in catalog.MODEM_MODES:
        problems.append("modem %r is not one of %s" % (modem, ", ".join(catalog.MODEM_MODES)))
    elif modem == "protel1200" and model and model != "7000":
        unsupported.append("the Protel 1200 bps modem is 7000 series only")
    return problems, unsupported, losses


def free_calling(rate_bands=catalog.USER_BANDS):
    """A configuration where every listed band is free and untimed."""
    return {"cost_bands": {str(n): {"initial_rate": 0.0, "initial_time": 0, "overtime_rate": 0.0,
                                    "overtime_period": 1, "keypad": 8} for n in rate_bands}}
