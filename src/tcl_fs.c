/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_fs.c - dispatch layer over the ext4 and FAT backends.
 *
 * The two libraries differ in ways worth naming, because they drive most of
 * the mapping code below:
 *
 *   - lwext4 wants UTF-8 paths under a per-mount prefix ("/v0_Disk0p2/...");
 *     FatFs with FF_LFN_UNICODE=1 wants UTF-16 under a drive number ("0:/..."),
 *     which is already what Total Commander hands us.
 *   - ext4 stores UTC seconds; FAT stores local wall-clock with 2-second
 *     granularity and no timezone, so every conversion goes through the
 *     local-time API rather than a fixed offset.
 *   - ext4 has symlinks and a permission mode; FAT has neither, just a
 *     read-only attribute bit.
 */
#include "tcl_fs.h"
#include "ff.h"
#include "ext4_inode.h"
#include "ext4_super.h"
#include <stdio.h>
#include <wchar.h>

struct tcl_file {
    tcl_volume *v;
    bool        fat;
    ext4_file   e;
    FIL         f;
};

struct tcl_dirh {
    tcl_volume *v;
    bool        fat;
    ext4_dir    ed;
    char        ebase[768];
    DIR         fd;
};

/* from tcl_fs_fat.c */
void tcl_fat_path(tcl_volume *v, const wchar_t *rel, wchar_t *out, size_t n);
void tcl_fat_time_to_unix(WORD fdate, WORD ftime, uint32_t *unix_out);
void tcl_fat_time_from_unix(uint32_t secs, WORD *fdate, WORD *ftime);

/* ------------------------------------------------------ path helpers */

/* "\Vol\a\b" -> volume, with *rel pointing at "a\b" (or L"" for the root). */
tcl_volume *tcl_fs_resolve(const wchar_t *tc_path, const wchar_t **rel)
{
    static const wchar_t empty[] = L"";
    const wchar_t *p = tc_path, *slash;
    wchar_t name[64];
    tcl_volume *v;

    if (!p)
        return NULL;
    if (*p == L'\\')
        p++;
    slash = wcschr(p, L'\\');

    if (slash) {
        size_t len = (size_t)(slash - p);
        if (len >= _countof(name))
            return NULL;
        wmemcpy(name, p, len);
        name[len] = 0;
        *rel = slash + 1;
    } else {
        wcsncpy_s(name, _countof(name), p, _TRUNCATE);
        *rel = empty;
    }
    if (!name[0])
        return NULL;

    v = tcl_vol_find(name);
    if (!v)
        return NULL;
    if (tcl_fs_mount(v) != EOK)
        return NULL;
    return v;
}

/* Build the lwext4 path for a volume-relative tail, following symlinks. */
static bool ext_path(tcl_volume *v, const wchar_t *rel, char *out, size_t n)
{
    char resolved[768];
    char *u8;
    size_t i;

    strcpy_s(out, n, v->mp);          /* always ends in '/' */
    if (rel && *rel) {
        u8 = tcl_w_to_u8(rel);
        if (!u8)
            return false;
        for (i = 0; u8[i]; i++)
            if (u8[i] == '\\')
                u8[i] = '/';
        strcat_s(out, n, u8);
        LocalFree(u8);
    }
    if (tcl_realpath(v, out, resolved, sizeof(resolved)))
        strcpy_s(out, n, resolved);
    return true;
}

/* ------------------------------------------------------ mount/unmount */

int tcl_fs_mount(tcl_volume *v)
{
    if (v->mounted)
        return EOK;
    if (!v->part.mountable)
        return ENOTSUP;
    return (v->fs == TCL_FS_FAT) ? tcl_fat_mount(v) : tcl_ext_mount(v);
}

void tcl_fs_unmount(tcl_volume *v)
{
    if (!v->mounted)
        return;
    if (v->fs == TCL_FS_FAT)
        tcl_fat_unmount(v);
    else
        tcl_ext_unmount(v);
}

/* ------------------------------------------------------------ files */

tcl_file *tcl_fs_fopen(tcl_volume *v, const wchar_t *path, bool for_write)
{
    const wchar_t *rel;
    tcl_volume *vv = tcl_fs_resolve(path, &rel);
    tcl_file *f;

    if (!vv || vv != v)
        return NULL;

    f = (tcl_file *)LocalAlloc(LPTR, sizeof(*f));
    if (!f)
        return NULL;
    f->v = v;
    f->fat = (v->fs == TCL_FS_FAT);

    if (f->fat) {
        wchar_t p[MAX_PATH];
        tcl_fat_path(v, rel, p, _countof(p));
        if (f_open(&f->f, p, for_write ? (FA_WRITE | FA_CREATE_ALWAYS)
                                       : (FA_READ | FA_OPEN_EXISTING)) != FR_OK) {
            LocalFree(f);
            return NULL;
        }
    } else {
        char p[768];
        if (!ext_path(v, rel, p, sizeof(p)) ||
            ext4_fopen(&f->e, p, for_write ? "wb" : "rb") != EOK) {
            LocalFree(f);
            return NULL;
        }
    }
    return f;
}

int tcl_fs_fread(tcl_file *f, void *buf, size_t len, size_t *got)
{
    if (f->fat) {
        UINT br = 0;
        if (f_read(&f->f, buf, (UINT)len, &br) != FR_OK)
            return EIO;
        *got = br;
        return EOK;
    }
    return ext4_fread(&f->e, buf, len, got);
}

int tcl_fs_fwrite(tcl_file *f, const void *buf, size_t len, size_t *put)
{
    if (f->fat) {
        UINT bw = 0;
        if (f_write(&f->f, buf, (UINT)len, &bw) != FR_OK)
            return EIO;
        *put = bw;
        return (bw == len) ? EOK : EIO;
    }
    return ext4_fwrite(&f->e, buf, len, put);
}

uint64_t tcl_fs_fsize(tcl_file *f)
{
    return f->fat ? (uint64_t)f_size(&f->f) : ext4_fsize(&f->e);
}

void tcl_fs_fclose(tcl_file *f)
{
    if (!f)
        return;
    if (f->fat)
        f_close(&f->f);
    else
        ext4_fclose(&f->e);
    LocalFree(f);
}

/* ------------------------------------------------------ directories */

tcl_dirh *tcl_fs_opendir(tcl_volume *v, const wchar_t *path)
{
    const wchar_t *rel;
    tcl_volume *vv = tcl_fs_resolve(path, &rel);
    tcl_dirh *d;

    if (!vv || vv != v)
        return NULL;

    d = (tcl_dirh *)LocalAlloc(LPTR, sizeof(*d));
    if (!d)
        return NULL;
    d->v = v;
    d->fat = (v->fs == TCL_FS_FAT);

    if (d->fat) {
        wchar_t p[MAX_PATH];
        tcl_fat_path(v, rel, p, _countof(p));
        if (f_opendir(&d->fd, p) != FR_OK) {
            LocalFree(d);
            return NULL;
        }
    } else {
        if (!ext_path(v, rel, d->ebase, sizeof(d->ebase))) {
            LocalFree(d);
            return NULL;
        }
        if (d->ebase[strlen(d->ebase) - 1] != '/')
            strcat_s(d->ebase, sizeof(d->ebase), "/");
        if (ext4_dir_open(&d->ed, d->ebase) != EOK) {
            LocalFree(d);
            return NULL;
        }
    }
    return d;
}

bool tcl_fs_readdir(tcl_dirh *d, tcl_dirent *out)
{
    ZeroMemory(out, sizeof(*out));

    if (d->fat) {
        FILINFO fi;
        if (f_readdir(&d->fd, &fi) != FR_OK || fi.fname[0] == 0)
            return false;
        wcsncpy_s(out->name, MAX_PATH, fi.fname, _TRUNCATE);
        out->is_dir    = (fi.fattrib & AM_DIR) != 0;
        out->read_only = d->v->read_only || (fi.fattrib & AM_RDO) != 0;
        out->size      = (uint64_t)fi.fsize;
        tcl_fat_time_to_unix(fi.fdate, fi.ftime, &out->mtime);
        out->atime = out->ctime = out->mtime;   /* FAT keeps only one usable stamp */
        return true;
    }

    for (;;) {
        const ext4_direntry *de = ext4_dir_entry_next(&d->ed);
        char nm[256], full[768], resolved[768];
        const char *target;
        struct ext4_inode ino;
        struct ext4_sblock *sb = NULL;
        uint32_t inum = 0;
        wchar_t *w;

        if (!de)
            return false;
        if (de->name_length == 1 && de->name[0] == '.')
            continue;
        if (de->name_length == 2 && de->name[0] == '.' && de->name[1] == '.')
            continue;

        memcpy(nm, de->name, de->name_length);
        nm[de->name_length] = 0;
        strcpy_s(full, sizeof(full), d->ebase);
        strcat_s(full, sizeof(full), nm);

        w = tcl_u8_to_w(nm);
        if (!w)
            continue;
        wcsncpy_s(out->name, MAX_PATH, w, _TRUNCATE);
        LocalFree(w);

        out->is_dir  = (de->inode_type == EXT4_DE_DIR);
        out->is_link = (de->inode_type == EXT4_DE_SYMLINK);
        target = full;
        if (out->is_link && tcl_realpath(d->v, full, resolved, sizeof(resolved))) {
            target = resolved;
            out->is_dir = (ext4_inode_exist(resolved, EXT4_DE_DIR) == EOK);
        }
        if (ext4_raw_inode_fill(target, &inum, &ino) == EOK &&
            ext4_get_sblock(d->v->mp, &sb) == EOK) {
            out->size  = ext4_inode_get_size(sb, &ino);
            out->mtime = ext4_inode_get_modif_time(&ino);
            out->atime = ext4_inode_get_access_time(&ino);
            out->ctime = ext4_inode_get_change_inode_time(&ino);
        }
        out->read_only = d->v->read_only;
        return true;
    }
}

void tcl_fs_closedir(tcl_dirh *d)
{
    if (!d)
        return;
    if (d->fat)
        f_closedir(&d->fd);
    else
        ext4_dir_close(&d->ed);
    LocalFree(d);
}

/* ------------------------------------------------------- metadata */

int tcl_fs_stat(tcl_volume *v, const wchar_t *path, tcl_dirent *out)
{
    const wchar_t *rel;
    tcl_volume *vv = tcl_fs_resolve(path, &rel);

    if (!vv || vv != v)
        return ENOENT;
    ZeroMemory(out, sizeof(*out));

    if (v->fs == TCL_FS_FAT) {
        wchar_t p[MAX_PATH];
        FILINFO fi;
        tcl_fat_path(v, rel, p, _countof(p));
        if (f_stat(p, &fi) != FR_OK)
            return ENOENT;
        wcsncpy_s(out->name, MAX_PATH, fi.fname, _TRUNCATE);
        out->is_dir    = (fi.fattrib & AM_DIR) != 0;
        out->read_only = (fi.fattrib & AM_RDO) != 0;
        out->size      = (uint64_t)fi.fsize;
        tcl_fat_time_to_unix(fi.fdate, fi.ftime, &out->mtime);
        return EOK;
    } else {
        char p[768];
        struct ext4_inode ino;
        struct ext4_sblock *sb = NULL;
        uint32_t inum = 0;
        if (!ext_path(v, rel, p, sizeof(p)))
            return ENOENT;
        if (ext4_raw_inode_fill(p, &inum, &ino) != EOK ||
            ext4_get_sblock(v->mp, &sb) != EOK)
            return ENOENT;
        out->is_dir = (ext4_inode_exist(p, EXT4_DE_DIR) == EOK);
        out->size   = ext4_inode_get_size(sb, &ino);
        out->mtime  = ext4_inode_get_modif_time(&ino);
        out->atime  = ext4_inode_get_access_time(&ino);
        out->ctime  = ext4_inode_get_change_inode_time(&ino);
        return EOK;
    }
}

#define FAT_PATH(v, rel, buf) tcl_fat_path((v), (rel), (buf), _countof(buf))

int tcl_fs_mkdir(tcl_volume *v, const wchar_t *path)
{
    const wchar_t *rel;
    if (!tcl_fs_resolve(path, &rel))
        return ENOENT;
    if (v->fs == TCL_FS_FAT) {
        wchar_t p[MAX_PATH];
        FAT_PATH(v, rel, p);
        return f_mkdir(p) == FR_OK ? EOK : EIO;
    } else {
        char p[768];
        if (!ext_path(v, rel, p, sizeof(p)))
            return ENOENT;
        return ext4_dir_mk(p);
    }
}

int tcl_fs_unlink(tcl_volume *v, const wchar_t *path)
{
    const wchar_t *rel;
    if (!tcl_fs_resolve(path, &rel))
        return ENOENT;
    if (v->fs == TCL_FS_FAT) {
        wchar_t p[MAX_PATH];
        FAT_PATH(v, rel, p);
        return f_unlink(p) == FR_OK ? EOK : EIO;
    } else {
        char p[768];
        if (!ext_path(v, rel, p, sizeof(p)))
            return ENOENT;
        return ext4_fremove(p);
    }
}

int tcl_fs_rmdir(tcl_volume *v, const wchar_t *path)
{
    const wchar_t *rel;
    if (!tcl_fs_resolve(path, &rel))
        return ENOENT;
    if (v->fs == TCL_FS_FAT) {
        wchar_t p[MAX_PATH];
        FAT_PATH(v, rel, p);
        /* FatFs deletes empty directories with f_unlink too. */
        return f_unlink(p) == FR_OK ? EOK : EIO;
    } else {
        char p[768];
        if (!ext_path(v, rel, p, sizeof(p)))
            return ENOENT;
        return ext4_dir_rm(p);
    }
}

int tcl_fs_rename(tcl_volume *v, const wchar_t *from, const wchar_t *to)
{
    const wchar_t *rf, *rt;
    if (!tcl_fs_resolve(from, &rf) || !tcl_fs_resolve(to, &rt))
        return ENOENT;
    if (v->fs == TCL_FS_FAT) {
        wchar_t a[MAX_PATH], b[MAX_PATH];
        FAT_PATH(v, rf, a);
        FAT_PATH(v, rt, b);
        return f_rename(a, b) == FR_OK ? EOK : EIO;
    } else {
        char a[768], b[768];
        if (!ext_path(v, rf, a, sizeof(a)) || !ext_path(v, rt, b, sizeof(b)))
            return ENOENT;
        return ext4_frename(a, b);
    }
}

int tcl_fs_set_times(tcl_volume *v, const wchar_t *path,
                     const FILETIME *atime, const FILETIME *mtime)
{
    const wchar_t *rel;
    if (!tcl_fs_resolve(path, &rel))
        return ENOENT;

    if (v->fs == TCL_FS_FAT) {
        wchar_t p[MAX_PATH];
        FILINFO fi;
        /* FAT has one write timestamp; access time is a date-only optional
           field FatFs does not expose, so atime is dropped rather than faked. */
        if (!mtime)
            return EOK;
        ZeroMemory(&fi, sizeof(fi));
        tcl_fat_time_from_unix(tcl_unix_from_filetime(mtime), &fi.fdate, &fi.ftime);
        FAT_PATH(v, rel, p);
        return f_utime(p, &fi) == FR_OK ? EOK : EIO;
    } else {
        char p[768];
        int rc = EOK;
        if (!ext_path(v, rel, p, sizeof(p)))
            return ENOENT;
        if (atime && ext4_atime_set(p, tcl_unix_from_filetime(atime)) != EOK)
            rc = EIO;
        if (mtime && ext4_mtime_set(p, tcl_unix_from_filetime(mtime)) != EOK)
            rc = EIO;
        return rc;
    }
}

int tcl_fs_set_readonly(tcl_volume *v, const wchar_t *path, bool ro)
{
    const wchar_t *rel;
    if (!tcl_fs_resolve(path, &rel))
        return ENOENT;

    if (v->fs == TCL_FS_FAT) {
        wchar_t p[MAX_PATH];
        FAT_PATH(v, rel, p);
        return f_chmod(p, ro ? AM_RDO : 0, AM_RDO) == FR_OK ? EOK : EIO;
    } else {
        char p[768];
        uint32_t mode = 0;
        if (!ext_path(v, rel, p, sizeof(p)))
            return ENOENT;
        if (ext4_mode_get(p, &mode) != EOK)
            return EIO;
        if (ro) mode &= ~0222u; else mode |= 0200u;
        return ext4_mode_set(p, mode);
    }
}

void tcl_fs_flush(tcl_volume *v)
{
    if (!v->mounted)
        return;
    if (v->fs == TCL_FS_FAT)
        FlushFileBuffers(v->bdev.h);
    else
        ext4_cache_flush(v->mp);
}

int tcl_fs_statfs(tcl_volume *v, uint64_t *total, uint64_t *freebytes)
{
    if (!v->mounted)
        return EIO;

    if (v->fs == TCL_FS_FAT) {
        wchar_t drv[8];
        DWORD nclst = 0;
        FATFS *fs = NULL;
        swprintf_s(drv, _countof(drv), L"%d:", v->fat_pdrv);
        if (f_getfree(drv, &nclst, &fs) != FR_OK)
            return EIO;
        *freebytes = (uint64_t)nclst * fs->csize * v->part.sector;
        *total     = v->part.size;
        return EOK;
    } else {
        struct ext4_mount_stats st;
        if (ext4_mount_point_stats(v->mp, &st) != EOK)
            return EIO;
        *freebytes = (uint64_t)st.free_blocks_count * st.block_size;
        *total     = (uint64_t)st.blocks_count * st.block_size;
        return EOK;
    }
}
