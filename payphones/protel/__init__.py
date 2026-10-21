"""Protel ExpressNet-generation payphones.

What works without a phone: the configuration model (cost bands, area code
and exchange assignments, options) checked against the ExpressNet manual's
limits, reading ExpressNet export files into the inventory, and explaining
ExpressNet communication errors by session phase.

The native protocol is not implemented. It is not publicly documented, and
no captures of it have been made yet; see docs/protel.md for the route to
it. program() and receive_reports() therefore raise NotSupported.
"""

from .. import backend
from . import catalog, config, xnet_import


@backend.register
class ProtelBackend(backend.Backend):
    name = "protel"
    family = "Protel"
    title = "Protel (ExpressNet generation)"

    def identify(self, evidence):
        model = str(evidence.get("model", ""))
        firmware = str(evidence.get("firmware", ""))
        details = {}
        if model in catalog.MODELS:
            details["model_family"] = catalog.MODELS[model]["family"]
            if catalog.MODELS[model]["family"] != "expressnet":
                details["note"] = "ProNet-era boards are a separate family; this backend does not cover them"
        # ExpressNet shows firmware as e.g. CD8715-2412R: the type is its first two letters.
        fw_type = firmware[:2].upper()
        if fw_type in catalog.FIRMWARE_TYPES:
            details["firmware_type"] = fw_type
        return backend.Identification(self.family, board=model, firmware=firmware,
                                      confidence="claimed" if model or firmware else "unknown",
                                      details=details)

    def prepare(self, cfg, board="", firmware=""):
        fw_type = (firmware or cfg.get("firmware", ""))[:2].upper()
        problems, unsupported, losses = config.check(cfg, firmware=fw_type or None, model=board or None)
        if board in catalog.MODELS and catalog.MODELS[board]["family"] != "expressnet":
            problems.append("%s is a ProNet-era board, not an ExpressNet one" % board)
        return backend.PreparedConfiguration(cfg, unsupported=unsupported, losses=losses, problems=problems)

    def program(self, phone_id, prepared, connection):
        raise backend.NotSupported("native Protel programming is not implemented yet: the protocol has not "
                                   "been captured. Use ExpressNet for now (see docs/protel.md).")

    def receive_reports(self, phone_id, connection):
        raise backend.NotSupported("Protel reporting is not implemented yet (see docs/protel.md)")

    def session_result(self, phone_id, operation, error_code=None):
        """A SessionResult from the outcome ExpressNet reported (an error code, or None for success)."""
        if error_code is None:
            # ExpressNet's success says the phone accepted the download; it was not read back.
            return backend.SessionResult(self.name, phone_id, operation, "complete", verification="acknowledged")
        info = catalog.explain_error(error_code)
        outcome = "partial" if int(error_code) in catalog.ACKNOWLEDGED_BUT_UNVERIFIED else "failed"
        return backend.SessionResult(self.name, phone_id, operation, outcome,
                                     verification="acknowledged" if outcome == "partial" else "none",
                                     error_code=int(error_code), phase=info["phase"],
                                     messages=[info["description"]])
