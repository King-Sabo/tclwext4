/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_vol.c - volume table, lazy mounting and TC-path -> lwext4-path mapping.
 *
 * Read-only is the safe default and is applied whenever ANY of these hold:
 *   - the superblock carries ro_compat bits lwext4 does not implement
 *   - the journal needs recovery, MMP is set, or the fs is not clean
 *   - the backing store could only be opened for reading
 *   - the process is not elevated and the backing store is a physical drive
 *   - the user set readonly=1 in the ini
 */
#include "tclwext4.h"
#include "tcl_fs.h"
#include <stdio.h>

tcl_volume       g_vol[TCL_MAX_VOLUMES];
int              g_vol_count = 0;
CRITICAL_SECTION g_ext4_cs;
bool             g_global_ro = false;

wchar_t g_images[TCL_MAX_IMAGES][MAX_PATH];
int     g_image_count = 0;

static void ext4_lock_cb(void)   { EnterCriticalSection(&g_ext4_cs); }
static void ext4_unlock_cb(void) { LeaveCriticalSection(&g_ext4_cs); }
static const struct ext4_lock g_ext4_locks = { ext4_lock_cb, ext4_unlock_cb };

void tcl_vol_rescan(void)
{
    tcl_part parts[TCL_MAX_VOLUMES];
    int n, i;

    tcl_vol_unmount_all();

    n = tcl_scan_all(parts, TCL_MAX_VOLUMES,
                     (const wchar_t (*)[MAX_PATH])g_images, g_image_count);

    ZeroMemory(g_vol, sizeof(g_vol));
    for (i = 0; i < n; i++) {
        g_vol[i].in_use   = true;
        g_vol[i].part     = parts[i];
        g_vol[i].fs       = (parts[i].fskind == TCL_FSK_FAT)  ? TCL_FS_FAT :
                            (parts[i].fskind == TCL_FSK_SQFS) ? TCL_FS_SQFS : TCL_FS_EXT;
        g_vol[i].fat_pdrv  = -1;
        g_vol[i].sqfs_slot = -1;
        _snprintf_s(g_vol[i].dev_name, sizeof(g_vol[i].dev_name), _TRUNCATE,
                    "bd%d", i);
        {
            char *lbl = tcl_w_to_u8(parts[i].label);
            _snprintf_s(g_vol[i].mp, sizeof(g_vol[i].mp), _TRUNCATE,
                        "/v%d_%.40s/", i, lbl ? lbl : "vol");
            if (lbl) LocalFree(lbl);
        }
    }
    g_vol_count = n;
    tcl_logf(L"tclwext4: %d volume(s) found", n);
}

int tcl_ext_mount(tcl_volume *v)
{
    bool want_write;
    int  r;

    if (v->mounted)
        return EOK;
    if (!v->part.mountable) {
        tcl_logf(L"tclwext4: %s not mountable, unsupported INCOMPAT 0x%08X",
                 v->part.label, v->part.incompat_unsup);
        return ENOTSUP;
    }

    want_write = !g_global_ro && !v->part.force_ro;
    if (want_write && v->part.kind == TCL_SRC_DISK && !tcl_is_elevated())
        want_write = false;

    r = tcl_bdev_open(&v->bdev, v->part.backing, v->part.offset,
                      v->part.size, v->part.sector, want_write);
    if (r != EOK)
        return r;

    /* tcl_bdev_open() downgrades on its own if the write open failed. */
    v->read_only = !v->bdev.writable;

    r = ext4_device_register(&v->bdev.bd, v->dev_name);
    if (r != EOK) {
        tcl_bdev_close(&v->bdev);
        return r;
    }

    r = ext4_mount(v->dev_name, v->mp, v->read_only);
    if (r != EOK) {
        ext4_device_unregister(v->dev_name);
        tcl_bdev_close(&v->bdev);
        tcl_logf(L"tclwext4: mount of %s failed (%d)", v->part.label, r);
        return r;
    }

    ext4_mount_setup_locks(v->mp, &g_ext4_locks);

    if (!v->read_only) {
        if (ext4_recover(v->mp) != EOK)
            tcl_logf(L"tclwext4: %s journal recovery reported an error", v->part.label);
        if (ext4_journal_start(v->mp) == EOK)
            v->journal_on = true;
    }

    v->mounted = true;
    tcl_logf(L"tclwext4: mounted %s %s%s%s", v->part.label,
             v->read_only ? L"read-only" : L"read-write",
             v->part.ro_reason[0] ? L" - " : L"",
             v->part.ro_reason[0] ? v->part.ro_reason : L"");
    return EOK;
}

void tcl_ext_unmount(tcl_volume *v)
{
    if (!v->mounted)
        return;

    if (v->wb_on) {
        ext4_cache_write_back(v->mp, 0);
        v->wb_on = false;
    }
    ext4_cache_flush(v->mp);
    if (v->journal_on) {
        ext4_journal_stop(v->mp);
        v->journal_on = false;
    }
    ext4_umount(v->mp);
    ext4_device_unregister(v->dev_name);
    tcl_bdev_close(&v->bdev);
    v->mounted = false;
}

int tcl_vol_mount(tcl_volume *v)
{
    return tcl_fs_mount(v);
}

void tcl_vol_unmount(tcl_volume *v)
{
    tcl_fs_unmount(v);
}

void tcl_vol_unmount_all(void)
{
    int i;
    for (i = 0; i < g_vol_count; i++)
        if (g_vol[i].in_use)
            tcl_vol_unmount(&g_vol[i]);
}

tcl_volume *tcl_vol_find(const wchar_t *name)
{
    int i;
    for (i = 0; i < g_vol_count; i++)
        if (g_vol[i].in_use && _wcsicmp(g_vol[i].part.label, name) == 0)
            return &g_vol[i];
    return NULL;
}

/*
 * Component-wise symlink resolution.
 *
 * Resolving only the final component would be cheaper but wrong: descending
 * into a directory that is itself reached through a symlinked parent (the
 * usual /usr/lib -> /usr/lib64 shape) needs every component resolved.
 *
 * ".." inside a target needs no normalisation - ext4 directories carry real
 * ".." entries, so lwext4's own lookup walks it correctly.
 */
bool tcl_realpath(tcl_volume *v, const char *in, char *out, size_t outsz)
{
    char cur[768], cand[768], link[512];
    const char *p;
    size_t mplen = strlen(v->mp);

    if (strncmp(in, v->mp, mplen) != 0) {
        strcpy_s(out, outsz, in);
        return false;
    }

    strcpy_s(cur, sizeof(cur), v->mp);      /* always ends in '/' */
    p = in + mplen;

    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        int depth;

        if (!len) {                          /* skip empty / doubled '/' */
            p = slash ? slash + 1 : p + len;
            continue;
        }

        strcpy_s(cand, sizeof(cand), cur);
        strncat_s(cand, sizeof(cand), p, len);

        for (depth = 0; depth < TCL_SYMLINK_MAX; depth++) {
            size_t rcnt = 0;
            char *tail;

            if (ext4_inode_exist(cand, EXT4_DE_SYMLINK) != EOK)
                break;
            if (ext4_readlink(cand, link, sizeof(link) - 1, &rcnt) != EOK) {
                strcpy_s(out, outsz, in);
                return false;
            }
            link[rcnt] = 0;

            if (link[0] == '/') {
                /* Absolute inside the volume: re-root at the mount point. */
                strcpy_s(cand, sizeof(cand), v->mp);
                strcat_s(cand, sizeof(cand), link + 1);
            } else {
                /* Relative: replace the last component of cand. */
                tail = strrchr(cand, '/');
                if (!tail) {
                    strcpy_s(out, outsz, in);
                    return false;
                }
                tail[1] = 0;
                strcat_s(cand, sizeof(cand), link);
            }
        }
        if (depth >= TCL_SYMLINK_MAX) {      /* loop or absurd nesting */
            strcpy_s(out, outsz, in);
            return false;
        }

        strcpy_s(cur, sizeof(cur), cand);
        if (slash) {
            strcat_s(cur, sizeof(cur), "/");
            p = slash + 1;
        } else {
            p += len;
        }
    }

    strcpy_s(out, outsz, cur);
    return true;
}

/* tcl_vol_resolve() was replaced by tcl_fs_resolve() in tcl_fs.c, which
   returns the volume-relative tail and lets each backend build its own path. */
