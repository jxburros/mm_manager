"""What is publicly known about Protel payphones and ExpressNet.

Everything here comes from the published ExpressNet manuals and the hobbyist
write-ups listed in SOURCES; docs/protel.md discusses it. The wire protocol
between ExpressNet and the phone is not publicly documented. Nothing here
claims to describe it beyond what the manuals' error messages reveal.
"""

SOURCES = {
    "xnet_manual": "ExpressNet Reference Manual XNETRM001.0, 4/17/95, chapters 1-9 and appendices "
                   "(https://archive.org/details/protel-expressnet-v144)",
    "xnet_errors": "Protel application note: Communication Errors, ExpressNet and ExpressNet III "
                   "(https://archive.org/details/protel-expressnet-v144)",
    "el_jefe": "El Jefe, Programming Protel Payphones (https://philtel.org/assets/docs/programming_protel_payphones.pdf)",
    "telesfor_7000": "Programming Protel 7000: test of various consumer-grade modems, telesfor.org, 07/07/2023 "
                     "(https://telesfor.org/payphones/lib/exe/fetch.php?media=smart%3Aprotel%3Aprogramming_protel_7000.pdf)",
    "philtel_pt4": "Fixing a Protel Payphone Part 4: XNet configuration (https://philtel.org/2024/08/26/fixing-a-protel-pt4-xnet-config.html)",
    "philtel_pt5": "Fixing a Protel Payphone Part 5: the PBX and dial-in (https://philtel.org/2024/10/07/fixing-a-protel-pt5-dial-in-config.html)",
    "elite_guide": "Protel Elite Series user's guide "
                   "(https://payphones.telesfor.org/lib/exe/fetch.php?media=smart%3Aprotel%3Aprotel_elite_series_users_guide.pdf)",
}

# Families are kept apart until captures show they share a protocol.
FAMILIES = {
    "expressnet": "Protel boards managed with ExpressNet (XNet)",
    "pronet": "Protel ProNet-era boards (Elite, Ascension): the Elite guide specifies ProNet for computer communication",
}

MODELS = {
    "1000": dict(family="expressnet", notes="Protel 1000 Express"),
    "2000": dict(family="expressnet", notes=""),
    "4000": dict(family="expressnet", notes=""),
    "7000": dict(family="expressnet", notes="Built-in modem; a 1200 bps Protel-specific modulation needs a Protel modem "
                                             "(UPMS 1200); 300 bps Bell 103 works with ordinary modems"),
    "8000": dict(family="expressnet", notes="Discontinued; some run custom 26 firmware"),
    "1230XP": dict(family="expressnet", notes="Discontinued; compatibility with ExpressNet not confirmed here"),
    "Elite": dict(family="pronet", notes="Managed with ProNet according to its user's guide"),
    "Ascension": dict(family="pronet", notes="Assumed ProNet-era until shown otherwise"),
}

# Firmware ("SW type") as ExpressNet names it on the site record. The manuals
# tell CA/DA and CD/DD apart in many options; what the letters stand for is
# not stated, so it is not guessed at here.
FIRMWARE_TYPES = ("CA", "DA", "CD", "DD", "BC")

# Firmware groups the options are documented for.
FIRMWARE_GROUPS = {
    "CA/DA": ("CA", "DA"),
    "CD/DD": ("CD", "DD"),
    "BC": ("BC",),
}

# Modem modes the phones use to reach the computer.
MODEM_MODES = {
    "bell103": dict(bps=300, standard=True,
                    notes="Bell 103 FSK. Works with ordinary modems (force 300 bps, e.g. AT&N1 on USR, "
                          "+MS=B103 on Conexant). Calls placed by the payphone are the most reliable direction."),
    "bell212a": dict(bps=1200, standard=True,
                     notes="Option 133D sets 1200 or 300 baud on CD/DD firmware. Whether non-7000 boards use "
                           "standard Bell 212A at 1200 bps is unconfirmed."),
    "protel1200": dict(bps=1200, standard=False,
                       notes="7000 series only (option '1200 Baud FSK'). Protel-specific; sources disagree on "
                             "whether it is FSK or PSK. Needs a Protel modem until it is characterised from captures."),
}

# Serial settings ExpressNet uses towards its modem.
XNET_SERIAL = dict(data_bits=8, parity="N", stop_bits=2)

# Options & Registers. Numbers are those printed in the manual's chapter 5;
# "firmware" is the group the manual restricts the option to (None = all).
OPTIONS = {
    "111": ("Delay for central office dial tone", None),
    "112": ("Central office dial tone time-out", None),
    "113": ("DTMF or pulse dialing to CO", None),
    "114": ("Enable coin tones", None),
    "115": ("Pulse or DTMF 111 for anti-fraud", None),
    "116": ("Rate intra/interstate NPA-555 the same", None),
    "117": ("Allow # key receiver volume adjustment", None),
    "118": ("Enable bong tone on 0+ calls", None),
    "122": ("Coin collect time", None),
    "123": ("Enable initial time message", None),
    "124": ("Enable wink detect", None),
    "125": ("Enable credit card messages", None),
    "127": ("Non-coin line or coin line operation", "CD/DD"),
    "133A": ("Use modem chip or simulated modem", "CA/DA"),
    "133D": ("Set modem speed to 1200 or 300 baud", "CD/DD"),
    "135": ("Allow US coins", None),
    "136": ("Allow Canadian coins", None),
    "137": ("Open loop/continuous ground coin check", "CD/DD"),
    "141": ("High or low transmit level", "CD/DD"),
    "145": ("Allow 976 numbers", None),
    "146": ("Enable voice error messages", None),
    "148": ("-130 or +130 volt refund signal", "CD/DD"),
    "151": ("Allow 0+ separation by LATA", None),
    "153": ("Inter-digit delay with dial pulse", None),
    "154": ("Enable cellular applications", None),
    "158": ("Prompt 0 or SD #92 for emergency", "CA/DA"),
    "161": ("Bong or voice prompt on 0+ calls", None),
    "163": ("Enable Feature Group F", "CD/DD"),
    "164": ("Only mode of operation", None),
    "165": ("English or Spanish voice prompts", "CD/DD"),
    "166": ("Call restrictions by time of day", "CD/DD"),
    "167": ("Answer recognition", "CD/DD"),
    "171": ("Use coin tone level twist", "CD/DD"),
    "172": ("High coin tone level amplitude", "CD/DD"),
    "173": ("Allow non-coin calls when relay jammed", "CD/DD"),
    "174": ("Use reverse twist", "CD/DD"),
    "175": ("Disable piezo ringer (7000 series only)", None),
    "178": ("Free/restrict after limited overtime", "CD/DD"),
    "181": ("Park tip on ground", "CD/DD"),
    "184": ("Monitor loop reversal until normal", "CD/DD"),
    "185": ("Remove ground when collect/refund detected", "CD/DD"),
    "188": ("Disallow local or 1+ calls on loop reversal", "CD/DD"),
}

# Cost bands 0-79 are assigned to area codes and exchanges; 80-137 are fixed
# to call types (manual page 4-10).
USER_BANDS = range(0, 80)
SPECIAL_BANDS = {
    "service (N11)": range(80, 88),
    "incoming": range(88, 89),
    "speed dial": range(89, 101),
    "555-XXXX": range(101, 102),
    "NPA-555-XXXX": range(102, 103),
    "10XXX": range(103, 104),
    "1-800": range(104, 105),
    "PBX": range(105, 106),
    "free call": range(106, 107),
    "950-XXXX": range(107, 108),
    "0-": range(108, 109),
    "00-": range(109, 110),
    "0+7": range(110, 111),
    "0+10": range(111, 112),
    "international": range(112, 122),
    "credit card": range(122, 137),
    "*1 to *7": range(137, 138),
}
LAST_BAND = 137

# Keypad control (digits accepted after answer), manual page 4-15.
KEYPAD_VALUES = {
    1: "no digits accepted",
    2: "NXXXXXX...",
    3: "NXXXXXX... or 0YXXXXX..., 45 s initial, 6 s inter-digit",
    4: "NXXXXXX... or 0YXXXXX..., 45 s initial, 6 s inter-digit",
    5: "XXXXXXX..., 45 s initial, 60 s inter-digit",
    6: "XXXXXXX..., 45 s initial, 60 s inter-digit",
    7: "XXXXXXX..., 3 min initial, 3 min inter-digit",
    8: "XXXXXXX..., unlimited initial, 4.25 min inter-digit",
}

# Third digit of the card/SAC/AD/CI field.
ANSWER_DETECTION = {0: "standard", 1: "loop reversal", 3: "disabled"}

# Status flags the phone reports (manual appendix D). Partial: only the codes
# that are legible in the scanned appendix are listed. It also describes box
# removed, coin totals zeroed, coin inactivity, dial inactive and loop
# reversed flags whose two-letter codes could not be read reliably.
FLAGS = {
    "BM": "Box missing for at least 15 minutes",
    "CG": "Coin gate failure (electronic coin scanner)",
    "CR": "Clock reset by a power-on reset",
    "E$": "Cash box emptied (*#1 reported)",
    "EE": "EEPROM error: program in EEPROM corrupted",
    "HG": "Handset gone (receiver resistance wrong)",
    "HO": "Handset off hook about 15 minutes with no activity",
    "LA": "Lower alarm: coin box door removed",
    "LB": "Low battery (NiCad charging)",
    "ME": "Coin mech error",
    "NE": "Audit CDR error",
    "OC": "Opto-coupler: collect/refund hardware failure",
}

# Session phases, in the order the application note lists their errors. The
# order is taken as the order of a poll; that is an inference, to be checked
# against captures.
PHASES = (
    ("database", "ExpressNet's own database or files (no phone traffic)"),
    ("connect", "Dialing and modem connection"),
    ("status_string", "Sync character and status string, with checksum"),
    ("identify", "Phone number (ANI) and phone type checks"),
    ("security", "Four-digit security code"),
    ("flags", "Clearing status flags"),
    ("version", "Reading the software version"),
    ("escrow", "Escrow relay check"),
    ("full_download", "Full parameter download in blocks, verify, RAM error flag reset"),
    ("partial_download", "Partial parameter download in blocks, new checksum, verify"),
    ("coin", "Coin flag and cash box"),
    ("checksum", "Phone checksum against the file checksum"),
    ("cdr", "Billing and audit CDR counts, records and clearing"),
    ("firmware", "Unlocking, writing and verifying the downloadable EEPROM"),
    ("call_accounting", "Call accounting read and reset"),
    ("clock", "Setting the clock"),
    ("options_7000", "7000 options read and write"),
)

# ExpressNet communication error codes: (description, phase). Codes 82-99
# exist but were not legible in the available copy.
COMM_ERRORS = {
    1: ("No site record in database", "database"),
    2: ("Build rate parameter failed", "database"),
    3: ("Database timed out", "database"),
    4: ("Time-out on carrier detect", "connect"),
    5: ("Busy signal detected", "connect"),
    6: ("No carrier signal detected", "connect"),
    7: ("Carrier detected but no connection established", "connect"),
    8: ("Sync character not detected", "status_string"),
    9: ("Status string received by the phone is not complete", "status_string"),
    10: ("Status string checksum is bad", "status_string"),
    11: ("Phone number mismatch", "identify"),
    12: ("Phone type mismatch", "identify"),
    13: ("Security code command time-out", "security"),
    14: ("Security code failed", "security"),
    15: ("Clear flags code sent, but timed out", "flags"),
    16: ("Clear flags command failed", "flags"),
    17: ("Failed software version memory read", "version"),
    18: ("Failed extended software version", "version"),
    19: ("Escrow relay check failure", "escrow"),
    20: ("Full download failure, time-out", "full_download"),
    21: ("Full download failure", "full_download"),
    22: ("Full download verify, time-out", "full_download"),
    23: ("Full download failure, verify (not used)", "full_download"),
    24: ("Full download flag reset time-out (not used)", "full_download"),
    25: ("Full download time-out: verify failed", "full_download"),
    26: ("Full download flag reset failed", "full_download"),
    27: ("Partial download time-out setting the new checksum", "partial_download"),
    28: ("Partial download failure", "partial_download"),
    29: ("Partial download time-out setting the new checksum", "partial_download"),
    30: ("Partial download failure: verify failed", "partial_download"),
    31: ("Partial download verify time-out", "partial_download"),
    32: ("Partial download verify failure", "partial_download"),
    33: ("Failed clear coin flag command", "coin"),
    34: ("Reset cash box volume failure (CA/CD only)", "coin"),
    35: ("Phone and file checksums differ", "checksum"),
    36: ("Incompatible firmware", "checksum"),
    37: ("Get CDR counts time-out", "cdr"),
    38: ("Billing CDR count error", "cdr"),
    39: ("Audit CDR count error", "cdr"),
    40: ("Get billing CDR record failure", "cdr"),
    41: ("Temp billing record save failure", "database"),
    42: ("Checksum error in billing record", "cdr"),
    43: ("Billing CDR not saved to file", "database"),
    44: ("Error opening billing CDR file", "database"),
    45: ("Bad billing CDR saved to file", "cdr"),
    46: ("Failed to clear billing records", "cdr"),
    47: ("Failed to clear billing flags", "cdr"),
    48: ("Get audit CDR record failure", "cdr"),
    49: ("Temp audit record save failed", "database"),
    50: ("Checksum error in audit record", "cdr"),
    51: ("Audit CDR not saved to file", "database"),
    52: ("Error opening audit CDR file", "database"),
    53: ("Failed to clear audit records", "cdr"),
    54: ("Failed to clear audit flags", "cdr"),
    55: ("No EEPROM blocks found", "firmware"),
    56: ("Error opening software file", "database"),
    57: ("Unlock EEPROM command time-out", "firmware"),
    58: ("Unlock EEPROM command failure", "firmware"),
    59: ("Disable EEPROM command time-out", "firmware"),
    60: ("Disable EEPROM command failure", "firmware"),
    61: ("Failure to save coin record", "database"),
    62: ("Software file not found (not used)", "database"),
    63: ("Software file download time-out", "firmware"),
    64: ("Verify software failure", "firmware"),
    65: ("Software download command failure", "firmware"),
    66: ("Check call accounting errors failure", "call_accounting"),
    67: ("Get call accounting failure", "call_accounting"),
    68: ("Failed to save call accounting", "database"),
    69: ("Reset call accounting time-out", "call_accounting"),
    70: ("Failed to save new call accounting", "database"),
    71: ("Reset call accounting command failed", "call_accounting"),
    72: ("Reset clock failure, time-out", "clock"),
    73: ("Reset clock failure", "clock"),
    74: ("Software file not assigned (not used)", "database"),
    75: ("Full download complete, CA/DA watchdog time-out", "full_download"),
    76: ("Full download incomplete, CA/DA watchdog time-out", "full_download"),
    77: ("Failed to save speed dials", "database"),
    78: ("Database read failure (7000 options)", "database"),
    79: ("Database write failure (7000 options)", "database"),
    80: ("Communication fails (7000 options)", "options_7000"),
}

# Codes that mean the phone accepted the data but the session was cut short.
ACKNOWLEDGED_BUT_UNVERIFIED = {75}


def phase_title(phase):
    for name, title in PHASES:
        if name == phase:
            return title
    return phase


def explain_error(code):
    """Description, phase and whether the phone was reached, for one ExpressNet error code."""
    code = int(code)
    if code not in COMM_ERRORS:
        return dict(code=code, description="unknown error code", phase=None, reached_phone=None)
    description, phase = COMM_ERRORS[code]
    # Errors in ExpressNet's own files say nothing about whether the phone was reached.
    reached = None if phase == "database" else phase != "connect"
    return dict(code=code, description=description, phase=phase, phase_title=phase_title(phase),
                reached_phone=reached)


def option_applies(option, firmware):
    """True if an option applies to this firmware type (CA, DA, CD, DD, BC)."""
    name, group = OPTIONS[option]
    if group is None:
        return True
    return firmware in FIRMWARE_GROUPS[group]


def band_kind(band):
    if band in USER_BANDS:
        return "area/exchange"
    for kind, r in SPECIAL_BANDS.items():
        if band in r:
            return kind
    raise ValueError("cost band %d is outside 0-%d" % (band, LAST_BAND))
