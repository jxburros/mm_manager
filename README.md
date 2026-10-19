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

* **tables**: your `tables/` folder is open already. Saving writes straight back into it, in any browser.
* **phones**: every payphone, by its terminal ID. Name it, change its ID when it changes on the phone, set how often it calls in, see whether its last update went through, and save its tables as a configuration to apply to other phones. See [several payphones](#several-payphones-updates-and-call-ins).
* **manager settings**: pick SIP or a serial modem, fill in the number the payphone dials, and press *start mm_manager*. Its output scrolls by on the page. It keeps running when you close the page; the button at the top shows whether it is running.
* **rom versions**: edits `config/control_rom_versions.csv` in place. mm_manager reads it when it starts.
* **records**: what payphones reported when they called in (calls, alarms, cash box counts, software versions), read from `mm_manager.db`. Save rows as CSV, or download the database.
* **tools**: the files in the working folder (download, upload, delete, turn a `.dlog` transcript into a `.pcap`, move generated tables into `tables/`), a scan for serial modems, and the Python table generators, with a button to install the packages they need. *Quit the control panel* stops everything.

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

# Original mm_manager documentation

The rest of this document is the upstream mm_manager documentation, lightly annotated where the SIP virtual modem changes things.

## Millennium Manager - mm_manager

### Manager for Nortel Millennium Payphones

[![Codacy Badge](https://app.codacy.com/project/badge/Grade/550811fd0fa140279b4edc2c1044860b)](https://www.codacy.com/gh/hharte/mm_manager/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=hharte/mm_manager&amp;utm_campaign=Badge_Grade)

[![Coverity](https://scan.coverity.com/projects/20341/badge.svg)](https://scan.coverity.com/projects/hharte-mm_manager)


# Overview

The Nortel Millennium payphones utilize a Manager to facilitate installation, reporting, call cost rating, and credit card processing.  This Millennium payphone calls into the Manager using a 1200-baud phone modem.  Unlike some other COCOT payphones, the Manager **_never calls the payphone_**.


# Compatibility

`mm_manager` runs on Windows, Linux, and MacOS X.

The `mm_manager` has been tested with a Nortel Multi-Pay (coin, credit card) Terminal with both V1.0 Control PCP (Through-hole, 1.20 firmware) and V1.1 Control PCP (Surface-mount, 2.11 firmware.)  It partially works with other terminal types and firmware versions, see table for limitations:


<table>
  <tr>
   <td><strong><em>Firmware Version</em></strong>
   </td>
   <td><strong><em>MTR</em></strong>
   </td>
   <td><strong><em>Terminal Type</em></strong>
   </td>
   <td><strong><em>Languages</em></strong>
   </td>
   <td><strong><em>Notes</em></strong>
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/blob/master/firmware/NQA1X01/NT_FW_2.12_U2_Rev2_CPC_STM29F040B_32PLCC">NQA1X01</a>
   </td>
   <td>2.20
   </td>
   <td>MP/MC
   </td>
   <td>English / Spanish
   </td>
   <td>Fully working
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/tree/master/firmware/NOK1X03">NOK1X03</a>
   </td>
   <td>2.13
   </td>
   <td>MP/MC/TTY
   </td>
   <td>
   </td>
   <td>?
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/tree/master/firmware/NLA1X05">NLA1X05</a>
   </td>
   <td>2.12
   </td>
   <td>
   </td>
   <td>
   </td>
   <td>?
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/tree/master/firmware/NKA1X02">NKA1X02</a>
   </td>
   <td>2.11
   </td>
   <td>MP/MC
   </td>
   <td>English / Spanish
   </td>
   <td>Fully working
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/armeniki/Nortel-Millennium/tree/master/Firmware/MTR_2.11/Multi_Pay">NKA1X05</a>
   </td>
   <td>2.11
   </td>
   <td>MP/MC
   </td>
   <td>
   </td>
   <td>?
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/tree/master/firmware/NCA1X03">NCA1X03</a>
   </td>
   <td>2.0
   </td>
   <td>MP/MC/DJ
   </td>
   <td>
   </td>
   <td>Fully working
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/blob/master/firmware/NPA1S01/dump2/NT_FW_1.20_STM27C2001_32DIP">NPA1S01</a>
   </td>
   <td>1.20
   </td>
   <td>MP/MC
   </td>
   <td>English / Spanish
   </td>
   <td>Fully working
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/blob/master/firmware/NNK1F05/U5_MTR1.13%20Keyboard.BIN">NNK1F05</a>
   </td>
   <td>1.13
   </td>
   <td>MP/MC/TTY
   </td>
   <td>English / French
   </td>
   <td>Fully working
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/blob/master/firmware/NBA1F02/U5_NBA1F02.BIN">NBA1F02</a>
   </td>
   <td>1.9
   </td>
   <td>MP/MC
   </td>
   <td>English / French
   </td>
   <td>Fully working
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/tree/71ccd39f3004b8e54ed6fa1af009516a9753a5fa/firmware/NNA1F02">NNA1F02</a>
   </td>
   <td>1.9
   </td>
   <td>MP/MC/TTY
   </td>
   <td>English / French
   </td>
   <td>Fully working
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/blob/master/firmware/NBJ1S03/NBJ1S03.bin">NBJ1S03</a>
   </td>
   <td>1.9
   </td>
   <td>Desk
   </td>
   <td>English / Spanish
   </td>
   <td>Fully working
   </td>
  </tr>
  <tr>
   <td>NBE1J01
   </td>
   <td>1.9
   </td>
   <td>Card-only
   </td>
   <td>English / Japanese
   </td>
   <td>Provisions but can’t rate calls.
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/tree/master/firmware/M06CAA17">06CAA17</a>
   </td>
   <td>1.7
   </td>
   <td>MP/MC
   </td>
   <td>
   </td>
   <td>Appears working except for chip cards, but Mandatory Table Alarm is present.
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/blob/master/firmware/06JAA03/U5_06JAA03.BIN">06JAA03</a>
   </td>
   <td>1.7
   </td>
   <td>MP/MC
   </td>
   <td>English / Japanese
   </td>
   <td>Fully working except for chip cards
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/blob/master/firmware/12AAV08/U5.BIN">12AAV08</a>
   </td>
   <td>1.7
   </td>
   <td>MP/MC
   </td>
   <td>
   </td>
   <td>?
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/armeniki/Nortel-Millennium/blob/master/Firmware/MTR_1.7/Multi_Pay/NAA1S05.bin">NAA1S05</a>
   </td>
   <td>1.7
   </td>
   <td>MP/MC
   </td>
   <td>
   </td>
   <td>?
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/blob/master/firmware/NPE1S01/NT_FW_1.20_NPE1S01_V1.0_M27C2001_DIP32_Coin_Basic.BIN">NPE1S01</a>
   </td>
   <td>1.20
   </td>
   <td>Coin Basic
   </td>
   <td>
   </td>
   <td>Fully working
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/armeniki/Nortel-Millennium/blob/master/Firmware/MTR_1.7/Coin/NAD4K02.bin">NAD4K02</a>
   </td>
   <td>1.7
   </td>
   <td>Coin Basic
   </td>
   <td>
   </td>
   <td>?
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/armeniki/Nortel-Millennium/blob/master/Firmware/MTR_1.7/Coin_NoDisplay/NAD4S02.bin">NAD4S02</a>
   </td>
   <td>1.7
   </td>
   <td>Coin Basic
   </td>
   <td>
   </td>
   <td>?
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/armeniki/Nortel-Millennium/blob/master/Firmware/MTR_1.7/Desk_Set/NAJ2S05.bin">NAJ2S05</a>
   </td>
   <td>1.7
   </td>
   <td>Desk
   </td>
   <td>
   </td>
   <td>Fully working except for chip cards
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/tree/master/firmware/M06CBBX1">06CBBX1</a>
   </td>
   <td>1.7
   </td>
   <td>Card-only
   </td>
   <td>English / Spanish
   </td>
   <td>Fully working except for chip cards
   </td>
  </tr>
  <tr>
   <td><a href="https://github.com/muccc/millennium/blob/master/firmware/PBAXS05/TEST.BIN">PBAXS05</a>
   </td>
   <td>1.7
   </td>
   <td>MP/MC/International
   </td>
   <td>
   </td>
   <td>Not working
   </td>
  </tr>
</table>



# Items Needed



1. Nortel Millennium Terminal running stock firmware (v1.20 or v2.20 is best, but other versions may work.)  **_Some Millennium Terminals purchased from online phone stores may have been re-programmed with “demo” firmware that does not need a Manager_**.  If you have one of these phones, you’ll have to program the phone back to [stock firmware](https://github.com/muccc/millennium/tree/master/firmware).
2. 24VDC @500mA Power Supply for Millennium Terminal
3. [4-pin plug-in screw terminal block](https://www.amazon.com/gp/product/B07SWSLR61) to connect 24VDC power and Tip/Ring to the terminal.
4. Two phone lines (one for `mm_manager`, one for the Millennium terminal.)  They can be real POTS lines, or lines from your own PBX, but the Millennium should be able to dial the manager with a 1- to 15-digit number.  _(mmumblephone: with the SIP virtual modem, the Manager side needs only a SIP account or PBX extension instead of a phone line.)_
5. _(mmumblephone: not needed when using the SIP virtual modem, `-S`.)_ 1200-baud or faster modem that supports [Bell 212A](https://en.wikipedia.org/wiki/Bell_212A) modulation.  I like the [Lenovo 56K USB](https://www.ebay.com/sch/i.html?&_nkw=lenovo+56k+usb+modem) modems, but any 56K modem with Conexant chipset should work.  Note for Linux users - Conexant soft-modems have never had a fully open-source driver, and the out-of-tree driver has not been maintained since kernel v2.4 - something like the US Robotics 5637 or another "real" modem where the device actually is responsible for encoding and decoding the data works well.
6. `mm_manager` software and a Windows, Linux machine (Raspberry Pi works great) or MacOS machine.  This machine should be available 24x7 so the terminal can call in when needed.
7. **_All terminals except desk:_** T-Key such as the [Jonard JIC-719A](https://jonard.com/jic-719a-t-key-tool?v=248) to open the payphone.  I don’t recommend the flat, stamped T-keys as they are prone to bending.
8. **_All terminals except desk:_** Keys for your terminal’s upper and lower locks, if locks are present.
9. **_Desk Terminal Only:_** A “Key Card” to enter the craft access menu.  This can be any credit card– best to use an expired one.  Specify the first 10-digits of the credit card number to `mm_manager` with the `-k` option.  The default key card number is 4012888888.  You can also program a magnetic stripe card with that number.


# mm_manager Installation and Usage

Simply download the source files and example tables from GitHub.


## Linux and MacOS

You need a C11 compiler and CMake 3.25 or later.  From the shell, type:


```
cmake .
make
ctest
```


to compile `mm_manager` and its utilities, and to run the tests (the software modem, the SIP virtual modem end to end and, on Unix, modem detection).  The build uses `-Wall -Wextra -Werror`, so warnings fail the build.  An out-of-source build (`cmake -S . -B build && cmake --build build`) works too.  `make install` installs the programs to `bin`, and the default tables, `config` and Wireshark files with them.


## Windows

Compile `mm_manager` with Microsoft Visual Studio with CMake 3.25 or later (Visual Studio 2022 17.5 or later includes it), using “Open Folder.”


### Usage:


```
usage: mm_manager [-vhmqDU] [-f <filename>] [-i "modem init string"] [-l <logfile>] [-p <pcapfile>] [-a <access_code>] [-k <key_code>] [-n <ncc_number>] [-d <default_table_dir] [-t <term_table_dir>] [-u <port>] [-S <sip_config>] [-R <retries>] [-I <minutes>]
        -a <access_code> - Craft 7-digit access code (default: CRASERV)
        -b <baudrate> - Modem baud rate, in bps.  Defaults to 19200.
        -c - Always download complete table set.
        -D - detect: scan the serial ports for modems, list them, and exit.
        -d <default_table_dir> - default table directory.
        -e <error_inject_type> - Inject error on SIGBRK.
        -f <filename> modem device or file.  With -m, -f auto (or leaving out -f) uses the first modem found.
        -h this help.
        -I <minutes> - Minutes between a terminal's scheduled call-ins (default: 720).
             A terminal's <term_table_dir>/<terminal_id>/phone.cfg can set its own callin_interval.
        -i "modem init string" - Modem initialization string.
        -k <key_code> - Desk Terminal 10-digit key card code (default: 4012888888)
        -l <logfile> - log bytes transmitted to and received from the terminal.  Useful for debugging.
        -m use serial modem (specify device with -f)
        -n <Primary NCC Number> [-n <Secondary NCC Number>] - specify primary and optionally secondary NCC number.
        -p <pcapfile> - Save packets in a .pcap file.
        -q - Don't display sign-on banner.
        -R <retries> - Resend a table the terminal doesn't acknowledge up to <retries> times (default: 2).
        -r - Rating test mode: Amount charged determined by last 4 digits of dialed number.
        -S <sip_config> - Use the SIP virtual modem instead of a serial modem.  <sip_config> is:
             user[:password]@registrar[:port][;option=value...] to register with a SIP server, or
             listen[;option=value...] to accept direct SIP calls without registering.
             Options: port, rtp_port, expires, auth_user, domain, local_ip, codec=pcmu|pcma,
             answer_tone=2225|2100.  The password may also be given in MM_SIP_PASSWORD.
        -s - Download only minimum required tables to terminal.
        -t <term_table_dir> - terminal-specific table directory.
        -U - When a terminal calls in and has new tables waiting (or a failed update to retry), send them.
        -u <port> - Send packets as UDP to <port>.
        -v verbose (multiple v's increase verbosity.
        -w - don't monitor the modem for carrier loss.
```



## Pre-built packages

Every push to `main` builds and tests the code on Linux (x64), macOS (x64) and Windows (x64 and x86), and publishes the packages as a GitHub pre-release.  Creating a release attaches the same packages to that release.


## Running from the right folder

`mm_manager` finds its files relative to the current directory, so start it from the folder that contains `tables/` and `config/` (normally your checkout), or point it at your tables with `-d` and `-t`.  The files are:

* `tables/default` and `tables/` - the default and terminal-specific table folders (`-d`, `-t`).  See [several payphones](#several-payphones-updates-and-call-ins) for the other files in a terminal's folder.
* `configurations/` - configurations saved from the control panel's phones tab.
* `config/control_rom_versions.csv` - maps each terminal's control ROM edition to its terminal type.
* `mm_manager.db` - a SQLite database that `mm_manager` creates in the current directory.  It keeps what the terminals report: alarms, call detail records, call and performance statistics, cash box status and collections, terminal status and software versions.



# Millennium Terminal Hardware Installation


### Phone Line

The Nortel Millennium payphones require a standard POTS line, with answer supervision in the form of polarity reversal to indicate that the far end has answered the call.  This is required for the Millennium to know when to collect or refund coins.  Most SIP ATAs and Cisco Voice Routers can be configured for polarity reversal.  If your phone line does not support polarity reversal, an answer supervision detection module is available from Nortel.

If you are using VoIP, make sure to use the u-law PCM codec, disable silence suppression, disable comfort noise generation, and disable echo cancellation.  This is required to condition the line for modem operation.  You may also need to increase the jitter buffer size, and use a fixed jitter buffer, rather than adaptive.


### Power Supply

The Nortel Millennium terminal requires 24VDC at 500mA to supply power to the phone.  Only limited functionality is provided for emergency service when this power is not present.


### Other Terminal Installation Notes

The Millennium Terminal is an advanced payphone that contains a multitude of sensors to determine if the phone is installed and operating properly.  This includes sensors to make sure that a coin box is installed, and also a sensor to ensure the coin vault door is in place.  If your Millennium does not have a coin box, please obtain one.  The coin boxes are standard Western Electric / Northern Electric Single Slot, readily available.  In a pinch, the coin box sensor switch can be taped in the closed position.  The same goes for the coin vault door.  The coin vault door must be in place and locked.  If your phone is missing the coin vault lock, as is quite common for these phones purchased on the second-hand market, please obtain the correct lock, or tape the switch closed.

If these sensors are not happy, the phone will alarm, and will go “Out of Service.”


# Millennium Terminal Provisioning

Provisioning the Millennium Terminal is accomplished through the craft access menu on the terminal itself.  For this, you will need the terminal’s access code, and a PIN.  The default Access Code is 2727378 (CRASERV).

With the upper housing of the phone locked, take the handset off the cradle and replace it.  	Then key in 2727378 (CRASERV.)  You will be prompted for a PIN, use anything above 50000, like 55555.  Unlock the upper housing with a T-Key when prompted.  You do not need to open the upper housing.

Generate NPA and LCD tables (see below,) unless you want to use the default tables included with mm_manager.

Start `mm_manager`:


```
./mm_manager -m -n 18005551234 -f /dev/ttyACM0 -vv -l install.dlog -p install.pcap
```


Where:

	18005551234 should be replaced by the phone number of your Manager (1-15 digits).

	`/dev/ttyACM0` should be replaced by your modem device.  For Windows, it will be something like `\\.\COMx` where ‘`x`’ is the COM port number of your modem.

	install.dlog is a log file that contains all of the data received and sent by the manager.

	install.pcap is a packet capture that can be loaded into [Wireshark](https://github.com/hharte/mm_manager/tree/master/wireshark) for debugging.

With the SIP virtual modem, use `-S` instead of `-m -f`, as described [above](#using-the-sip-virtual-modem):


```
./mm_manager -S 1000:secret@pbx.example.com -n 18005551234 -vv -l install.dlog -p install.pcap
```


Follow the Terminal’s on-screen prompts to install.

Key in this Terminal’s telephone number (10-digits) NPA-NXX-XXXX.

Key in this Terminal’s 10-digit serial number (1234567890 is fine.)

Key in the Manager’s phone number (I use 1-800-555-1234, which I intercept in the Asterisk dialplan and send to the modem connected to the computer running `mm_manager`.)


# Configuration Tables

Communication with the Nortel Millennium involves sending and receiving tables.  Tables are numbered 1 through 155, and contain configuration information sent to the terminal or query / status information received from the terminal.  Tables are of fixed size, depending on the type of table.

Each packet response from the Manager includes the Terminal’s phone number, followed by a byte containing the Table ID of the current table being sent by the Manager.  Multiple tables may be packed into a single packet (the maximum packet table payload length is 245 bytes.)

In the case of large table download (> 245 bytes) to the Millennium terminal, the table payload is split into 245-byte chunks, but the Table ID is only included in the first packet.

The following are some of the tables used by the Millennium terminal.  They should be customized (using a hex editor) for your specific phone.  _(mmumblephone: or with the [table editor](#editing-tables-in-your-browser), `html/table_editor.html`.)_  Some parameters (like NCC numbers and Access Code) can be changed via command-line parameters to `mm_manager`.

You can customize the Advertising Messages (ADMESS table 0x1d) using the [web-based editor](https://html-preview.github.io/?url=https://github.com/hharte/mm_manager/blob/main/html/admess_editor.html) (`html/admess_editor.html`).  Upload your mm_table_1d.bin file, modify it as needed, and then download the updated version from the webpage.  The [table editor](#editing-tables-in-your-browser) does this and more.  Advertising messages can also be generated from a CSV file with `generate_advert_prompts.py` (see below).

Some of the important tables for configuring a Millennium terminal include:


<table>
  <tr>
   <td><strong><em>Table ID (DEC)</em></strong>
   </td>
   <td><strong><em>Table ID (Hex)</em></strong>
   </td>
   <td><strong><em>Description</em></strong>
   </td>
   <td><strong><em>Length</em></strong>
   </td>
   <td><strong><em>Notes</em></strong>
   </td>
  </tr>
  <tr>
   <td>10
   </td>
   <td>0xa
   </td>
   <td>Terminal Status
   </td>
   <td>10
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_term_status">TSTATUS</a> - pp. 2-636
   </td>
  </tr>
  <tr>
   <td>13
   </td>
   <td>0xd
   </td>
   <td>End of Data Message
   </td>
   <td>0
   </td>
   <td>Manager sends no data.
   </td>
  </tr>
  <tr>
   <td>14
   </td>
   <td>0x0e
   </td>
   <td>Table Update ACK
   </td>
   <td>1
   </td>
   <td>Terminal -> Manager
   </td>
  </tr>
  <tr>
   <td>18
   </td>
   <td>0x12
   </td>
   <td>Terminal Table Data Update
   </td>
   <td>0
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_table_upd">TERMDAT</a> - pp. 2-528
   </td>
  </tr>
  <tr>
   <td>20
   </td>
   <td>0x14
   </td>
   <td>Date/Time Synchronization
   </td>
   <td>7
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_time_sync">Time Sync</a>
   </td>
  </tr>
  <tr>
   <td>21
   </td>
   <td>0x15
   </td>
   <td>Terminal Access Parameters
   </td>
   <td>47
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_ncc_term_parms">TERM</a> - pp. 2-492: Contains our number and  primary/sec NCC#, call in start date, time, interval, CDR threshold.
   </td>
  </tr>
  <tr>
   <td>26
   </td>
   <td>0x1a
   </td>
   <td>Feature Configuration – Universal
   </td>
   <td>71
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_fconfig_opts">FEATRU</a> - pp. 2-151
   </td>
  </tr>
  <tr>
   <td>29
   </td>
   <td>0x1d
   </td>
   <td>Advertising Prompts
   </td>
   <td>480
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_advert_prompts">ADMESS</a> - pp. 2-5
   </td>
  </tr>
  <tr>
   <td>30
   </td>
   <td>0x1e
   </td>
   <td>User Interface Parameters Universal
   </td>
   <td>67
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_user_if_parms">USERPRM</a> - pp. 2-669
   </td>
  </tr>
  <tr>
   <td>31
   </td>
   <td>0x1f
   </td>
   <td>Installation/Servicing Parameters
   </td>
   <td>36
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_install_parms">INSTSV</a> - pp. 2-235
   </td>
  </tr>
  <tr>
   <td>32
   </td>
   <td>0x20
   </td>
   <td>Communication Statistics and Configuration Parameters
   </td>
   <td>32
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_comm_stat_parms">COMMST</a> - pp. 2-86
   </td>
  </tr>
  <tr>
   <td>33
   </td>
   <td>0x21
   </td>
   <td>Modem Parameters
   </td>
   <td>35
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_modem_parms">MODEM</a> - pp. 2-279
   </td>
  </tr>
  <tr>
   <td>34
   </td>
   <td>0x22
   </td>
   <td>Call Statistics Parameters
   </td>
   <td>18
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_call_stats">CALLST</a> - pp. 2-36
   </td>
  </tr>
  <tr>
   <td>35
   </td>
   <td>0x23
   </td>
   <td>Time/Call-In Parameters
   </td>
   <td>20
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_call_in_parms">TERM</a> table, pp. 2-517
   </td>
  </tr>
  <tr>
   <td>36
   </td>
   <td>0x24
   </td>
   <td>Time Synchronization Request
   </td>
   <td>0
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_time_sync_req">TTMSYNC</a>
   </td>
  </tr>
  <tr>
   <td>38
   </td>
   <td>0x26
   </td>
   <td>Terminal Cash Box Status
   </td>
   <td>62
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_cash_box_status">TCASHST</a> - pp. 2-420
   </td>
  </tr>
  <tr>
   <td>44
   </td>
   <td>0x2c
   </td>
   <td>Terminal Upload Attention
   </td>
   <td>1
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_atn_req_tab_upd">TUPLOAD</a> - pp. 2-656
<p>
TTBLREQ - pp. 2-651
   </td>
  </tr>
  <tr>
   <td>50
   </td>
   <td>0x32
   </td>
   <td>Coin Validation Parameters
   </td>
   <td>104
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_coin_val_table">COINVL</a> - pp. 2-79
   </td>
  </tr>
  <tr>
   <td>51
   </td>
   <td>0x33
   </td>
   <td>Cash Box
<p>
Collection
   </td>
   <td>70
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_cash_box_collection">TCOLLCT</a> - pp. 2-
   </td>
  </tr>
  <tr>
   <td>55
   </td>
   <td>0x37
   </td>
   <td>Enhanced Repertory Dialer List
   </td>
   <td>570
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_rep_dial_list">RDLIST</a> - pp. 2-329
   </td>
  </tr>
  <tr>
   <td>58
   </td>
   <td>0x3a
   </td>
   <td>Service Level
   </td>
   <td>25
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_limserv_data">SERVLEV</a> - pp. 2-349
   </td>
  </tr>
  <tr>
   <td>60
   </td>
   <td>0x3c
   </td>
   <td>Terminal Software Version
   </td>
   <td>27
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_sw_version">TSWVERS</a> - pp. 2-647
   </td>
  </tr>
  <tr>
   <td>62
   </td>
   <td>0x3e
   </td>
   <td>Dialing Rules
   </td>
   <td>352
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_num_plan_table">DIALRL</a> - pp. 2-107
<p>
32 bytes: Fixed Format Data
<p>
180 bytes: Numbering Plan Rules
<p>
140 bytes: Display Formatting
   </td>
  </tr>
  <tr>
   <td>72
   </td>
   <td>0x48
   </td>
   <td>Spare Table
   </td>
   <td>1000
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_spare_table">SPARE</a> - pp. 2-356 NOTE: This is a required table for V1.3 PCP, but not used on V1.0.
   </td>
  </tr>
  <tr>
   <td>73
   </td>
   <td>0x49
   </td>
   <td>Rate Table
   </td>
   <td>1191
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_rate_table">RATE</a> - pp. 2-321
   </td>
  </tr>
  <tr>
   <td>
   </td>
   <td>
   </td>
   <td>French VFD Text
   </td>
   <td>8000
   </td>
   <td>French: V1.3 Only
<p>
0x74001 offset in U2 ROM.
   </td>
  </tr>
  <tr>
   <td>85
   </td>
   <td>0x55
   </td>
   <td>Expanded Visual Prompts Language A
   </td>
   <td>8000
   </td>
   <td>English: V1.3 Only
<p>
Qty 400: 20-line VFD strings
<p>
0x71B5A offset in U2 ROM.
   </td>
  </tr>
  <tr>
   <td>
   </td>
   <td>
   </td>
   <td>Japanese VFD Text
   </td>
   <td>8000
   </td>
   <td>Japanese: V1.3 Only
<p>
0x7A001 offset in U2 ROM.
   </td>
  </tr>
  <tr>
   <td>86
   </td>
   <td>0x56
   </td>
   <td>Expanded Visual Prompts Language B
   </td>
   <td>8000
   </td>
   <td>Spanish: V1.3 Only
<p>
Qty 400: 20-line VFD strings
<p>
0x77001 offset in U2 ROM.
   </td>
  </tr>
  <tr>
   <td>92
   </td>
   <td>0x5c
   </td>
   <td>180 Number Call Screening List
   </td>
   <td>3060
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_call_screen_list">CALLSCRN</a> (Call Screening List) pp. 2-33, and FREE (Free Call) pp. 2-187
   </td>
  </tr>
  <tr>
   <td>93
   </td>
   <td>0x5d
   </td>
   <td>Smart Card Parameters Table
   </td>
   <td>224
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_scard_parm_table">SMCARD</a> - pp. 2-351
   </td>
  </tr>
  <tr>
   <td>134
   </td>
   <td>0x86
   </td>
   <td>Expanded Card Table (32 Entries)
   </td>
   <td>1152
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_card_table">CARD</a> - pp. 2-57
   </td>
  </tr>
  <tr>
   <td>135
   </td>
   <td>0x87
   </td>
   <td>Expanded Carrier Table (33 Entries)
   </td>
   <td>1108
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_carrier_table">CARRIER</a> - pp. 2-69
   </td>
  </tr>
  <tr>
   <td>136-149
   </td>
   <td>0x88-0x95
   </td>
   <td>Double Compressed LCD Tables 1-14
   </td>
   <td>202
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_npa_nxx_table_1">LCD</a> (Local Call Determination) - pp. 2-248
   </td>
  </tr>
  <tr>
   <td>150
   </td>
   <td>0x96
   </td>
   <td>Set Based Rating NPA Table
   </td>
   <td>400
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_npa_sbr_table">RATENPA</a> - pp. 2-327
   </td>
  </tr>
  <tr>
   <td>151
   </td>
   <td>0x97
   </td>
   <td>Set Based Rating - International Table
   </td>
   <td>603
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_intl_sbr_table">RATEINT</a> - pp. 2-326
   </td>
  </tr>
  <tr>
   <td>154,155
   </td>
   <td>0x9a,0x9b
   </td>
   <td>Double Compressed LCD Tables 15,16
   </td>
   <td>202
   </td>
   <td><a href="https://wiki.muc.ccc.de/millennium:dlog:dlog_mt_npa_nxx_table_1">LCD</a> (Local Call Determination) - pp. 2-248
   </td>
  </tr>
</table>




* Page number references are to [Millennium Database Design Report MSR 2.1](https://github.com/muccc/millennium/blob/master/documentation/manager/A0xxxxxx_00_02.pdf).


## Generating NPA and LCD tables

Python3 scripts are included to generate NPA, LCD, international rating and advertising tables automatically, using spreadsheets available from the [North American Numbering Plan Administrator](https://www.nanpa.com/).  For Canada, spreadsheets are available from the [Canadian Numbering Administrator](http://www.cnac.ca/).  The NPA and LCD scripts require the Python3 libraries pandas, requests, and xmltodict (`generate_advert_prompts.py` and `generate_intl_sbr.py` use only the standard library).  `mm_lcd.py` is a helper module used by `generate_lcd.py` and `generate_lcd_lir.py`.

To generate an NPA table that classifies US and Canadian numbers as domestic:


```
./generate_npa.py --countries US CANADA
```


The `generate_lcd_lata.py` script is used to generate LCD tables for a given NPA-NXX in the United States.  LCD tables generated using this script will properly classify Local, Intra-LATA and Inter-LATA calls.  This script does not work in Canada as all of Canada is a single LATA.

For example, to generate LCD tables for San Jose, California, USA for a terminal with phone number of (408) 535-XXXX :


```
./generate_lcd_lata.py --npa=408 --nxx=535
```


To generate LCD tables for Ottawa, Canada, use `generate_lcd_lir.py`:


```
./generate_lcd_lir.py --npa 613 --nxx 562
```


`generate_lcd.py` generates LCD tables from a list of rate centers that you choose, for the USA or Canada, instead of looking them up.  For example, for the San Jose, California rate center:


```
./generate_lcd.py --state CA --npa 408 --ratecenters "SNJS NORTH" "SNJS WEST" "SNJS SOUTH" CAMPBELL SARATOGA SUNNYVALE "LOS GATOS"
```


`generate_intl_sbr.py` generates the International Set-based Rating table (0x97) from `icc_dial_codes.csv`, which maps each international dialing code to a RATE table entry.  Run it from the repository folder.  It writes `mm_table_97.bin` to the current directory, so copy it into your `tables/` folder:


```
./generate_intl_sbr.py
```


`generate_advert_prompts.py` generates the Advertising Messages table (0x1d) from a CSV file with 20 messages (the first 10 are on-hook, the last 10 are off-hook).  By default it reads `config/advert_prompts.csv`, and writes `mm_table_1d.bin` to the current directory.  With `--terminal`, it writes the table into `tables/<terminal>/` instead, which must already exist:


```
./generate_advert_prompts.py --csv config/advert_prompts.csv --terminal 4085359990
```



## Terminal-Specific Tables

`mm_manager` has the ability to support multiple terminals with different provisioning. `mm_manager` searches for configuration tables as follows:



1. `tables/NPANXXXXXX` - where `NPANXXXXXX` is the 10-digit Terminal ID (phone number.)
2. `tables/<model-specific-dir>` - where `<model-specific-dir>` is one of:

	`multipay, card_only, desk, coin, inmate`.

	The folder is chosen from the terminal type that the terminal reports.  Folders for `card_only`, `desk` and `coin` are included; add `multipay` or `inmate` yourself if you need them.



3. `tables/default` - will be used as a last resort if tables cannot be found in the previous directories.

The `tables` and `tables/default` folders can be changed with the `-t` and `-d` options.

`mm_manager` stores the last table update date/time in the terminal-specific directory.  This allows for quicker iteration during testing by using "force download" in the terminal’s craft interface.  This will download only the table that changed and a few tables that are generated within `mm_manager` itself.


### Terminal-specific Table Example

Two multipay terminals with different Advertising messages, and otherwise configured the same:

`tables/4085359990/mm_table_1d.bin` - Advertising messages for terminal ID 4085359990

`tables/4085359995/mm_table_1d.bin` - Advertising messages for terminal ID 4085359995

`tables/default/` -  All of the default tables are in this directory (including `mm_table_1d.bin`)


# mm_manager Utilities


<table>
  <tr>
   <td>Utility
   </td>
   <td>Description
   </td>
  </tr>
  <tr>
   <td>mm_admess
   </td>
   <td>Dump Advertising Messages table
   </td>
  </tr>
  <tr>
   <td>mm_areacode
   </td>
   <td>Dump Set-based rating (NPA) table, MTR 1.20, 2.x
   </td>
  </tr>
  <tr>
   <td>mm_callin
   </td>
   <td>Dump Call-in Parameters table
   </td>
  </tr>
  <tr>
   <td>mm_callscrn
   </td>
   <td>Dump Call Screening List
   </td>
  </tr>
  <tr>
   <td>mm_callstat
   </td>
   <td>Dump Call Statistics Parameters table
   </td>
  </tr>
  <tr>
   <td>mm_card
   </td>
   <td>Dump Credit Card / Smart Card table MTR 1.20, 2.x
   </td>
  </tr>
  <tr>
   <td>mm_card_mtr1
   </td>
   <td>Dump Credit Card / Smart Card table MTR 1.7, 1.9
   </td>
  </tr>
  <tr>
   <td>mm_carrier
   </td>
   <td>Dump Carrier table MTR 1.20, 2.x
   </td>
  </tr>
  <tr>
   <td>mm_carrier_mtr1
   </td>
   <td>Dump Carrier table MTR 1.7, 1.9
   </td>
  </tr>
  <tr>
   <td>mm_coinvl
   </td>
   <td>Dump Coin Validator table
   </td>
  </tr>
  <tr>
   <td>mm_commstat
   </td>
   <td>Dump Communications Statistics Parameters table
   </td>
  </tr>
  <tr>
   <td>mm_convert_callscrn_mtr2_to_mtr1
   </td>
   <td>Convert MTR 1.20/2.x Call Screening List to MTR 1.7, 1.9.
   </td>
  </tr>
  <tr>
   <td>mm_convert_card_mtr2_to_mtr1
   </td>
   <td>Convert MTR 1.20/2.x Card Table to MTR 1.7, 1.9.
   </td>
  </tr>
  <tr>
   <td>mm_dlog2pcap
   </td>
   <td>Convert mm_manager dialog output to pcap format for visualization with WireShark.
   </td>
  </tr>
  <tr>
   <td>mm_fconfig
   </td>
   <td>Dump Feature Configuration Options table
   </td>
  </tr>
  <tr>
   <td>mm_instsv
   </td>
   <td>Dump Installation / Servicing table
   </td>
  </tr>
  <tr>
   <td>mm_lcd
   </td>
   <td>Dump LCD tables (supports uncompressed, compressed, and double-compressed tables)
   </td>
  </tr>
  <tr>
   <td>mm_limserv
   </td>
   <td>Dump Service Level (Limited Service) table
   </td>
  </tr>
  <tr>
   <td>mm_luhn
   </td>
   <td>Generate / Check magnetic card Luhn check digit
   </td>
  </tr>
  <tr>
   <td>mm_packtest
   </td>
   <td>Check the packing of the table data structures (built, but not installed.)
   </td>
  </tr>
  <tr>
   <td>mm_rate
   </td>
   <td>Dump Rate table
   </td>
  </tr>
  <tr>
   <td>mm_rateint
   </td>
   <td>Dump International Set-based rating table (MTR 1.20. 2.x)
   </td>
  </tr>
  <tr>
   <td>mm_rdlist
   </td>
   <td>Dump Repertory Dialer list
   </td>
  </tr>
  <tr>
   <td>mm_smcard
   </td>
   <td>Dump Smart Card (TeleCard) table
   </td>
  </tr>
  <tr>
   <td>mm_table_cutter
   </td>
   <td>Extract ROM tables from firmware binaries
   </td>
  </tr>
  <tr>
   <td>mm_userif
   </td>
   <td>Dump User Interface Parameters table
   </td>
  </tr>
</table>



# Low-Level Protocol

The low-level protocol sent over the modem is a stream of bytes framed within START and END bytes.


<table>
  <tr>
   <td>START
   </td>
   <td>FLAG Byte
   </td>
   <td>LEN (includes END)
   </td>
   <td>DATA
   </td>
   <td>CRC-16
   </td>
   <td>END
   </td>
  </tr>
  <tr>
   <td>02
   </td>
   <td>1-byte
   </td>
   <td>1-byte
   </td>
   <td>N bytes...
   </td>
   <td>2-bytes *
   </td>
   <td>03
   </td>
  </tr>
</table>


*CRC-16 IBM with the 0xA001 polynomial [per this site](https://www.scadacore.com/tools/programming-calculators/online-checksum-calculator/).  Includes all bytes including START through the DATA.

FLAG Bits:


<table>
  <tr>
   <td>7
   </td>
   <td>6
   </td>
   <td>5
   </td>
   <td>4
   </td>
   <td>3
   </td>
   <td>2
   </td>
   <td>1
   </td>
   <td>0
   </td>
  </tr>
  <tr>
   <td>?
   </td>
   <td>?
   </td>
   <td>DISCONNECT
   </td>
   <td>STATUS (1 = failure)
   </td>
   <td>ACK
   </td>
   <td>RETRY
   </td>
   <td colspan="2" >Sequence
   </td>
  </tr>
</table>


Separate sequence numbers are counted for each of tx_seq and rx_seq.  ACKs should always be sent with the same sequence as the last received Rx packet.  Tx seq is incremented after every successful packet transmission, and reset when the terminal disconnects.


# Debugging


## Dialog Transcripts

`mm_manager` can log all bytes sent to or received from a Millennium terminal using the `-l <logfile.dlog>` option.  This can be used to record a transcript of the session, that can be “played back” to `mm_manager` by specifying this file to the -f option, without supplying -m (modem.)  This allows quick iteration when debugging and testing `mm_manager`, as a real Millennium terminal is not needed.

One useful trick is to parse the transcript with `mm_manager`, and save it to a file.  Then the code can be modified and improved and tested by re-running the transcript through `mm_manager` and comparing it with the previous run using a tool such as `tkdiff`.


## Wireshark

`mm_manager` can save all packets sent and received to a packet capture (.pcap) file for viewing in [Wireshark](https://www.wireshark.org/) using the `-p <pcapfile.pcap>` option.  This .pcap file can be opened with [Wireshark](https://www.wireshark.org/), and dissected using the [Millennium LUA Dissector Plugin](https://github.com/hharte/mm_manager/blob/main/wireshark/README.md).

In addition, mm_manager can send all packets via UDP to the localhost port 27273 (“CRASE”) so [Wireshark](https://www.wireshark.org/) can view them in real-time while communicating with a terminal.


# Filing Bug Reports

If you find a bug, please provide the following information when you report the bug on GitHub:



1. Please make sure you run `mm_manager` with the `-l <filename>` option to generate the session transcript of all the data sent to and from the manager.  Please attach this file to your bug report.
2. Use mm_manager’s `-p <pcapfile.pcap>` option to save a packet capture and attach this file to your bug report.
3. Please provide information about the type of Millennium phone you have and what ROM version it’s running.  Some of this information is displayed by `mm_manager` after it connects to the phone.
4. Please provide details about the operating system you are using, what kind of modem you are using, and if you are using a serial modem, what type of serial port you are using (built-in PC serial port, USB serial port, etc.)
5. Please provide details about the phone lines you are using: Are they VoIP, POTS, going through your own PBX, or going over the PSTN?  Analog and TDM switches are preferable to VoIP, if at all possible.
6. Steps to reproduce the issue.


# Known Issues



1. When installing a Millennium Terminal, the answer supervision test will call the Manager, and disconnect.   It may take a few seconds for mm_manager’s modem to detect this condition and go back on-hook.  You may want to wait a few seconds after the test completes before proceeding with the installation.
2. Some older firmware versions will fail to install on the first try.  They will usually install correctly on a retry.
3. Lots of tables are not well understood.  Please help figure more out if you can.


# Further Work Needed (Feel free to help!)



1. Improve modem robustness.
2. Lots of other things, please file bugs as you find them.


# References

[Nortel Millennium on Wikipedia](https://en.wikipedia.org/wiki/Nortel_payphones#Millennium)

[Chaos Computer Club Muenchen](https://wiki.muc.ccc.de/millennium:start)

[muccc / millennium on GitHub](https://github.com/muccc/millennium/)

[Description of Millennium Tables](https://wiki.muc.ccc.de/millennium:dlog:start)

[nortel-voiceware-decoder](https://github.com/hharte/nortel-voiceware-decoder) - Voiceware ROM decoder to extract voice prompts to .wav files

[millenniium-panel on GitHub](https://github.com/pc-coholic/millennium-panel)

[Armeniki’s Nortel Millennium information on GitHub](https://github.com/armeniki/Nortel-Millennium)

[Millennium Database Design Report MSR 2.1](https://github.com/muccc/millennium/blob/master/documentation/manager/A0xxxxxx_00_02.pdf)

[Millennium Multi-Pay Terminal Installation, Operation, and Maintenance Guide](https://github.com/muccc/millennium/blob/master/documentation/nortel_millennium_text.pdf)

[Millennium Desk Terminal: Installing and Repairing Terminal Hardware](https://github.com/muccc/millennium/blob/master/documentation/desk_terminals/P0883900_00_01.pdf)

[pyMM](https://github.com/pc-coholic/pyMM)
