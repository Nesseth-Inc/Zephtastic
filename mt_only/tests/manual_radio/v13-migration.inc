// SPDX-License-Identifier: GPL-3.0-only
// Root-adapted Qwen proposal; independently executed by the host suite.
void legacyConfigAssertions(const uint8_t *raw, size_t n)
{
    // NDEBUG-safe failure path: report and abort.
    auto fail = [](const char *what) -> int {
        std::fprintf(stderr, "legacyConfigAssertions: %s\n", what);
        std::abort();
    };

    // Precondition: the caller supplies the pinned valid v13 fixture.
    if (!raw || n != 576) fail("expected the 576-byte v13 fixture");
    if (raw[0] != 'M' || raw[1] != 'T' || raw[2] != 13)
        fail("raw is not a version-13 record");

    // 1. Successful decode of the valid v13 fixture.
    Config up;
    if (!Config::decode(raw, n, up))
        fail("decode of valid v13 fixture failed");

    // 2. Historical bytes migrate to the current 640-byte record. The pinned
    //    v13 producer stays unchanged; v15 adds consent in former reserved122.
    if (Config::Size != 640) fail("current record size is not 640 bytes");
    if (up.bytes[0] != 'M' || up.bytes[1] != 'T' || up.bytes[2] != Config::Version)
        fail("decoded record is not the current Config version");
    if (up.primaryPositionPrecision() != 0 || up.positionSharing())
        fail("unshared v13 fixture acquired position consent");

    // 3. Legacy body preserved: bytes 3..573 (571 bytes) unchanged.
    if (std::memcmp(up.bytes + 3, raw + 3, 571) != 0)
        fail("legacy bytes 3..573 were not preserved");

    // 4. Unspecified-power board defaults (defaults, not caps): -9 dBm on
    //    T096, +2 dBm on XIAO.
    if (up.effectivePower(true) != -9)
        fail("T096 unspecified-power default is not -9 dBm");
    if (up.effectivePower(false) != 2)
        fail("XIAO unspecified-power default is not +2 dBm");

    // 5. Failure path: flip a CRC byte in a local copy. The v13 CRC is the
    //    trailing pair (bytes 574..575); flip the first of the two.
    uint8_t bad[576];
    std::memcpy(bad, raw, 576);
    bad[574] ^= 0xFF;

    // Sentinel Config in a known state (0xA5 fill) so "unchanged" is a
    // non-trivial check.
    Config sentinel;
    for (size_t i = 0; i < Config::Size; ++i)
        sentinel.bytes[i] = 0xA5;
    Config before = sentinel;

    if (Config::decode(bad, 576, sentinel))
        fail("decode of CRC-flipped copy succeeded");
    if (std::memcmp(sentinel.bytes, before.bytes, Config::Size) != 0)
        fail("failed decode modified the sentinel Config");
}
