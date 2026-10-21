# Changelog

### 0.4.0: Protel and Elcotel groundwork, compatibility matrix

- Add `mm_payphone.py` and the `payphones` package: a backend boundary for families other than the Nortel Millennium, which mm_manager keeps handling as before.
- Protel: a configuration model (cost bands, area code and exchange bands, options) checked against the ExpressNet manual's limits; reading ExpressNet export files into inventory entries; ExpressNet communication errors explained by session phase. Native programming and reporting are not implemented: the protocol is not public and has not been captured yet (docs/protel.md).
- Elcotel Series 5: clean-room reading, checking and writing of R94, S94 and P94/P99 files.
- Session captures with the metadata research needs, transcript import and comparison.
- A compatibility matrix (config/compatibility.csv, docs/compatibility.md) listing every operation for every board as planned, experimental or hardware verified. Nothing for Protel or Elcotel is hardware verified.

### Release infrastructure migration — 2026-10-20

- Adopt the unified release manifest, version-advance validation, manual main-only planning, complete artifact staging, and verified draft publisher. Preserve local packaging, existing updater trust, and distribution identities.
- Validation: shared engine tests, workflow lint, and prepared manifest/version checks. Native/hosted consumer release dry runs and publication remain unverified until the migration is merged and credentials/settings are checked.
