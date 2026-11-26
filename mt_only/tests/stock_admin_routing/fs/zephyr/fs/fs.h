// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <sys/types.h>
struct fs_dirent {int type;size_t size;};
struct fs_file_t {int unused;};
enum {FS_DIR_ENTRY_DIR=1,FS_DIR_ENTRY_FILE,FS_O_CREATE=1,FS_O_WRITE=2,FS_O_APPEND=4};
void fs_file_t_init(fs_file_t *);
int fs_stat(const char *,fs_dirent *);
int fs_unlink(const char *);
int fs_open(fs_file_t *,const char *,int);
ssize_t fs_write(fs_file_t *,const void *,size_t);
int fs_sync(fs_file_t *);
int fs_close(fs_file_t *);
