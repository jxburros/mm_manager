\*\*mmumblephone expansion plan\*\*



Build one application for managing a mixed payphone collection: identify phones, configure supported features, program them, collect reports, and help connect them for working calls.



Prioritize North American equipment initially, while making additional manufacturers and international models possible through separate backends.



\*\*Proposed coverage order\*\*



| Priority | Family | Initial objective |

|---|---|---|

| 1 | Nortel Millennium | Establish hardware-tested reliability for the existing manager, SIP modem, and update features. |

| 2 | Protel ExpressNet generation | Implement native programming for one representative board, then expand across compatible board/firmware combinations. |

| 3 | Elcotel Series 5 and related boards | Add configuration-file support, followed by native communication and reporting. |

| 4 | Protel ProNet / Elite / Ascension | Investigate and implement these as a separate compatibility family. |

| 5 | Western Electric/AT&T and other network-controlled coin phones | Integrate external coin controllers and PBX operation. |

| 6 | Intellicall, Ernest, older boards, and international equipment | Prioritize according to available hardware, documentation, captures, and likely reuse. |

| Optional | Phones with replacement electronics | Integrate compatible modern controllers while identifying them separately from original-board support. |



Elcotel already has public configuration-file tools in \[elcotel-playground\](https://github.com/vekejsn/elcotel-playground). External-controller research can start with \[1dcoinctrl\](https://github.com/hharte/1dcoinctrl) and \[PhreakScript\](https://github.com/InterLinked1/phreakscript). \[OpenPayphone\](https://github.com/eric-cgn/openpayphone) is a reference for replacement electronics; it does not provide native management of the original Ernest board.



1\. \*\*Establish the baseline and collection inventory.\*\*



   Record manufacturer, installed board, board revision, firmware, modem options, coin mechanism, power requirements, and available programming equipment. Identify each device independently of its telephone number, since numbers can change or be reused.



   For the existing Nortel implementation, preserve working configurations and run the documented software checks. Verify SIP communication, programming, updates, and reporting against actual phones. The fork currently documents hardware-testing gaps in its software modem and experimental update behavior. \[Existing limitations\](https://github.com/jxburros/mm\_manager#status-and-limitations)



   \*\*Completion gate:\*\* a repeatable baseline and an inventory showing which representative boards are available for testing.



2\. \*\*Introduce manufacturer backends without disrupting Nortel support.\*\*



   Separate four responsibilities: connection/audio transport, modem implementation, manufacturer protocol, and board/firmware capabilities.



   Keep the current Nortel engine intact initially. Add a backend interface for identifying a phone, preparing configuration, programming supported settings, receiving reports, and reporting session results. Separate worker processes are a practical first implementation.



   Extend the browser controls to select a backend and connection profile. Initially route incoming calls through explicitly configured endpoints; investigate automatic protocol detection only where reliable identification is demonstrated.



   Add a common collection catalog while retaining existing Nortel files and records through an adapter. Include export, backup, and migration verification.



   \*\*Completion gate:\*\* the existing Nortel workflow still works, and a second backend can appear in the application without pretending to support programming.



3\. \*\*Build a capture and protocol research workbench.\*\*



   Add tools to import serial transcripts, retain original audio captures, compare sessions, annotate messages, and replay captured data through offline decoders.



   Every recording should include board, firmware, connection equipment, modem mode, call direction, configuration changes, and outcome. Capture sessions that change one setting at a time.



   Separate modem failures from protocol failures by using a known-working physical modem/reference setup where available. A software modem can come later for each newly understood mode.



   \*\*Completion gate:\*\* reproducible captures and offline decoding for the first Protel target. If reference equipment is unavailable, the framework and analysis tools can proceed, while hardware-dependent protocol work remains explicitly pending.



4\. \*\*Deliver the first native Protel backend.\*\*



   Begin with whichever ExpressNet-compatible board has the strongest working reference setup. Investigate identification, session establishment, framing, acknowledgements, retries, and configuration transfer.



   Start with a small useful configuration set: rates/free calling, supported dialing restrictions, incoming-call behavior, and relevant operating options. Add reporting as its messages become understood.



   Prefer phone-initiated sessions initially. Add outgoing polling afterward. Expand modem support according to the boards being tested: Bell 103, compatible standard DPSK modes, and Protel-specific FSK. The 7000’s modem requirements need their own investigation. \[Published modem testing\](https://telesfor.org/payphones/lib/exe/fetch.php?media=smart%3Aprotel%3Aprogramming\_protel\_7000.pdf)



   ExpressNet import/export may provide an interim configuration bridge, but native operation remains the objective. Firmware flashing is a later capability.



   \*\*Completion gate:\*\* mmumblephone independently programs and verifies useful settings on one physical Protel board, with repeatable results and documented limitations.



5\. \*\*Expand Protel coverage and add Elcotel.\*\*



   Test additional Protel boards and firmware editions individually. Reuse protocol behavior only when captures and hardware checks justify it.



   Treat ProNet-era equipment separately until compatibility is established; the Elite manual explicitly specifies ProNet for computer communication. \[Elite documentation\](https://payphones.telesfor.org/lib/exe/fetch.php?media=smart%3Aprotel%3Aprotel\_elite\_series\_users\_guide.pdf)



   For Elcotel, begin with reading, validating, and generating supported configuration files. Native transfer is a separate milestone. Evaluate third-party licensing before importing code into the MIT-licensed core.



   \*\*Completion gate:\*\* a published compatibility matrix identifies tested operations for every supported board/firmware combination.



6\. \*\*Make mixed-fleet management practical.\*\*



   Provide one phone list, manufacturer-specific settings, reusable configurations, scheduled jobs, update history, reports, and session diagnostics.



   Shared settings should represent matching behavior. Preserve manufacturer-specific settings and show unsupported fields or conversion losses before applying a configuration.



   Add queues and connection-resource ownership. Multiple phones may share a manager, but simultaneous sessions require sufficient modem channels or SIP workers. Prevent two jobs from taking the same physical modem.



   Bulk updates should preview affected phones and configuration differences, then report results separately for each phone.



   \*\*Completion gate:\*\* Nortel and Protel phones can be managed together, and one failed session does not interrupt unrelated work.



7\. \*\*Extend call operation and additional families.\*\*



   Integrate PBX and external-controller workflows for phones whose coin operation depends on network equipment. Verify dialing, ringing, answer supervision, and coin collection/refund where the hardware supports them.



   Rank further backends by available test phones, reference software, documentation, protocol evidence, and how many boards the work could unlock. Include guided local-programming instructions for equipment without remote management.



   \*\*Completion gate:\*\* each additional family has a concrete deliverable and physical verification before being advertised as supported.



For every model, show support separately for \*\*inventory, file editing, communication, programming, reporting, call/coin operation, and firmware updates\*\*. Mark each operation as \*\*planned, experimental, or hardware verified\*\*, with the tested board, firmware, connection setup, and known limitations.



A successful download acknowledgement should be recorded accurately. Where supported, verify by reading settings back; otherwise use documented behavioral checks. Preserve original configurations wherever they can be obtained, and distinguish an application backup from a complete backup of the phone.



The first implementation tranche should cover the inventory, Nortel baseline, backend boundary, and capture workbench. Its deliverable is the foundation for broad support plus a concrete route to the first native Protel implementation. Subsequent releases should advance according to demonstrated compatibility rather than a fixed promise to support every board.
