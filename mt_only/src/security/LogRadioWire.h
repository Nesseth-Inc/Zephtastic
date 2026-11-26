// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ApiLogBuffer.h"
#include "../BleTransport.h"
#include <pb_encode.h>

namespace mt::api {
// RedirectablePrint::log_to_ble in Meshtastic 8e6a88d sends a bare LogRecord.
// Only the existing typed event ring can supply message, source and severity.
template<class LocalClock>
bool encodeLogRadio(const ApiLogBuffer &source,meshtastic_LogRecord &scratch,
                    ble::LogRadioValue &out,LocalClock localTime) {
  out.size=0;
  if(!source.latest(scratch))return false;
  scratch.time=localTime(scratch.time);
  auto stream=pb_ostream_from_buffer(out.bytes,sizeof(out.bytes));
  if(!pb_encode(&stream,meshtastic_LogRecord_fields,&scratch))return false;
  out.size=uint8_t(stream.bytes_written);return out.size!=0;
}
}
