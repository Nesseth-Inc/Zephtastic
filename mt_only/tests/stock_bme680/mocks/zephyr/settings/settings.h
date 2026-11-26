#pragma once
#include <sys/types.h>
#include <cstddef>
ssize_t settings_get_val_len(const char*);
ssize_t settings_load_one(const char*,void*,size_t);
int settings_save_one(const char*,const void*,size_t);
int settings_delete(const char*);
