# Channel meter and shared telemetry encoder acceptance

Run `run.py --output <directory outside the source tree>` and repeat with
`--short-enums` in a new output directory. The runner honors `CC` and `CXX` using
shell-style argument splitting, including compiler wrappers. It uses bounded
child process groups through the existing airtime qualification helper, Clang
or GCC compatible sanitizer options, and enabled C++ assertions.

The 18 scenario groups exercise the actual production ChannelMeter and shared
TelemetryQuery encoder. They cover present idle zero, known RX and exactly once
clean TX credits, lost terminal events, full minute uncertainty recovery,
retained known credits, unknown and regressing clocks, drain ordering, unsupported
coverage, fresh recovery after a long coverage gap, overflow aging, and optional
nanopb telemetry field validation. There is no encoder replica.

The legacy header is copied without changes from `mt_only/src/TelemetryQuery.h`
at M60d2 source commit `a44a8175462273aa53c475fc239ae600d16cf2bb`. Its exact
SHA-256 is `fd623e266712ea1d0a5557824b9481605865f673cff950bd713178285949d677`.
The runner verifies this digest, compiles the actual legacy encoder separately,
and compares 15 wire samples byte for byte with the current encoder. The
header's original GPL-3.0-only SPDX notice is preserved. Other include and
protobuf inputs come from the current source tree and are recorded by digest.

The output receipt records frozen input and compiler identities, exact commands,
scenario results, and child process cleanup. Native queue concurrency, Service
and main call sites, target linkage, physical RF, and firmware installation are
outside this fixture's scope.
