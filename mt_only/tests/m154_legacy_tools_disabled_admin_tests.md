# M154 disabled legacy-admin test

`m154_legacy_tools_disabled_admin_tests.cpp` includes the existing M154
preserved-root fixture. Compile **all** translation units with
`MT_PKI_LEGACY_TAIL_TOOLS_NATIVE_DISABLED=1`. The former store/lifecycle
enable APIs must return `-ENOTSUP` before lock, pause, or storage activity. The
compact-contact administration API must still migrate a valid bounded store,
preserving the identity, nonce records/high-water, and append bytes.

Run it through the pinned native runner with this file as the case source and a
local isolated output directory. It is host-only evidence and does not test
the Zephyr USB dispatcher or a target profile build.
