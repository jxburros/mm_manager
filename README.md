# mmumblephone

### A SIP-enabled fork of [mm_manager](https://github.com/hharte/mm_manager), the Millennium Manager by Howard M. Harte

> **mmumblephone is a fork of [mm_manager](https://github.com/hharte/mm_manager), and is built entirely on top of it.**
> All of the Millennium Manager itself is the work of Howard M. Harte and the mm_manager contributors: the reverse-engineered terminal protocol, table generation and download, provisioning, call rating, CDR, alarm and maintenance reporting, and every utility in this repository. That work made this fork possible.
>
> This fork adds one main feature on top: a **SIP client with a software ("virtual") modem**. A Millennium payphone can then call the Manager over VoIP without a physical modem or a second analog phone line. It also adds a browser control panel, and tools for [several payphones](#several-payphones-updates-and-call-ins): per-phone settings, saved configurations, retried updates and call-in schedules.
>
> For anything about mm_manager itself, please go to the [upstream project](https://github.com/hharte/mm_manager). Please report problems with the code this fork adds here, not upstream.

## What this fork adds

The Millennium terminal calls its Manager with a 1200 bps Bell 212A modem. Upstream mm_manager answers that call with a physical modem on a second phone line. mmumblephone can instead register to a SIP server (Asterisk, FreePBX, a VoIP provider, etc.) as an extension or SIP number. When the payphone dials that number, mmumblephone answers the SIP call and runs a **software Bell 212A modem** on the call's G.711 audio. The rest of mm_manager sees an ordinary Hayes-style modem.

```
 Millennium ──analog──► ATA (FXS) ──SIP/RTP──► PBX / SIP provider ──SIP/RTP──► mmumblephone
  terminal               G.711, no VAD/EC                                     (SIP UA + soft modem
                                                                               + mm_manager)
```

The design borrows from software fax over IP (spandsp, iaxmodem, t38modem):

* **A soft modem on PCM.** Like a soft fax modem, the Bell 212A modem works directly on 8 kHz G.711 samples. The receiver recovers symbol timing from the signal, and differential PSK detection removes the need for carrier phase recovery. It therefore copes with the clock drift and jitter of a VoIP path without a real-time clock of its own.
* **A virtual tty.** Like iaxmodem, the virtual modem emulates the AT command set, RING / CONNECT / NO CARRIER result codes, DCD, and DTR hang-up. mm_manager's existing modem and protocol code runs unchanged.
* **Voice-band data (VBD) conditioning.** As with G.711 fax pass-through, the SDP offers only PCMU/PCMA. It also asks the far end to treat the call as modem data (`a=gpmd:<pt> vbd=yes`, ITU-T V.152) with silence suppression off. Gateways that honor this disable VAD, comfort noise and adaptive jitter buffering for the call.
* **Robust media.** RTP is symmetric, so it latches to wherever the far end's audio comes from (NAT). A small re-ordering buffer fixes out-of-order packets, and lost packets are filled with silence rather than stalling the modem. If audio stops entirely, carrier loss is still detected.

### What's included

| File | Purpose |
|---|---|
| `src/mm_softmodem.c` | Software Bell 212A modem (answer and originate, 1200 bps DPSK, 1+x⁻¹⁴+x⁻¹⁷ scrambler, async framing) and G.711 codecs |
| `src/mm_sip.c` | Minimal SIP user agent: REGISTER with MD5 digest auth and NAT discovery, INVITE/ACK/BYE/CANCEL/OPTIONS/UPDATE, RTP |
| `src/mm_vmodem.c` | Virtual modem: AT command emulation, auto-answer, 20 ms media clock, glue between SIP and the soft modem |
| `src/mm_osal.c` | Small sockets/threads/time portability layer (Linux, macOS, Windows) |
| `src/mm_detect.c` | Serial modem detection: enumerates serial ports and probes them with AT commands (`-D`, `-f auto`) |
| `src/mm_gui.c` | Control panel: serves the table editor on `127.0.0.1` and lets it start and stop mm_manager, browse its database and run the tools, so no terminal is needed |
| `html/table_editor.html` | Browser-based editor for the configuration tables, the mm_manager command line and the control ROM version list |
| `tests/` | Modem loopback tests (noise, echo, clock offset, A-law), an end-to-end SIP test and, on Linux, macOS and the BSDs, a modem detection test that uses pseudo-terminals (`ctest`) |

## The control panel: no terminal needed

Download it from the [releases page](https://github.com/jxburros/mm_manager/releases), under **Assets**:

* **Windows:** `mmumblephone-<version>-win64.exe` is an installer. Run it, then start **mmumblephone control panel** from the Start menu or the desktop. Windows may say "Windows protected your PC", because the installer isn't signed: choose *More info*, then *Run anyway*. (`…-win64.zip` is the same thing to unpack and run from any folder, without installing.)
* **macOS and Linux:** unpack the `.tar.gz` and open `bin/mm_gui`. On a Mac, right-click it and choose *Open* the first time, because it isn't signed.

Start **`mm_gui`** and it opens the table editor in your web browser, with everything mm_manager needs on buttons:

A rail on the left has four places:

* **phones**: every payphone, by its terminal ID. Pick one for its *overview* (name, ID, how often it calls in, whether its last update went through, saved configurations), its settings grouped by what they do (display & messages, features & hours, prices, free/blocked/speed-dial numbers, local & long distance, cards & carriers, coins & cash box, advanced), and its *records*. Each setting is marked *shared* or *this phone only*: change a shared one here and the phone gets its own copy, which *go back to the shared version* removes again. Tick two or more phones and press *edit together* to change the same fields on all of them, or their call-in schedule, while each keeps its other settings. See [several payphones](#several-payphones-updates-and-call-ins).
* **global**: the same setting groups for every phone, with a switch to give one model (coin, card-only, desk…) its own version, and a list of which phones get each file. Then mm_manager's own options (connection, with a serial modem scan; numbers & codes; calling in & updates; sending tables; logs), the control ROM list (`config/control_rom_versions.csv`), saved configurations, the Python table generators, the working folder (download, upload, delete, turn a `.dlog` transcript into a `.pcap`), every table file folder by folder, and *quit the control panel*.
* **manager**: start and stop mm_manager and watch its output, and the records from every phone, read from `mm_manager.db`. It keeps running when you close the page; the button at the top shows whether it is running.
* **help**: the guide.

The welcome screen, tour and help tab cover all of this.

`mm_gui` finds its files by itself: the working folder (the one with `tables/` and `config/`, where mm_manager runs and keeps `mm_manager.db`) is the current folder, `../share/mm_manager` in a release, or the source checkout. If that folder can't be written to, as when installed in Program Files, `mm_gui` works in `Documents\mmumblephone` (`~/mmumblephone` on macOS and Linux) instead, starting from a copy of the installed tables and config. Uninstalling or upgrading leaves that folder alone. mm_manager and the other tools must be next to `mm_gui`. Options:

| Option | Default | Description |
|---|---|---|
| `-p <port>` | 8273 | Local port. If it is busy, the next free one is used. |
| `-w <folder>` | found automatically | Working folder |
| `-H <file>` | found automatically | The `table_editor.html` to serve |
| `-n` | | Don't open a browser |

The control panel only listens on `127.0.0.1`. Each API request must carry a random token that is put in the page when it is served, and must be addressed to `127.0.0.1` or `localhost`, so other web sites can't use it. It only runs mm_manager, the `mm_*` tools next to it and the `generate_*.py` scripts, and only reads and writes files inside the working folder. The SIP password is passed to mm_manager in `MM_SIP_PASSWORD` and isn't saved. On Windows, *stop* ends mm_manager at once, rather than with ^C.

## Using the SIP virtual modem

Use `-S` in place of `-m -f <modem device>`:

```
# Register as extension 1000 on a PBX, and have the payphone dial 1000:
mm_manager -S 1000:secret@pbx.example.com -n 1000

# Keep the password off the command line:
MM_SIP_PASSWORD=secret mm_manager -S 1000@pbx.example.com:5060 -n 1000

# No registration: accept direct SIP calls (e.g. an ATA configured to dial sip:mm_manager@<ip>:5062)
mm_manager -S "listen;port=5062" -n 5551212
```

The `-n` NCC number(s) must be the number the payphone dials to reach the SIP number mmumblephone is registered on.

`-S` accepts `user[:password]@registrar[:port]` or `listen`, followed by optional `;option=value` settings:

| Option | Default | Description |
|---|---|---|
| `port` | 5060 | Local SIP UDP port |
| `rtp_port` | 10000 | Local RTP UDP port (the next free even port is used) |
| `expires` | 300 | Registration expiry, in seconds |
| `auth_user` | user | Authentication user name, if different from the SIP user |
| `domain` | registrar | SIP domain used in From/To/Request-URI |
| `local_ip` | auto | IP address to advertise in Contact and SDP |
| `codec` | `pcmu` | Preferred codec: `pcmu` (µ-law) or `pcma` (A-law) |
| `answer_tone` | 2225 | Answer tone: 2225 Hz (Bell) or 2100 Hz (V.25) |

Example: `-S "1000:secret@192.168.1.10;port=5070;rtp_port=12000;codec=pcmu"`

The modem init string (`-i`) is still accepted. The virtual modem answers after `S0` rings (default 1), hangs up when DTR drops, and ignores other settings. `-v` prints extra detail, and `-vv` also prints SIP messages.

## Finding a serial modem automatically

If the payphone calls in through a real modem (or a USB modem / serial-attached ATA), mm_manager can find it for you, so you don't need to know its device name or baud rate:

```
# Scan the serial ports for modems, list them, and exit:
mm_manager -D

# Use the first modem found:
mm_manager -m -f auto -n 5551212
```

Each serial port is probed with Hayes AT commands (`ATQ0V1E1`, then `ATI3` for an identification string). The rate given with `-b` is tried first, then 19200, 9600, 38400, 57600, 115200, 2400 and 1200 bps. A port counts as a modem only if it answers `OK`, so an idle port, a GPS receiver or a loopback plug won't be picked. With `-m`, leaving out `-f` is the same as `-f auto`.

Ports are enumerated from `/dev` (`ttyS*`, `ttyUSB*`, `ttyACM*`, `ttyAMA*`, `rfcomm*` on Linux, `cu.*` on macOS, `cua*` on the BSDs) or the `COM` ports on Windows. On Linux, you may need to be in the `dialout` group to open them. The scan sends AT commands to every port it can open, so stop any program that is using a modem first.

A Millennium terminal can't be detected this way, because it never answers calls and only calls the Manager. What is detected is the modem it will call through.

### ATA / PBX configuration

These are the same settings that make fax and modem calls work over VoIP:

* Use **G.711 µ-law (PCMU)**, or A-law where that is the norm. No compressed codecs.
* **Disable VAD / silence suppression, comfort noise and echo cancellation** on the ATA port the payphone uses.
* Use a **fixed jitter buffer** (e.g. 60–100 ms) rather than an adaptive one. Prefer a LAN path between the ATA and mmumblephone.
* The ATA must provide **polarity reversal on answer** (answer supervision), as with any Millennium installation.
* Disable T.38 / fax re-INVITE on the line. Bell 212A is not fax, and mmumblephone only handles G.711.
* On Asterisk/FreePBX, make sure the call is not transcoded, and that `directmedia`/re-INVITEs do not break the audio path.

### Status and limitations

* The software modem is tested against an independent implementation: spandsp's V.22 modem at 1200 bps, which uses the same modulation and scrambler. It exchanges data error-free with it in both directions. It also passes loopback tests with noise, echo and ±200 ppm clock offset. **It has not yet been tested against a real Millennium terminal**. Please report results, good or bad.
* Bell 212A only (1200 bps). Bell 103 (300 bps) fallback is not implemented.
* UDP, IPv4, one call at a time. TLS/SRTP and SIP over TCP are not supported.
* The Manager only answers calls, as in upstream mm_manager. The Manager never calls the payphone.

## Several payphones, updates and call-ins

One mm_manager serves any number of payphones. It tells them apart by the **terminal ID**, the 10-digit number keyed into each payphone when it is installed (normally the payphone's own phone number), which the terminal sends with every message. Each payphone has its own folder, `tables/<terminal ID>/`:

| File | Written by | Purpose |
|---|---|---|
| `mm_table_XX.bin` | you | Tables only this payphone gets. For any other table, mm_manager uses the model folder (`coin/`, `desk/`, ...), then `default/`. |
| `phone.cfg` | you, or the control panel | This payphone's settings, one `key=value` per line: `name`, `callin_interval` (minutes between call-ins, 5 to 143999), `callin_retry` (minutes before it retries a failed call-in, 1 to 59), `cdr_threshold` (call in once it holds this many call records, 1 to 50). |
| `phone_status` | mm_manager | What mm_manager last saw: `terminal_type`, `last_call`, `last_update` (`complete`, or why it failed). |
| `update_pending` | mm_manager, or you | If present, every table is sent the next time this payphone calls in. mm_manager creates it when a download fails and removes it after a complete one. |
| `table_update.log` | mm_manager | The time of each complete download. |

The **phones** tab of the control panel manages these folders. It lists every payphone that has called in, and you can add one before it does. Changing a payphone's terminal ID renames its folder and moves its records in `mm_manager.db` to the new ID. Do this when the ID is changed on the payphone itself.

**Saved configurations** are kept in `configurations/<name>/`: a payphone's own tables and its call-in settings. *Save as a configuration* on one payphone, then *apply* it to others. Applying copies the tables into each payphone's folder, replacing the tables and settings it had (untick *replace* to keep them), and marks it to get every table at its next call.

**Retries.** If the payphone doesn't acknowledge a table, mm_manager sends it again, up to 2 times (`-R <n>`, 0 to 10), before going on to the next one. A download is only recorded as complete when every table was acknowledged. If one wasn't, or the call dropped, mm_manager creates `update_pending`, so everything is sent again at the payphone's next call.

**Calling in.** The manager never calls a payphone, but it tells each payphone when to call in. That schedule goes to the payphone with its tables, in the call-in parameters table (0x23). The default is every 12 hours, as in upstream mm_manager. Change it for all payphones with `-I <minutes>`, or for one payphone with `callin_interval` in its `phone.cfg` (the *Calling in* card on the phones tab). A payphone only calls in on a schedule until midnight, then starts again at the same time of day the next day. For intervals under a day, mm_manager therefore starts the schedule as early in the day as the interval allows.

**Picking up updates when a payphone calls in (`-U`).** A payphone normally gets new tables only when it asks for them (on install, or a forced download from its craft menu). With `-U`, when a payphone calls in on its schedule, mm_manager checks whether it has an update waiting. An update is waiting if `update_pending` is there (mm_manager then sends every table), or if one of its tables, or its `phone.cfg`, changed since its last complete update (then just the changed tables). If so, mm_manager answers the call-in with a table update instead of asking for the payphone's data. The payphone reports its data on its next call. mm_manager only does this for a payphone whose model it already knows, and that it has updated before (or that has `update_pending`). Together with `-I`, payphones check for new tables every few hours by themselves.

> `-U` is experimental. It is tested against a simulated terminal, but not yet against a real Millennium. Please report how it goes.

```
# Payphones call in every 2 hours, and pick up whatever changed:
mm_manager -S 1000:secret@pbx.example.com -n 1000 -I 120 -U
```

## Editing tables in your browser

`html/table_editor.html` replaces hex editing. It is a single page with no build step and no server. Open it in a browser, either from your checkout or [online](https://html-preview.github.io/?url=https://github.com/jxburros/mm_manager/blob/main/html/table_editor.html). Your files stay in the browser and are written only when you press save. Served by `mm_gui` instead (see [the control panel](#the-control-panel-no-terminal-needed)), it reads and writes mm_manager's folder directly, in any browser.

* **Tables.** Open your `tables/` folder to see `default/`, the model folders and any terminal-specific folders together, or open single `mm_table_XX.bin` files. Each table that mm_manager reads from disk has its own form, with labelled fields, flags as checkboxes, phone numbers as digits, prices in dollars and times in seconds:
  * feature configuration, user interface parameters, coin validation and smart card parameters
  * card, carrier, call screening, repertory dialer, service level, rate and international rate lists
  * advertising messages, with a display preview and import and export in the `config/advert_prompts.csv` format
  * visual prompts, plus the LCD and NPA tables as maps you can paint
  * every table, including undocumented ones such as MODEM (0x21) and DIALRL (0x3e), as editable bytes
* **New and per-terminal tables.** *New table* creates any table, for example `mm_table_1e.bin`, pre-filled with mm_manager's built-in user interface defaults. *Copy for a terminal* saves a table into `tables/<terminal number>/`.
* **Manager settings.** This tab builds the `mm_manager` command line (served by `mm_gui`, it also starts and stops mm_manager): SIP or serial modem, NCC numbers, access code, key card, table folders and logging. It also lists the tables that mm_manager generates on each download, and which option controls each one.
* **ROM versions.** This tab edits `config/control_rom_versions.csv`, which maps control ROM editions to terminal types. It checks each row against the limits of mm_manager's CSV reader.

* **Help and tour.** The first visit opens a short welcome. *Take the tour* walks you through the real controls on practice tables, waiting for you to do each step (or doing it for you). The *help* tab has the full guide: where mm_manager looks for tables, which tables each firmware version gets, what every table does, and a glossary. Each table also shows what it does and a few tips; turn these hints off with *hints* at the top.

In Chrome, Edge and other Chromium browsers, *save* writes straight back to the folder you opened. In other browsers, *save* downloads the file, and you copy it into place.

## Credits

* **[mm_manager](https://github.com/hharte/mm_manager)**: Copyright (c) 2020-2025 Howard M. Harte, MIT License. mmumblephone is a fork of mm_manager, and everything except the SIP virtual modem is Howard's work. Thank you!
* The SIP virtual modem additions are Copyright (c) 2026 the mmumblephone contributors, under the same MIT License.
* Design inspiration for the virtual modem: [spandsp](https://github.com/freeswitch/spandsp) by Steve Underwood, [iaxmodem](https://iaxmodem.sourceforge.net/) and [t38modem](https://t38modem.sourceforge.net/).

The executable is still named `mm_manager`, so that existing scripts and documentation keep working.

---

## Original mm_manager documentation

The full mm_manager documentation (overview, compatibility, installation and usage, terminal hardware and provisioning, configuration tables, utilities, the low-level protocol, debugging, known issues and references) is in its own file: **[mm_manager documentation](MM_MANAGER.md)**. It is the upstream documentation, lightly annotated where the SIP virtual modem changes things.

## Unified release procedure

`release.json` is the authoritative release manifest. Keep configured embedded versions synchronized and advance affected apps (patch by default) before merging changes into main. The first adoption establishes a baseline; later PRs must pass **Release version validation**.

Use **Actions → Release → Run workflow** from **main**, choose stable or preview, and leave **dry_run** enabled first. Only apps ahead of their published version in their own namespace are selected. Build/test/sign all required packages before publishing. Tag pushes and merges no longer publish migrated apps automatically. After inspecting the dry run, dispatch with dry_run disabled and satisfy configured approvals. Published versions are never overwritten; use a new version for a correction.

See [.release/README.md](.release/README.md) for this repository's inventory, credentials, compatibility, and validation limits. Shared implementation: [JX Holdings release infrastructure](https://github.com/JX-Holdings-LLC/release-infrastructure).
