# Elcotel payphones

mmumblephone reads, checks and writes the configuration files the Elcotel software sends to Series 5 phones. Native transfer to the phone is a separate milestone and is not implemented: load the files with the Elcotel software for now. Support is in the [compatibility matrix](compatibility.md).

| File | Contents | Support |
|---|---|---|
| `.R94` | Rate file: per-area-code defaults, price bands, NXX overrides, surcharges | Read, check and write. A file that is read and written back comes out byte for byte the same; this was tested on the one public sample. NPA group headers and NXX tables are kept as raw bytes. |
| `.S94` | Up to 50 speed dials, each with its own rates and LATA type | Read, check and write |
| `.P94` (+ `.P99`) | Up to 250 priority parsing patterns, each with macro, timer and rates. Records 51–250 go in the `.P99` file. | Read, check and write |

S94 and P94 support is written from the published description only. It has not been checked against a real file, and no Elcotel phone or Elcotel software has loaded a file written by mmumblephone yet.

```
mm_payphone.py elcotel read RATES.R94 --json rates.json    # dump to JSON
mm_payphone.py elcotel check rates.json                    # after editing
mm_payphone.py elcotel write rates.json NEW.R94            # checks, then writes
```

JSON dumps keep everything needed to write the file back, including the header and any bytes not understood. Rates are in nickels and times in minutes. 254 means unlimited time and 255 restricted. In R94 area code defaults, `"nxx"` means the exchange tables decide.

In P94 patterns, the digit 0 is stored as a zero byte, the same as the padding. So a pattern ending in 0 is shown without its trailing zeros, but `pattern_raw` keeps it, and the file is written back unchanged unless you edit `pattern`.

## Formats and licensing

The formats come from the description in [elcotel-playground](https://github.com/vekejsn/elcotel-playground)'s README. That project is GPL-3.0 and mmumblephone is MIT, so this is a separate implementation written from that description, and no code from that project was copied or adapted. File layouts are facts about the format, not code. The work is credited here all the same.

Two R94 details were worked out here from the public sample `208209BN.R94`, and have only been seen in that one file:

* header bytes 1–2 hold the expanded body length;
* body bytes 887, 888 and 889 are the counts of NPA groups, price bands and NXX tables. In that sample, 890 + 6 × groups + 4 × bands + 103 × tables equals the body length exactly.

Sample files, and reports of which files the Elcotel software and phones accept, would let the S94/P94 support move from description-only to checked against real files.
