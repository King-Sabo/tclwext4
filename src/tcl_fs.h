/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_fs.h - filesystem abstraction.
 *
 * Every WFX entry point now speaks this API instead of calling lwext4 (or
 * FatFs) directly, so the Total Commander plumbing - progress callbacks, abort
 * handling, read-only gating - is written once and shared.
 *
 * All paths in this API are Total Commander paths: wide, backslash-separated,
 * starting with the volume label, e.g. "\Disk0p2\boot\grub.cfg". Each backend
 * converts to whatever its library wants (UTF-8 with a mount prefix for
 * lwext4, "N:/..." UTF-16 for FatFs).
 */
#ifndef TCL_FS_H_
#define TCL_FS_H_

#include "tclwext4.h"

typedef enum {
    TCL_FS_NONE = 0,
    TCL_FS_EXT,          /* ext2/3/4 via lwext4      */
    TCL_FS_FAT,          /* FAT12/16/32 via FatFs    */
    TCL_FS_SQFS          /* SquashFS via squashfuse, always read-only */
} tcl_fs_kind;

typedef struct {
    wchar_t  name[MAX_PATH];
    bool     is_dir;
    bool     is_link;      /* ext only; FAT has no symlinks */
    bool     read_only;
    uint64_t size;
    uint32_t mtime, atime, ctime;   /* unix seconds, 0 if unknown */
} tcl_dirent;

/* Opaque-ish handles. The unions keep callers from needing either library's
   headers; only the backends look inside. */
typedef struct tcl_file tcl_file;
typedef struct tcl_dirh tcl_dirh;

tcl_file *tcl_fs_fopen(tcl_volume *v, const wchar_t *path, bool for_write);
int       tcl_fs_fread(tcl_file *f, void *buf, size_t len, size_t *got);
int       tcl_fs_fwrite(tcl_file *f, const void *buf, size_t len, size_t *put);
uint64_t  tcl_fs_fsize(tcl_file *f);
void      tcl_fs_fclose(tcl_file *f);

tcl_dirh *tcl_fs_opendir(tcl_volume *v, const wchar_t *path);
bool      tcl_fs_readdir(tcl_dirh *d, tcl_dirent *out);
void      tcl_fs_closedir(tcl_dirh *d);

int  tcl_fs_stat(tcl_volume *v, const wchar_t *path, tcl_dirent *out);
int  tcl_fs_mkdir(tcl_volume *v, const wchar_t *path);
int  tcl_fs_unlink(tcl_volume *v, const wchar_t *path);
int  tcl_fs_rmdir(tcl_volume *v, const wchar_t *path);
int  tcl_fs_rename(tcl_volume *v, const wchar_t *from, const wchar_t *to);
int  tcl_fs_set_times(tcl_volume *v, const wchar_t *path,
                      const FILETIME *atime, const FILETIME *mtime);
int  tcl_fs_set_readonly(tcl_volume *v, const wchar_t *path, bool ro);
void tcl_fs_flush(tcl_volume *v);
int  tcl_fs_statfs(tcl_volume *v, uint64_t *total, uint64_t *freebytes);

/* Mount / unmount dispatch. */
int  tcl_fs_mount(tcl_volume *v);
void tcl_fs_unmount(tcl_volume *v);

/*
 * Resolve a TC path to a volume, mounting it on demand, and hand back the
 * volume-relative remainder ("" for the volume root itself).
 */
tcl_volume *tcl_fs_resolve(const wchar_t *tc_path, const wchar_t **rel);

/* ---- backend entry points (not called directly by the WFX layer) ---- */

int  tcl_ext_mount(tcl_volume *v);
void tcl_ext_unmount(tcl_volume *v);
int  tcl_fat_mount(tcl_volume *v);
void tcl_fat_unmount(tcl_volume *v);
int  tcl_sqfs_mount(tcl_volume *v);
void tcl_sqfs_unmount(tcl_volume *v);

#endif /* TCL_FS_H_ */
