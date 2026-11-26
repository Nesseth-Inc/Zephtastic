// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Nanopb's descriptor format supports mixing field widths in one message.
// Choose each width from the actual target ABI, using nanopb's encoders and
// field-offset macros. The iterator is bounded by field_count. An empty
// message keeps one readable zero for pb_field_iter_begin_extension().
namespace mt::descriptor {

struct Field {
    uint32_t tag, type, offset, size;
    int32_t sizeOffset;
    uint32_t count;

    constexpr unsigned width() const {
        if (tag < 64 && offset < 256 && size < 16 && sizeOffset >= 0 && sizeOffset < 16 && count == 1)
            return 1;
        if (tag < 1024 && offset < 65536 && size < 4096 && sizeOffset >= 0 && sizeOffset < 16 && count < 4096)
            return 2;
        return 4;
    }

    constexpr void emit(uint32_t *out) const {
        if (width() == 1) {
            const uint32_t words[] = { PB_FIELDINFO_1(tag, type, offset, size, sizeOffset, count) };
            out[0] = words[0];
        } else if (width() == 2) {
            const uint32_t words[] = { PB_FIELDINFO_2(tag, type, offset, size, sizeOffset, count) };
            out[0] = words[0];
            out[1] = words[1];
        } else {
            const uint32_t words[] = { PB_FIELDINFO_4(tag, type, offset, size, sizeOffset, count) };
            for (size_t i = 0; i < 4; ++i) out[i] = words[i];
        }
    }
};

template <size_t N>
constexpr size_t wordCount(const Field (&fields)[N]) {
    size_t words = N == 1 ? 1 : 0;
    for (size_t i = 0; i + 1 < N; ++i) words += fields[i].width();
    return words;
}

template <size_t N>
struct Table { uint32_t words[N]; };

template <size_t Words, size_t N>
constexpr Table<Words> encode(const Field (&fields)[N]) {
    Table<Words> result{};
    size_t offset = 0;
    for (size_t i = 0; i + 1 < N; ++i) {
        fields[i].emit(result.words + offset);
        offset += fields[i].width();
    }
    return result;
}

static constexpr const pb_msgdesc_t *emptySubmessages[] = {nullptr};
} // namespace mt::descriptor

#define MT_DESCRIPTOR_FIELD(st, atype, htype, ltype, name, tag) \
    {tag, PB_ATYPE_ ## atype | PB_HTYPE_ ## htype | PB_LTYPE_MAP_ ## ltype, \
     PB_DATA_OFFSET_ ## atype(_PB_HTYPE_ ## htype, st, name), \
     PB_DATA_SIZE_ ## atype(_PB_HTYPE_ ## htype, st, name), \
     PB_SIZE_OFFSET_ ## atype(_PB_HTYPE_ ## htype, st, name), \
     PB_ARRAY_SIZE_ ## atype(_PB_HTYPE_ ## htype, st, name)},

#undef PB_BIND
#define PB_BIND(msgname, st, width) \
    static constexpr mt::descriptor::Field st ## _layout[] = { \
        msgname ## _FIELDLIST(MT_DESCRIPTOR_FIELD, st) {} }; \
    static constexpr auto st ## _table = \
        mt::descriptor::encode<mt::descriptor::wordCount(st ## _layout)>(st ## _layout); \
    static constexpr const pb_msgdesc_t *st ## _submessages[] = { \
        msgname ## _FIELDLIST(PB_GEN_SUBMSG_INFO, st) nullptr }; \
    const pb_msgdesc_t st ## _msg = { \
        st ## _table.words, \
        sizeof(st ## _submessages) == sizeof(void *) ? mt::descriptor::emptySubmessages : st ## _submessages, \
        msgname ## _DEFAULT, msgname ## _CALLBACK, \
        0 msgname ## _FIELDLIST(PB_GEN_FIELD_COUNT, st), \
        0 msgname ## _FIELDLIST(PB_GEN_REQ_FIELD_COUNT, st), \
        0 msgname ## _FIELDLIST(PB_GEN_LARGEST_TAG, st) }; \
    msgname ## _FIELDLIST(PB_GEN_FIELD_INFO_ASSERT_4, st)
