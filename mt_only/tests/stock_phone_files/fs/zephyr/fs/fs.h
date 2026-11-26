// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdio>
#include <dirent.h>
#include <sys/types.h>
struct fs_dirent {int type;size_t size;char name[256];};
struct fs_statvfs {unsigned long f_bsize,f_frsize,f_blocks,f_bfree;};
int fs_statvfs(const char *,struct fs_statvfs *);
struct fs_dir_t {DIR *value;};
void fs_dir_t_init(fs_dir_t *);
int fs_opendir(fs_dir_t *,const char *);
int fs_closedir(fs_dir_t *);
int fs_readdir(fs_dir_t *,fs_dirent *);
struct fs_file_t { FILE *file; };
enum { FS_DIR_ENTRY_DIR=1,FS_DIR_ENTRY_FILE,FS_O_CREATE=1,FS_O_WRITE=2,FS_O_APPEND=4,
       FS_O_READ=8,FS_O_TRUNC=16,FS_SEEK_SET=SEEK_SET };
void fs_file_t_init(fs_file_t *);
int fs_stat(const char *,fs_dirent *);
int fs_unlink(const char *);
int fs_open(fs_file_t *,const char *,int);
ssize_t fs_read(fs_file_t *,void *,size_t);
ssize_t fs_write(fs_file_t *,const void *,size_t);
int fs_sync(fs_file_t *);
int fs_close(fs_file_t *);
int fs_seek(fs_file_t *,off_t,int);
int fs_rename(const char *,const char *);
