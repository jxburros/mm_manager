# Changelog

### 0.5.4: update center, releases build on GCC 15

- An update center in the control panel, under manager → updates. It lists the releases on GitHub that are newer than the installed version, and installs the one you pick: any newer version, never an older one or the same one (mm_gui checks this itself, not only the page). The package is checked against the release's `SHA256SUMS.txt` and the new mm_gui is test-run before anything is replaced; a failed install puts back what it changed. Installed tables and config you may have edited are kept. Then mm_gui restarts as the new version on the same port. Windows installs from the installer get the new installer instead. Previews are listed only on request. See README → Updates.
- `mm_gui -V` prints the version.
- Tests: `tests/test_gui_update.py` (no downgrades, a damaged package is refused, an install keeps the user's config).
- The release build failed on GitHub's current Linux runners: GCC 15 with glibc 2.43 warns about the bundled `third-party/sqlite3.c`, and warnings are errors. Warnings in that vendored file no longer stop the build; mmumblephone's own code still must build cleanly. 0.5.3 was never published, so 0.5.4 is the first release with its changes.

### 0.5.3: control panel settings stay put

- The control panel's settings (mm_manager's options, the page you were on, theme, hints) now also live in the working folder, in `.mm_gui/prefs/`, and the page reads them back when it opens. Before, only the browser kept them, and a browser keeps them per address. They were lost whenever mm_gui moved to the next free port, the page was opened as `localhost` instead of `127.0.0.1`, or another browser was used. mm_manager's options are shared by the admins; everything else is each account's own, and is removed with the account.
- The SIP password now survives a reload: it stays in that browser tab until the tab is closed. It is still never written to disk.
- `tests/test_gui_accounts.py` covers the new `/api/prefs` call.

### 0.5.2: accurate wording on the Elcotel formats

- The Elcotel documentation and code comments no longer call the R94/S94/P94 support "clean room". That term means a documented process with separate people for reading the original and writing the code, which wasn't followed here. They now say what was done: a separate implementation written from elcotel-playground's published format description, with no code from that GPL-3.0 project copied or adapted. The two R94 details found here are marked as seen in only one sample file. No change to the programs.

### 0.5.1: releases publish again

- The release workflow now uses the shared release toolkit at d7f42f9. The earlier pin looked up its new draft release by tag, which GitHub doesn't answer for drafts, so publishing stopped with "Not Found" (the 0.4.0 release run). The new toolkit also puts the notes of every unpublished version (0.4.0, 0.5.0, 0.5.1) into the release.

### 0.5.0: control panel accounts

- Optional accounts in the control panel (`mm_gui`), under global → accounts. With none, nothing changes. Once an admin account exists, everyone signs in. Admins can use everything; a user only gets the phones an admin assigns, and on each only the parts ticked: overview, records and each group of settings. mm_gui enforces this on every request.
- Passwords are stored as salted PBKDF2-SHA-256 hashes in `.mm_gui/accounts`, which the file API can't reach. Repeated wrong passwords lock an account briefly; the last admin can't be removed.
- `mm_gui -L <address>` lets other computers sign in (plain HTTP: trusted networks only). Without accounts, only the local computer is answered, as before.
- Tests: SHA-256/HMAC/PBKDF2 vectors (`test_sha256`) and an end-to-end check of the accounts API (`tests/test_gui_accounts.py`).

### 0.4.0: Protel and Elcotel groundwork, compatibility matrix

- Add `mm_payphone.py` and the `payphones` package: a backend boundary for families other than the Nortel Millennium, which mm_manager keeps handling as before.
- Protel: a configuration model (cost bands, area code and exchange bands, options) checked against the ExpressNet manual's limits; reading ExpressNet export files into inventory entries; ExpressNet communication errors explained by session phase. Native programming and reporting are not implemented: the protocol is not public and has not been captured yet (docs/protel.md).
- Elcotel Series 5: reading, checking and writing of R94, S94 and P94/P99 files, written from elcotel-playground's published format description (no code from that GPL-3.0 project was copied or adapted).
- Session captures with the metadata research needs, transcript import and comparison.
- A compatibility matrix (config/compatibility.csv, docs/compatibility.md) listing every operation for every board as planned, experimental or hardware verified. Nothing for Protel or Elcotel is hardware verified.

### Release infrastructure migration — 2026-10-20

- Adopt the unified release manifest, version-advance validation, manual main-only planning, complete artifact staging, and verified draft publisher. Preserve local packaging, existing updater trust, and distribution identities.
- Validation: shared engine tests, workflow lint, and prepared manifest/version checks. Native/hosted consumer release dry runs and publication remain unverified until the migration is merged and credentials/settings are checked.
