// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
ssize_t settings_get_val_len(const char *key);
ssize_t settings_load_one(const char *key,void *value,size_t capacity);
int settings_delete(const char *key);
int settings_save_one(const char *key,const void *value,size_t length);
#ifdef __cplusplus
}
#endif
