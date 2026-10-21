# Protel payphones

This page collects what is publicly known about managing Protel payphones, what mmumblephone does with it today, and the route to a native backend (phase 4 of `next-plan.md`). Support for each board is in the [compatibility matrix](compatibility.md).

**Status:** no Protel phone has been programmed or polled by mmumblephone. The wire protocol between ExpressNet and the phone is not publicly documented, and no captures of it have been made yet. What exists is the groundwork that doesn't need a phone: the configuration model, ExpressNet export import, error diagnosis and capture tools.

## What works now

All of it is in `mm_payphone.py` (Python 3, standard library only) and the `payphones/protel` package.

| Command | What it does |
|---|---|
| `mm_payphone.py protel check CONFIG.json [--firmware DD] [--model 7000]` | Checks a configuration against the limits in the ExpressNet manual. It lists errors, settings that won't be applied to that firmware or board, and values the phone would change, such as a rate rounded up to the next nickel. |
| `mm_payphone.py protel free-calling OUT.json` | Writes a configuration where cost bands 0–79 are free and untimed. |
| `mm_payphone.py protel error CODE...` | Explains an ExpressNet communication error code (1–80): what failed, and in which phase of the session. |
| `mm_payphone.py protel import FILE.imp...` | Reads ExpressNet export files and lists the phones in them as inventory entries: phone number, description, group, costing and options record. |
| `mm_payphone.py capture import/check/compare` | Records sessions for protocol research, and compares two of them (see [Captures](#captures)). |

### The configuration

The configuration covers the plan's first set: rates and free calling, keypad (dialing) restrictions, incoming calls and operating options. It is a JSON file in mmumblephone's own format. Neither the phone nor ExpressNet reads it.

```json
{
  "firmware": "DD",
  "modem": "bell103",
  "cost_bands": {
    "0":  {"initial_rate": 0.25, "initial_time": 0, "overtime_rate": 0.0, "overtime_period": 1, "keypad": 8},
    "1":  {"initial_rate": 1.00, "initial_time": 5, "overtime_rate": 1.00, "overtime_period": 5, "lata": "inter"},
    "88": {"initial_rate": 0.0, "initial_time": 0, "overtime_rate": 0.0, "overtime_period": 1}
  },
  "npa_bands": {"212": 1},
  "nxx_bands": {"718": {"555": 0}},
  "options": {"127": "non-coin", "133D": 300}
}
```

Each cost band has these fields. The limits are from the manual, pages 4-14 to 4-16.

| Field | Values |
|---|---|
| `initial_rate`, `overtime_rate` | $0.00–$12.75. The phone rounds a rate up to the next nickel. On a coin line (option 127), `overtime_rate` must be 0. |
| `initial_time` | 0–255 minutes. 0 means unlimited. |
| `overtime_period` | 0–15 minutes. 0 denies every call in the band. |
| `keypad` | 1 (no digits accepted after answer) to 8 (any digits, no initial time-out) |
| `fraud` | 0 or 1: the acoustic coupler anti-fraud feature |
| `route` | 0 dials the number as dialed; 1–15 use a configured route |
| `answer_detection` | 0 standard, 1 loop reversal, 3 disabled |
| `lata` | `intra` or `inter`, for bands 0–79, 110 and 111 |

Bands 0–79 are assigned to area codes (`npa_bands`) or to single exchanges (`nxx_bands`). Bands 80–137 are fixed to call types: 80–87 service (N11) calls, 88 incoming, 89–100 speed dial, 101 555-XXXX, 102 NPA-555-XXXX, 103 10XXX, 104 1-800, 105 PBX, 106 free call, 107 950-XXXX, 108 0-, 109 00-, 110 0+7, 111 0+10, 112–121 international, 122–136 credit card, and 137 \*1 to \*7.

Options are given by the numbers the manual prints, such as 127 (coin or non-coin line), 133D (modem speed) and 167 (answer recognition). The full list is `OPTIONS` in `payphones/protel/catalog.py`. An option that the manual limits to CA/DA or CD/DD firmware is reported as not applied on other firmware. Option values are kept as you enter them: how the phone encodes them is unknown until the protocol is.

### ExpressNet exports

ExpressNet's *File Utilities → Export* writes `sites.imp`, `miscopt.imp`, `callcost.imp` and others. Their layout is not documented. The reader decodes the first record's name fields, using the layout of the one public set of exports (ExpressNet 1.44). It rejects files that differ from that sample, and warns that only the first record is read. More exports, especially ones holding several sites, would let it read them all, and then the cost bands and options too.

## What is known

### Boards, firmware and families

| Model | Notes |
|---|---|
| 1000 (Express), 2000, 4000 | ExpressNet |
| 7000 | ExpressNet. Has a built-in modem with a Protel-specific 1200 bps mode. |
| 8000 | ExpressNet. Discontinued; some run "custom 26" firmware. |
| 1230XP | Discontinued; whether ExpressNet manages it is not confirmed. |
| Elite, Ascension | **Separate family.** The Elite guide specifies ProNet for computer communication. They are kept apart until captures show what they share. |

ExpressNet records a firmware type for each site: CA, DA, CD, DD or BC. The full version looks like `CD8715-2412R`. Many options only apply to CA/DA or to CD/DD firmware. The manuals don't say what the letters stand for. DD phones with a downloadable chip can be sent new firmware (`Dd*.231` files in ExpressNet's `param\software` folder).

### Modems

* **300 bps Bell 103** works with ordinary modems. Force the speed, for example with `&N1` on US Robotics modems or `+MS=B103` on Conexant modems. ExpressNet's serial settings are 8 data bits, no parity, 2 stop bits.
* **1200 bps**: option 133D selects 1200 or 300 baud on CD/DD firmware. The 7000 also has a "1200 Baud FSK" option, and that mode needs Protel's UPMS 1200 modem. Sources disagree on whether it is FSK or PSK. It needs characterising from audio captures before a software modem can be written. mmumblephone's Bell 212A modem may suit other boards at 1200 bps, but that is unconfirmed.
* **Direction matters.** In telesfor.org's systematic tests (USR 5686E, USR 5630G and a USR ISA modem against a 7000), calls placed by the payphone (`*#3` in service mode) always worked at 300 bps. Calls to the payphone often failed after a few blocks. Hence the plan: phone-initiated sessions first.
* **VoIP**: two ATAs (Grandstream HT502, Cisco SPA112) failed at 300 bps on a LAN. A T1 channel bank on Asterisk works (PhilTel's community programming line). mmumblephone's SIP soft modem would remove the ATA from the path. That only applies once it supports Bell 103.

### A session, from ExpressNet's error messages

ExpressNet's *Communication Errors* application note lists 80 error codes, in roughly the order a poll runs. Read together, they outline a session. This order is an inference: captures have to confirm it.

1. **Connect:** dial, carrier, CONNECT (errors 4–7).
2. **Status string:** a sync character, then a status string with a checksum, which the phone checks (8–10).
3. **Identify:** the phone's ANI and phone type are checked against the site record (11, 12).
4. **Security code:** a four-digit code. The phone hangs up at once if it is wrong (13, 14).
5. **Flags:** status flags are cleared (15, 16).
6. **Version:** the software version is read, and an extended version too during a firmware download (17, 18).
7. **Escrow relay check** (19).
8. **Parameter download**, full or partial. Data goes in blocks, each acknowledged. Then a verify step. A full download ends with a RAM-error flag reset; a partial one sets a new checksum (20–32). On CA/DA phones the call may be ended early to avoid a watchdog time-out (75, 76).
9. **Coin flag and cash box** (33, 34).
10. **Checksum:** the phone's checksum is compared with the file's (35, 36).
11. **CDRs:** billing and audit CDR counts, records with checksums, then clearing (37–54).
12. **Firmware:** unlock the downloadable EEPROM, write blocks, verify, disable (55–65).
13. **Call accounting:** read and reset (66–71).
14. **Clock:** set (72, 73).
15. **7000 options:** read and write (80).

So a successful ExpressNet download is a phone **acknowledgement**, plus the phone's own verify and checksum step. It is not a read-back of the settings. `ProtelBackend.session_result()` records it as `acknowledged`.

### Reports

The phone reports status flags (manual appendix D). Among them are BM (box missing), CG (coin gate failure), CR (clock reset), E$ (cash box emptied), EE (EEPROM error), HG (handset gone), HO (handset off hook), LA (lower alarm), LB (low battery), ME (coin mech error), NE (audit CDR error) and OC (opto-coupler). It also reports coin totals, call accounting by call type, and billing and audit CDRs.

## The route to a native backend

1. **Reference setup.** Use a 7000 (the best-documented board) on an analog line simulator or a channel bank. On the computer side, ExpressNet 1.55 runs in DOS (or VirtualBox) with a USR 5630G modem forced to 300 bps. Record the line audio, and the serial traffic between ExpressNet and the modem: a serial tap, or a null-modem pair through a logging program.
2. **Phone-initiated captures.** Place calls from the phone with `*#3`, changing one setting per capture (one cost band rate, one option), and keep the "polling only" sessions apart from download sessions. Import each with `mm_payphone.py capture import TRANSCRIPT OUT.json board=7000 firmware=... connection=... modem_mode=bell103 direction=phone_to_computer change="band 0 initial rate 0.25 -> 0.35" outcome=complete`. `capture compare` then shows where the two byte streams first differ.
3. **Framing.** From the captures, find the sync character, the status string and its checksum, the block structure, acknowledgements and retries. Use the session outline above as the map.
4. **Answer one phone.** With the mmumblephone side answering instead of ExpressNet (a physical modem first), reproduce a polling session, then a download of one cost band. Verify it from the phone's behavior: the rate it asks for, or `*#6` for the time of the last download. The milestone is reached when this is repeatable and documented on one board.
5. **Then:** reports (flags, coin totals, CDRs), outgoing polling, the Protel 1200 bps modem from audio captures, other boards one at a time, and firmware download last.

Until then, the security code, ANI and phone type handling stay unimplemented. mmumblephone will only program phones its user owns and has the codes for.

## Sources

* ExpressNet Reference Manual XNETRM001.0 (4/17/95), chapters 1–9 and appendices; *Communication Errors* application note; NANP conversion guides; ExpressNet 1.44 software and sample exports: [Internet Archive: Protel Expressnet v1.44](https://archive.org/details/protel-expressnet-v144)
* El Jefe, [Programming Protel Payphones](https://philtel.org/assets/docs/programming_protel_payphones.pdf)
* [Programming Protel 7000: test of various consumer-grade modems](https://telesfor.org/payphones/lib/exe/fetch.php?media=smart%3Aprotel%3Aprogramming_protel_7000.pdf), telesfor.org, 2023
* PhilTel, Fixing a Protel Payphone, [part 4: XNet configuration](https://philtel.org/2024/08/26/fixing-a-protel-pt4-xnet-config.html) and [part 5: the PBX and dial-in](https://philtel.org/2024/10/07/fixing-a-protel-pt5-dial-in-config.html)
* [Protel Elite Series user's guide](https://payphones.telesfor.org/lib/exe/fetch.php?media=smart%3Aprotel%3Aprotel_elite_series_users_guide.pdf)
