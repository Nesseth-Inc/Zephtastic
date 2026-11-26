// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace mt {
#if defined(CONFIG_MT_SERIAL_MODULE)
inline constexpr unsigned LocalClientCount=4;
#elif defined(CONFIG_MT_TCP_COMPANION)
inline constexpr unsigned LocalClientCount=3;
#else
inline constexpr unsigned LocalClientCount=2;
#endif
static_assert(LocalClientCount<=8,"owner masks are uint8_t");
inline constexpr unsigned LocalClientMask=(1u<<LocalClientCount)-1u;
}
