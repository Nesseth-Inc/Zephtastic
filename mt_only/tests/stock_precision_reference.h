// Unchanged function extracted from pinned stock 8e6a88d; GPL-3.0-only.
namespace stock_reference {
int32_t truncateCoordinate(int32_t coordinate, uint32_t precision)
{
    if (precision == 0 || precision >= 32)
        return coordinate;

    uint32_t coordinateBits = static_cast<uint32_t>(coordinate);
    uint32_t truncated = coordinateBits & (UINT32_MAX << (32 - precision));

    // Use the middle of the possible location, not the low edge of the bucket.
    truncated += (1UL << (31 - precision));

    return static_cast<int32_t>(truncated);
}
}
