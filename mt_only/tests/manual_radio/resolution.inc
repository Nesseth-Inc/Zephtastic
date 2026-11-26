using Request=mt::RadioSettings::Request;
// Qwen assertion proposal, adapted to actual nested API/member names and fieldwise output canaries.
static void AssertM96RadioSettingsResolution() {
  // Sentinel outputs: a failed resolve must leave these byte-for-byte unchanged.
  const LoRaPresets::Profile profileSentinel{0xa5a5a5a5u,0xa5a5a5a5u,0xa5u,0xa5u};
  const USChannelSelection selectionSentinel{0xa5a5u,0xa5a5u,0xa5a5a5a5u,0xa5u};
  const auto preserved=[&](const LoRaPresets::Profile&p,const USChannelSelection&s){
    return p.bandwidthHz==profileSentinel.bandwidthHz&&p.wideBandwidthHz==profileSentinel.wideBandwidthHz&&
      p.spreadingFactor==profileSentinel.spreadingFactor&&p.codingDenominator==profileSentinel.codingDenominator&&
      s.slotCount==selectionSentinel.slotCount&&s.slot==selectionSentinel.slot&&s.frequencyHz==selectionSentinel.frequencyHz&&s.regionCode==selectionSentinel.regionCode;
  };

  // Accepted preset IDs are exactly 0,1,3,4,5,6,7,8,9,16. LoRaPresets::lookup
  // has rows 0..16, but RadioSettings rejects 2 and 10..15.
  static const unsigned accepted[] = {0, 1, 3, 4, 5, 6, 7, 8, 9, 16};
  static const unsigned rejected[] = {2, 10, 11, 12, 13, 14, 15};
  for (unsigned p : accepted) assert(RadioSettings::presetSupported(p));
  for (unsigned p : rejected) assert(!RadioSettings::presetSupported(p));

  // 1. LongFast (preset 0) + configured denominator 8 (in 5..8 and > preset
  //    denominator 5): profile denominator is raised to the configured value.
  {
    Request req;                    // defaults: usePreset=true, channel=20, power=0
    req.preset = 0;
    req.codingRate = 8;
    LoRaPresets::Profile profile{};
    USChannelSelection selection{};
    assert(RadioSettings::resolve(req, profile, selection, "LongFast"));
    assert(profile.bandwidthHz == 250000u);
    assert(profile.spreadingFactor == 11u);
    assert(profile.codingDenominator == 8u);
  }

  // 2. Configured denominator 4 is outside 5..8: it must NOT override the
  //    preset's CR5.
  {
    Request req;
    req.preset = 0;
    req.codingRate = 4;
    LoRaPresets::Profile profile{};
    USChannelSelection selection{};
    assert(RadioSettings::resolve(req, profile, selection, "LongFast"));
    assert(profile.bandwidthHz == 250000u);
    assert(profile.spreadingFactor == 11u);
    assert(profile.codingDenominator == 5u);
  }

  // 3. Manual mode: bandwidth 250 (kHz) / SF9 / CR7 -> 250000 Hz / SF9 / CR7.
  {
    Request req;
    req.usePreset = false;
    req.bandwidth = 250;
    req.spreadingFactor = 9;
    req.codingRate = 7;
    LoRaPresets::Profile profile{};
    USChannelSelection selection{};
    assert(RadioSettings::resolve(req, profile, selection, "LongFast"));
    assert(profile.bandwidthHz == 250000u);
    assert(profile.spreadingFactor == 9u);
    assert(profile.codingDenominator == 7u);
  }

  // 4. Preset 2 has a lookup row but is not accepted by RadioSettings: resolve
  //    fails and both outputs stay at the sentinel byte-for-byte.
  {
    Request req;
    req.preset = 2;
    LoRaPresets::Profile profile = profileSentinel;
    USChannelSelection selection = selectionSentinel;
    assert(!RadioSettings::resolve(req, profile, selection, "LongFast"));
    assert(preserved(profile, selection));
  }

  // 5. Manual mode with unsupported bandwidth code 126 (an unsupported code,
  //    not a stock-region issue): resolve fails, outputs stay at the sentinel.
  {
    Request req;
    req.usePreset = false;
    req.bandwidth = 126;
    req.spreadingFactor = 9;
    req.codingRate = 7;
    LoRaPresets::Profile profile = profileSentinel;
    USChannelSelection selection = selectionSentinel;
    assert(!RadioSettings::resolve(req, profile, selection, "LongFast"));
    assert(preserved(profile, selection));
  }
}
