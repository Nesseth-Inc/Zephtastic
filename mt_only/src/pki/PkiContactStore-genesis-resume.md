# Explicit interrupted genesis completion

`PkiContactStore::load()` remains read-only and fail-closed for a lone generation-one slot. The explicit `resumeGenesis()` seam is not called by load or boot code. Its caller must provide the current validated identity/root tuple and the independently replayed effective legacy contact list.

Resume is restricted to an `Unloaded` or `Frozen` instance and requires slot zero to match the complete canonical generation-one record for that exact identity, root, and contact list. Slot one must return exactly `-ENOENT`. The method then performs one write of the matching generation-two record, exact readback and decode, and a final reread/decode of both adjacent generations before setting `Ready`. A mismatch, present slot one, malformed record, read error, or generation other than one performs no write. Any attempted-write or post-write verification uncertainty freezes the instance.

This is not a general repair operation: it cannot initialize an empty store, remove/replace records, roll a newer snapshot back, change peer domains, or make contacts ready from an incomplete journal. The API does not prove provenance of the supplied peer list; the future caller must derive it by fully replaying and validating the immutable legacy journal under its current identity/root before invoking this explicit method.
