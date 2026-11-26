// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "modules/RangeTestLog.h"
#include <zephyr/fs/fs.h>
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
#include "PublicTree.h"
#endif
namespace mt::stock::files {
class RangeFile {
    modules::RangeTestLog &log_;
    fs_file_t handle_{};
    bool upload_=false,replacingCsv_=false;
    uint8_t pendingReplacement_=0;
    static_assert(modules::RangeTestLog::QueueCapacity<=UINT8_MAX);
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    char destination_[PublicTree::PhysicalSize]{};
#endif
public:
    explicit RangeFile(modules::RangeTestLog &log):log_(log){}
    int info(uint32_t &bytes);
    int open(bool upload);
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    int open(bool upload,const char *name,size_t length);
#endif
    int read(uint32_t offset,uint8_t *out,size_t n);
    int write(const uint8_t *data,size_t n);
    int close(bool commit);
};
}
