The complete Default.cpp and Default.h files are byte-identical official stock
8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c sources; PROVENANCE.json pins their
original path, bytes, SHA256 and Git blob. Both literal normal and event branches
compile. Host stubs explicitly supply role, valid resolved SF/BW, preset/manual
resolver results and regional throttle. They do not reproduce hardware resolution,
node discovery, config minimum-interval policy, module scheduling or RF.

The real ModuleIntervals, NeighborIntervals and NeighborReplyWindow headers are
used. Valid resolved interval vectors compare to the literal seconds/scaler/
throttle bodies. A dedicated vector documents a deliberate conservative edge:
seconds1984882, online42, SF5, BW7.8125 normal (BW31.25 event) have a double product
below INT32_MAX that rounds to2^31 in float. Stock returns uint32_t2^31; ZT clamps
to INT32_MAX. No universal out-of-range bit identity is claimed. Unknown online
refusal, accepted replies at0 and failed allocation/enqueue not consuming the
reply window are local wrapper contracts rather than extracted stock behavior.
