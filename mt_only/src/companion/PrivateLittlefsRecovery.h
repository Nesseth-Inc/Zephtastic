// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included only in the pinned Zephyr LittleFS adapter. Its mutex serializes
// every call below. The same wrappers run on synthetic NOR in the host fixture.
#ifdef __ZEPHYR__
#define LFS_LOG_REGISTER
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/devicetree.h>
FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(mt_sat_fs));
#else
#include <lfs.h>
extern lfs_t *warmFs;
#endif

static inline bool zt_private_lfs(lfs_t *fs)
{
#ifdef __ZEPHYR__
	const struct fs_mount_t *mount = &FS_FSTAB_ENTRY(DT_NODELABEL(mt_sat_fs));
	return fs == &((struct fs_littlefs *)mount->fs_data)->lfs;
#else
	return fs == warmFs;
#endif
}

enum zt_lfs_health { ZT_LFS_OK, ZT_LFS_UNCERTAIN, ZT_LFS_DETACHED };
static enum zt_lfs_health zt_lfs_health;

// Do not reuse the program cache after a device I/O error. A damaged program
// can commit either side of a rename. Mount resolves it from durable bytes.
// Existing file and directory handles must first leave LittleFS's open list.
static int zt_lfs_ready(lfs_t *fs)
{
	if (!zt_private_lfs(fs) || zt_lfs_health == ZT_LFS_OK) {
		return 0;
	}
	if (zt_lfs_health == ZT_LFS_UNCERTAIN) {
		if (fs->mlist) {
			return LFS_ERR_IO;
		}
		int rc = lfs_unmount(fs);
		if (rc < 0) {
			return rc;
		}
		zt_lfs_health = ZT_LFS_DETACHED;
	}
	int rc = lfs_mount(fs, fs->cfg);
	if (rc == 0) {
		zt_lfs_health = ZT_LFS_OK;
	}
	return rc;
}

static int zt_lfs_result(lfs_t *fs, int rc)
{
	if (rc == LFS_ERR_IO && zt_private_lfs(fs)) {
		zt_lfs_health = ZT_LFS_UNCERTAIN;
	}
	return rc;
}

static int zt_lfs_close_file(lfs_t *fs, lfs_file_t *file)
{
	bool invalid = zt_private_lfs(fs) && zt_lfs_health != ZT_LFS_OK;
	if (invalid) {
		// lfs_file_close_ releases the cache and detaches this handle without
		// syncing when LFS_F_ERRED is set. A concurrent upload is abandoned.
		file->flags |= LFS_F_ERRED;
	}
	int rc = zt_lfs_result(fs, lfs_file_close(fs, file));
	return invalid ? LFS_ERR_IO : rc;
}

#ifdef __ZEPHYR__
static int zt_vfs_close(lfs_t *fs, lfs_file_t *file, struct fs_file_t *fp)
{
	int rc = zt_lfs_close_file(fs, file);
	if (rc < 0 && zt_private_lfs(fs)) {
		// The adapter always frees filep; prevent fs_close's error path from
		// leaving a previously closed Zephyr handle pointing to that storage.
		fp->mp = NULL;
	}
	return rc;
}
#define lfs_file_close(fs, file) zt_vfs_close(fs, file, fp)
#else
#define lfs_file_close(fs, file) zt_lfs_close_file(fs, file)
#endif

// The pinned adapter calls these operations with its existing fs_lock held.
// Close-dir only removes the handle from mlist and performs no media I/O.
#define ZT_LFS_CALL(fn, fs, ...) ({ lfs_t *zt_fs = (fs); int zt_rc = zt_lfs_ready(zt_fs); \
	zt_rc < 0 ? zt_rc : zt_lfs_result(zt_fs, (fn)(zt_fs, ##__VA_ARGS__)); })
#define lfs_file_opencfg(fs, ...) ZT_LFS_CALL(lfs_file_opencfg, fs, __VA_ARGS__)
#define lfs_file_read(fs, ...) ZT_LFS_CALL(lfs_file_read, fs, __VA_ARGS__)
#define lfs_file_write(fs, ...) ZT_LFS_CALL(lfs_file_write, fs, __VA_ARGS__)
#define lfs_file_seek(fs, ...) ZT_LFS_CALL(lfs_file_seek, fs, __VA_ARGS__)
#define lfs_file_tell(fs, ...) ZT_LFS_CALL(lfs_file_tell, fs, __VA_ARGS__)
#define lfs_file_truncate(fs, ...) ZT_LFS_CALL(lfs_file_truncate, fs, __VA_ARGS__)
#define lfs_file_sync(fs, ...) ZT_LFS_CALL(lfs_file_sync, fs, __VA_ARGS__)
#define lfs_remove(fs, ...) ZT_LFS_CALL(lfs_remove, fs, __VA_ARGS__)
#define lfs_rename(fs, ...) ZT_LFS_CALL(lfs_rename, fs, __VA_ARGS__)
#define lfs_mkdir(fs, ...) ZT_LFS_CALL(lfs_mkdir, fs, __VA_ARGS__)
#define lfs_dir_open(fs, ...) ZT_LFS_CALL(lfs_dir_open, fs, __VA_ARGS__)
#define lfs_dir_read(fs, ...) ZT_LFS_CALL(lfs_dir_read, fs, __VA_ARGS__)
#define lfs_stat(fs, ...) ZT_LFS_CALL(lfs_stat, fs, __VA_ARGS__)
#define lfs_fs_size(fs) ZT_LFS_CALL(lfs_fs_size, fs)
#define lfs_fs_gc(fs) ZT_LFS_CALL(lfs_fs_gc, fs)
#ifndef __ZEPHYR__
#define lfs_file_open(fs, ...) ZT_LFS_CALL(lfs_file_open, fs, __VA_ARGS__)
#endif
