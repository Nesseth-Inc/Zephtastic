#pragma once
#include <cstdint>
struct net_if {};
struct net_linkaddr {uint8_t len,addr[6];};
inline net_if *net_if_get_default(){return nullptr;}
inline const net_linkaddr *net_if_get_link_addr(net_if*){return nullptr;}
