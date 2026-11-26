// SPDX-License-Identifier: GPL-3.0-only
#include "modules/SameChannelBeaconPayloads.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/mesh_beacon.pb.h"
#include "meshtastic/portnums.pb.h"
#include <pb_decode.h>
#include <assert.h>
#include <string.h>

using namespace mt::beacon;

static bool decodeBeacon(const meshtastic_Data &d, meshtastic_MeshBeacon &decoded) {
  decoded = meshtastic_MeshBeacon_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(d.payload.bytes, d.payload.size);
  return pb_decode(&stream, &meshtastic_MeshBeacon_msg, &decoded);
}

static void expectText(const meshtastic_Data &d, const char *text, size_t len) {
  assert(d.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP);
  assert(d.want_response == false);
  assert(d.payload.size == len);
  assert(memcmp(d.payload.bytes, text, len) == 0);
}

int main() {
  meshtastic_MeshBeacon offer = meshtastic_MeshBeacon_init_zero; // sanitized
  offer.has_offer_channel = true;
  offer.offer_channel = meshtastic_ChannelSettings_init_zero;
  meshtastic_MeshBeacon zeroOffer = meshtastic_MeshBeacon_init_zero;
  char hello[61] = "hello";
  char ping[61] = "ping";
  char empty[61] = {};
  BeaconPayloadBatch out{};

  // (1) text-only: exactly one TEXT port, raw bytes, want_response=false, even split=true.
  assert(composePayloads(hello, false, zeroOffer, false, out));
  assert(out.count == 1);
  expectText(out.packets[0], "hello", 5);
  assert(composePayloads(hello, false, zeroOffer, true, out));
  assert(out.count == 1);
  expectText(out.packets[0], "hello", 5);

  // (2) offer-only: one decodable MESH_BEACON with empty message.
  assert(composePayloads(empty, true, offer, false, out));
  assert(out.count == 1);
  assert(out.packets[0].portnum == meshtastic_PortNum_MESH_BEACON_APP);
  meshtastic_MeshBeacon decoded;
  assert(decodeBeacon(out.packets[0], decoded));
  assert(decoded.message[0] == '\0');
  assert(decoded.has_offer_channel);

  // (3) offer+text: split=false combines into one beacon; split=true beacon first, then raw TEXT.
  assert(composePayloads(ping, true, offer, false, out));
  assert(out.count == 1);
  assert(out.packets[0].portnum == meshtastic_PortNum_MESH_BEACON_APP);
  assert(decodeBeacon(out.packets[0], decoded));
  assert(strcmp(decoded.message, "ping") == 0);
  assert(decoded.has_offer_channel);
  assert(composePayloads(ping, true, offer, true, out));
  assert(out.count == 2);
  assert(out.packets[0].portnum == meshtastic_PortNum_MESH_BEACON_APP);
  assert(decodeBeacon(out.packets[0], decoded));
  assert(decoded.message[0] == '\0');
  expectText(out.packets[1], "ping", 4);

  // (4) 60-byte UTF-8 input is counted by bytes; rejections leave out unchanged.
  char utf8[61];
  for (int i = 0; i < 60; i += 2) { utf8[i] = (char)0xC3; utf8[i + 1] = (char)0xA9; }
  utf8[60] = '\0'; // 30 x U+00E9 (2 bytes each): 60 bytes, terminated
  assert(composePayloads(utf8, false, zeroOffer, false, out));
  assert(out.count == 1);
  expectText(out.packets[0], utf8, 60);
  BeaconPayloadBatch saved = out;
  assert(!composePayloads(empty, false, zeroOffer, false, out));
  char full[61];
  memset(full, 0xE9, sizeof(full)); // no NUL anywhere; full[60] != '\0'
  assert(!composePayloads(full, false, zeroOffer, false, out));
  assert(out.count == saved.count && out.packets[0].portnum == saved.packets[0].portnum &&
         out.packets[0].want_response == saved.packets[0].want_response);
  assert(out.packets[0].payload.size == saved.packets[0].payload.size &&
         memcmp(out.packets[0].payload.bytes, saved.packets[0].payload.bytes,
                saved.packets[0].payload.size) == 0);
  return 0;
}
