#pragma once
#include <lfs.h>
#include <sys/types.h>
#if defined(CONFIG_MT_PAGED_SATELLITES)
#include "companion/PrivateLittlefsRecovery.h"
#endif
using fs_file_t=lfs_file_t;
extern lfs_t *warmFs;
inline const char *warmName(const char *p){return p+sizeof("/zt-history/")-1;}
inline void fs_file_t_init(fs_file_t *f){*f={};}
#define FS_O_READ LFS_O_RDONLY
#define FS_O_CREATE LFS_O_CREAT
#define FS_O_TRUNC LFS_O_TRUNC
#define FS_O_WRITE LFS_O_WRONLY
inline int fs_open(fs_file_t *f,const char *p,int mode){return lfs_file_open(warmFs,f,warmName(p),mode);}
inline ssize_t fs_read(fs_file_t *f,void *p,size_t n){return lfs_file_read(warmFs,f,p,n);}
inline ssize_t fs_write(fs_file_t *f,const void *p,size_t n){return lfs_file_write(warmFs,f,p,n);}
inline int fs_close(fs_file_t *f){return lfs_file_close(warmFs,f);}
inline int fs_sync(fs_file_t *f){return lfs_file_sync(warmFs,f);}
inline int fs_rename(const char *p,const char *q){return lfs_rename(warmFs,warmName(p),warmName(q));}
inline int fs_unlink(const char *p){return lfs_remove(warmFs,warmName(p));}

using fs_dirent=lfs_info;
inline int fs_stat(const char *p,fs_dirent *entry){return lfs_stat(warmFs,warmName(p),entry);}
