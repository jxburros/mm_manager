# Compatibility matrix

<!-- Generated from config/compatibility.csv by `mm_payphone.py matrix --write`. Edit the CSV, not this file. -->

Each operation is **planned**, **experimental** (implemented and tested in software, not yet on a physical phone) or **hardware verified** (tested on the board, firmware and connection named in the details). Nothing is supported unless it is listed here.

| Family | Board | Firmware | Inventory | File editing | Communication | Programming | Reporting | Call/coin operation | Firmware updates |
|---|---|---|---|---|---|---|---|---|---|
| Nortel Millennium | Multi-Pay terminal | 1.20 (V1.0 Control PCP); 2.11 (V1.1 Control PCP) | experimental | hardware verified | hardware verified | hardware verified | hardware verified | n/a | n/a |
| Nortel Millennium | Multi-Pay terminal via SIP virtual modem | 1.20 (V1.0 Control PCP); 2.11 (V1.1 Control PCP) | experimental | experimental | experimental | experimental | experimental | n/a | n/a |
| Protel | 7000 | CD/DD | experimental | experimental | planned | planned | planned | n/a | planned |
| Protel | 1000, 2000, 4000, 8000 | CA/DA; CD/DD; BC | experimental | experimental | planned | planned | planned | n/a | planned |
| Protel ProNet | Elite, Ascension | any | planned | planned | planned | planned | planned | n/a | planned |
| Elcotel | Series 5 | any | planned | experimental | planned | planned | planned | n/a | planned |

## Nortel Millennium Multi-Pay terminal (1.20 (V1.0 Control PCP); 2.11 (V1.1 Control PCP))

| Operation | Status | Tested setup | Limitations | Evidence |
|---|---|---|---|---|
| Inventory | experimental | none | Phones are known by terminal ID, which can be changed or reused; the board revision, coin mechanism and power are not recorded yet | tables/<terminal ID>/phone.cfg and phone_status (README: several payphones) |
| File editing | hardware verified | Upstream mm_manager: Multi-Pay terminal, V1.0 Control PCP 1.20 and V1.1 Control PCP 2.11, physical Bell 212A modem | Other terminal types and firmware partly work (MM_MANAGER.md compatibility table); the browser table editor itself has software tests only | MM_MANAGER.md: Compatibility |
| Communication | hardware verified | Upstream mm_manager: as above, physical Bell 212A modem on a second line | The manager only answers; it never calls the terminal | MM_MANAGER.md: Compatibility |
| Programming | hardware verified | Upstream mm_manager: as above, physical Bell 212A modem | A download is complete when every table is acknowledged; tables are not read back. Picking up updates on call-in (-U) is tested against a simulated terminal only | MM_MANAGER.md: Compatibility; README: retries |
| Reporting | hardware verified | Upstream mm_manager: as above, physical Bell 212A modem |  | MM_MANAGER.md: Compatibility |
| Call/coin operation | n/a | none | Calls and coins are handled by the terminal on an ordinary line |  |
| Firmware updates | n/a | none | mm_manager does not change terminal firmware |  |

## Nortel Millennium Multi-Pay terminal via SIP virtual modem (1.20 (V1.0 Control PCP); 2.11 (V1.1 Control PCP))

| Operation | Status | Tested setup | Limitations | Evidence |
|---|---|---|---|---|
| Inventory | experimental | none | As for the physical modem | tables/<terminal ID>/phone.cfg and phone_status |
| File editing | experimental | none | The same tables as with a physical modem | tests/test_sip_vmodem.c |
| Communication | experimental | none | Not yet tested against a real terminal. Bell 212A only (no Bell 103), UDP/IPv4, one call at a time | tests/test_softmodem.c (spandsp V.22 interoperability; noise; echo; clock offset); tests/test_sip_vmodem.c |
| Programming | experimental | none | Not yet tested against a real terminal | tests/test_sip_vmodem.c |
| Reporting | experimental | none | Not yet tested against a real terminal | tests/test_sip_vmodem.c |
| Call/coin operation | n/a | none | Calls and coins are handled by the terminal |  |
| Firmware updates | n/a | none | mm_manager does not change terminal firmware |  |

## Protel 7000 (CD/DD)

| Operation | Status | Tested setup | Limitations | Evidence |
|---|---|---|---|---|
| Inventory | experimental | none | Reads site records from ExpressNet exports (sites.imp): phone number, description, group, costing and options record names. Decoded from one ExpressNet 1.44 sample; only the first record | payphones/protel/xnet_import.py; tests/test_payphones.py |
| File editing | experimental | none | mmumblephone's own JSON configuration (cost bands, area code/exchange bands, options), checked against the ExpressNet manual's limits. Not a format the phone or ExpressNet reads; option values are not encoded | payphones/protel/config.py; tests/test_payphones.py |
| Communication | planned | none | Protocol not public and not yet captured. Phone-initiated sessions first. 300 bps Bell 103 works with ordinary modems; the 1200 bps Protel mode needs a Protel modem until characterised | docs/protel.md |
| Programming | planned | none | Depends on communication | docs/protel.md |
| Reporting | planned | none | Flags, coin totals, call accounting and CDRs are documented as screens; their encoding is not known | docs/protel.md |
| Call/coin operation | n/a | none | Smart board: the phone rates and collects coins itself on an ordinary line |  |
| Firmware updates | planned | none | A later capability: ExpressNet downloads DD firmware to phones with a downloadable chip | docs/protel.md |

## Protel 1000, 2000, 4000, 8000 (CA/DA; CD/DD; BC)

| Operation | Status | Tested setup | Limitations | Evidence |
|---|---|---|---|---|
| Inventory | experimental | none | As for the 7000 | payphones/protel/xnet_import.py; tests/test_payphones.py |
| File editing | experimental | none | As for the 7000; options are checked against the firmware group the manual gives | payphones/protel/config.py; tests/test_payphones.py |
| Communication | planned | none | Each board and firmware is to be tested on its own before reusing the 7000's results | docs/protel.md |
| Programming | planned | none | Depends on communication | docs/protel.md |
| Reporting | planned | none | Depends on communication | docs/protel.md |
| Call/coin operation | n/a | none | Smart boards |  |
| Firmware updates | planned | none |  | docs/protel.md |

## Protel ProNet Elite, Ascension

| Operation | Status | Tested setup | Limitations | Evidence |
|---|---|---|---|---|
| Inventory | planned | none | A separate family until captures show compatibility with ExpressNet boards | docs/protel.md |
| File editing | planned | none |  | docs/protel.md |
| Communication | planned | none | The Elite guide specifies ProNet for computer communication | docs/protel.md |
| Programming | planned | none |  | docs/protel.md |
| Reporting | planned | none |  | docs/protel.md |
| Call/coin operation | n/a | none | Smart boards |  |
| Firmware updates | planned | none |  | docs/protel.md |

## Elcotel Series 5

| Operation | Status | Tested setup | Limitations | Evidence |
|---|---|---|---|---|
| Inventory | planned | none |  |  |
| File editing | experimental | none | R94 rate files: reads, checks and writes back byte for byte (one public sample); NPA groups and NXX tables are kept raw. S94 and P94/P99: written from the published description, not yet checked against a real file. No Elcotel software or phone has loaded a file written here | payphones/elcotel; tests/test_payphones.py |
| Communication | planned | none | A separate milestone after file support | docs/elcotel.md |
| Programming | planned | none | Load the files with the Elcotel software for now | docs/elcotel.md |
| Reporting | planned | none |  | docs/elcotel.md |
| Call/coin operation | n/a | none | Smart board |  |
| Firmware updates | planned | none |  | docs/elcotel.md |
