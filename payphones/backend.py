"""The manufacturer backend boundary.

A backend knows one family of payphones. It identifies a phone from what is
known about it, prepares a configuration (saying which fields it cannot
apply and what would be lost converting it), programs it, receives its
reports and returns a SessionResult for each session.

The Nortel Millennium engine is mm_manager itself and is not routed through
this layer; NortelBackend only describes it, so the families can be listed
together. A backend whose programming is not implemented says so: calling
program() raises NotSupported rather than pretending.
"""

from . import support


class NotSupported(Exception):
    """The backend does not implement this operation (yet)."""


class Identification:
    def __init__(self, family, board="", firmware="", confidence="unknown", details=None):
        self.family = family
        self.board = board
        self.firmware = firmware
        # "unknown", "claimed" (from the inventory or a file), or "observed" (from the phone itself)
        self.confidence = confidence
        self.details = details or {}

    def as_dict(self):
        return dict(family=self.family, board=self.board, firmware=self.firmware,
                    confidence=self.confidence, details=self.details)


class PreparedConfiguration:
    """A configuration checked against what one board/firmware can take."""

    def __init__(self, data, unsupported=None, losses=None, problems=None):
        self.data = data
        self.unsupported = unsupported or []    # fields this board/firmware has no place for
        self.losses = losses or []              # values that change on conversion
        self.problems = problems or []          # values that are invalid

    @property
    def ok(self):
        return not self.problems

    def as_dict(self):
        return dict(data=self.data, unsupported=self.unsupported, losses=self.losses, problems=self.problems)


# How the outcome of a download was established, from weakest to strongest.
# An acknowledgement only says the phone accepted the data.
VERIFICATION = ("none", "acknowledged", "behavior_checked", "read_back")


class SessionResult:
    def __init__(self, backend, phone_id, operation, outcome, verification="none",
                 error_code=None, phase=None, messages=None):
        if verification not in VERIFICATION:
            raise ValueError("verification must be one of %s" % ", ".join(VERIFICATION))
        self.backend = backend
        self.phone_id = phone_id
        self.operation = operation
        self.outcome = outcome            # "complete", "failed", "partial", "not_attempted"
        self.verification = verification
        self.error_code = error_code
        self.phase = phase
        self.messages = messages or []

    def as_dict(self):
        return dict(backend=self.backend, phone_id=self.phone_id, operation=self.operation,
                    outcome=self.outcome, verification=self.verification,
                    error_code=self.error_code, phase=self.phase, messages=self.messages)


class Backend:
    name = ""
    family = ""
    title = ""

    def capabilities(self, matrix=None):
        """This family's rows of the compatibility matrix."""
        if matrix is None:
            matrix = support.load_matrix()
        return [e for e in matrix if e.family == self.family]

    def identify(self, evidence):
        raise NotSupported("%s cannot identify phones" % self.title)

    def prepare(self, config, board="", firmware=""):
        raise NotSupported("%s cannot prepare configurations" % self.title)

    def program(self, phone_id, prepared, connection):
        raise NotSupported("%s cannot program phones" % self.title)

    def receive_reports(self, phone_id, connection):
        raise NotSupported("%s cannot receive reports" % self.title)


class NortelBackend(Backend):
    """Nortel Millennium, handled by mm_manager itself."""
    name = "nortel"
    family = "Nortel Millennium"
    title = "Nortel Millennium (mm_manager)"

    def identify(self, evidence):
        # mm_manager knows each terminal by the terminal ID it sends, and its type from phone_status.
        tid = str(evidence.get("terminal_id", ""))
        return Identification(self.family, board=evidence.get("terminal_type", ""),
                              firmware=evidence.get("firmware", ""),
                              confidence="observed" if evidence.get("terminal_type") else "claimed",
                              details={"terminal_id": tid})

    def program(self, phone_id, prepared, connection):
        raise NotSupported("Millennium terminals are programmed by mm_manager when they call in; "
                           "create tables/%s/update_pending to send every table on the next call" % phone_id)


_BACKENDS = {}


def register(backend_class):
    _BACKENDS[backend_class.name] = backend_class
    return backend_class


def backends():
    from . import protel, elcotel  # noqa: F401  (registers them)
    return {name: cls() for name, cls in _BACKENDS.items()}


def get(name):
    b = backends()
    if name not in b:
        raise KeyError("no backend %r (have %s)" % (name, ", ".join(sorted(b))))
    return b[name]


register(NortelBackend)
