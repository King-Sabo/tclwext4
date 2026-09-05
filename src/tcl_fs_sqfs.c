/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_fs_sqfs.c - SquashFS backend (squashfuse).
 *
 * Read-only by construction: SquashFS has no write path at all, so every
 * mutating operation returns EROFS and the volume is always mounted read-only.
 * None of the corruption hazards that dominate the ext and FAT backends apply
 * here - there is no journal, no cache write-back, no partial-update window.
 *
 * squashfuse ships first-class Windows support: sqfs_fd_t is a HANDLE, and
 * sqfs_init() takes a byte offset into it. That is exactly what tcl_bdev holds
 * for a partition, so the existing block layer maps on with no adapter.
 *
 * Unlike FAT, this is allowed on physical disks as well as images. Windows has
 * no SquashFS driver, so there is no second writer to race with - and since we
 * never write, there is nothing to race about in any case.
 */
#include "tcl_fs.h"
#include "squashfuse.h"
#include <stdio.h>

/*
 * config/squashfuse/swap.*.inc are pre-generated from squashfs_fs.h (see the
 * banner in those files). They cannot go stale by accident, since the submodule
 * is pinned by gitlink - but if someone bumps squashfuse and the on-disk
 * structures have moved, a stale swap table would mis-decode rather than fail.
 *
 * These assertions catch that at compile time, with no external tooling, which
 * is the point: the build must work with nothing but Visual Studio.
 */
_STATIC_ASSERT(sizeof(struct squashfs_super_block)   == 96);
_STATIC_ASSERT(sizeof(struct squashfs_base_inode)    == 16);
_STATIC_ASSERT(sizeof(struct squashfs_symlink_inode) == 24);
_STATIC_ASSERT(sizeof(struct squashfs_dir_inode)     == 32);
_STATIC_ASSERT(sizeof(struct squashfs_reg_inode)     == 32);

#define SQFS_NORM_MAX 128   /* path components; deeper paths are rejected */

/* One open filesystem per volume. */
typedef struct {
    sqfs  fs;
    bool  in_use;
} sqfs_slot;

static sqfs_slot g_sqfs[TCL_MAX_VOLUMES];

sqfs *tcl_sqfs_of(tcl_volume *v)
{
    if (v->sqfs_slot < 0 || v->sqfs_slot >= TCL_MAX_VOLUMES)
        return NULL;
    if (!g_sqfs[v->sqfs_slot].in_use)
        return NULL;
    return &g_sqfs[v->sqfs_slot].fs;
}

/* ------------------------------------------------- mount / unmount */

int tcl_sqfs_mount(tcl_volume *v)
{
    int slot;
    sqfs_err err;

    for (slot = 0; slot < TCL_MAX_VOLUMES; slot++)
        if (!g_sqfs[slot].in_use)
            break;
    if (slot == TCL_MAX_VOLUMES)
        return ENOSPC;

    /* Always read-only, so never ask for write access on the backing store.
       This also means a SquashFS partition on a physical disk needs no
       elevation. */
    if (tcl_bdev_open(&v->bdev, v->part.backing, v->part.offset,
                      v->part.size, v->part.sector, false) != EOK)
        return EIO;

    v->read_only = true;

    err = sqfs_init(&g_sqfs[slot].fs, v->bdev.h, (size_t)v->part.offset);
    if (err != SQFS_OK) {
        tcl_bdev_close(&v->bdev);
        tcl_logf(L"tclwext4: SquashFS mount of %s failed (%s)", v->part.label,
                 err == SQFS_BADFORMAT  ? L"bad format" :
                 err == SQFS_BADVERSION ? L"unsupported squashfs version" :
                 err == SQFS_BADCOMP    ? L"unsupported compression" :
                 err == SQFS_UNSUP      ? L"unsupported feature" : L"error");
        /*
         * The scan already vetted the compression id, so BADCOMP here means the
         * codec we advertised is not actually registered in squashfuse's table.
         * For xz that points at one specific thing: the shadowed
         * win_decompress.c.inc is no longer being picked up - see the comment at
         * the top of config/squashfuse/win_decompress.c.inc.
         */
        if (err == SQFS_BADCOMP)
            tcl_logf(L"tclwext4: %s was detected as a supported codec but "
                     L"squashfuse has no decompressor for it - check the include "
                     L"order for config/squashfuse (see win_decompress.c.inc)",
                     v->part.label);
        return (err == SQFS_BADCOMP) ? ENOTSUP : EIO;
    }

    g_sqfs[slot].in_use = true;
    v->sqfs_slot = slot;
    v->mounted   = true;

    tcl_logf(L"tclwext4: mounted %s as SquashFS (read-only)", v->part.label);
    return EOK;
}

void tcl_sqfs_unmount(tcl_volume *v)
{
    if (!v->mounted)
        return;
    if (v->sqfs_slot >= 0 && v->sqfs_slot < TCL_MAX_VOLUMES &&
        g_sqfs[v->sqfs_slot].in_use) {
        sqfs_destroy(&g_sqfs[v->sqfs_slot].fs);
        g_sqfs[v->sqfs_slot].in_use = false;
    }
    v->sqfs_slot = -1;
    tcl_bdev_close(&v->bdev);
    v->mounted = false;
}

/* ---------------------------------------------------------- lookup */

/*
 * Lexically normalise a volume-relative path in place: collapse "//", drop
 * ".", and pop the previous component on "..".
 *
 * This has to be lexical rather than a per-component lookup, because SquashFS
 * directories contain no "." or ".." entries at all - unlike ext4, where
 * lwext4 can simply walk them. A relative symlink target such as
 * "../lib/libfoo.so" is therefore unresolvable by lookup and must be folded
 * into the path first.
 */
static void sqfs_norm(char *p)
{
    const char *seg[SQFS_NORM_MAX];
    size_t      len[SQFS_NORM_MAX];
    int         n = 0, i;
    const char *in = p;
    char       *out;

    /*
     * Components are collected from the original string first and only written
     * back afterwards.
     *
     * The obvious in-place version - copy each component forward, then append
     * '/' - is wrong in a way that hides well: when nothing collapses, the read
     * and write positions are the same pointer, so writing the separator
     * overwrites the NUL the reader is about to reach. The loop then runs on
     * into whatever follows the buffer. It only looks correct when the bytes
     * past the string happen to be zero, which is exactly what a unit test on a
     * fresh stack buffer gives you.
     */
    while (*in && n < SQFS_NORM_MAX) {
        const char *s;
        size_t l;

        while (*in == '/')
            in++;
        if (!*in)
            break;
        s = in;
        while (*in && *in != '/')
            in++;
        l = (size_t)(in - s);

        if (l == 1 && s[0] == '.')
            continue;
        if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (n)                      /* ".." above the root clamps to it */
                n--;
            continue;
        }
        seg[n] = s;
        len[n] = l;
        n++;
    }

    out = p;
    for (i = 0; i < n; i++) {
        if (i)
            *out++ = '/';
        memmove(out, seg[i], len[i]);
        out += len[i];
    }
    *out = 0;
}

/*
 * Resolve a volume-relative UTF-8 path to an inode, following symlinks at every
 * component - not just the last one. Resolving only the final component looks
 * like it works until a path descends through a symlinked parent, which is the
 * common case in a root filesystem.
 *
 * Depth-capped exactly as tcl_realpath() is for ext, so loops terminate.
 */
bool tcl_sqfs_resolve_u8(tcl_volume *v, const char *rel, sqfs_inode *out)
{
    sqfs *fs = tcl_sqfs_of(v);
    char cur[1024] = { 0 };
    char seg_dbg[256] = { 0 };
    const char *in = rel;
    int total_links = 0;

    if (!fs)
        return false;
    if (sqfs_inode_get(fs, out, sqfs_inode_root(fs)) != SQFS_OK)
        return false;
    if (!rel || !*rel)
        return true;

    while (*in) {
        const char *seg;
        size_t len, curlen;
        int depth;

        while (*in == '/')
            in++;
        if (!*in)
            break;
        seg = in;
        while (*in && *in != '/')
            in++;
        len = (size_t)(in - seg);

        {
            size_t dl = len < sizeof(seg_dbg) - 1 ? len : sizeof(seg_dbg) - 1;
            memcpy(seg_dbg, seg, dl);
            seg_dbg[dl] = 0;
        }

        curlen = strlen(cur);
        if (curlen + len + 2 >= sizeof(cur))
            return false;
        if (curlen)
            cur[curlen++] = '/';
        memcpy(cur + curlen, seg, len);
        cur[curlen + len] = 0;
        sqfs_norm(cur);

        /* Resolve this prefix, following any symlink it lands on. */
        for (depth = 0; ; depth++) {
            bool found = false;
            char target[1024];
            size_t tn;

            if (sqfs_inode_get(fs, out, sqfs_inode_root(fs)) != SQFS_OK)
                return false;
            if (!cur[0])
                break;                      /* normalised back to the root */
            if (sqfs_lookup_path(fs, out, cur, &found) != SQFS_OK || !found) {
                tcl_dbgf(L"tclwext4: sqfs: lookup failed for '%S' (depth %d)", cur, depth);
                return false;
            }
            if (!S_ISLNK(sqfs_mode(out->base.inode_type)))
                break;
            if (depth >= TCL_SYMLINK_MAX || ++total_links > TCL_SYMLINK_MAX * 4)
                return false;               /* loop, or a pathological chain */

            /*
             * Use squashfuse's documented two-call protocol: a NULL buffer
             * reports the length (symlink_size + 1), then the real read.
             *
             * The one-shot form - passing a big buffer and trusting the
             * library's terminator - relies on the inode's symlink_size being
             * sane. When it is not, sqfs_readlink() still reports success and
             * writes an unterminated buffer, and the garbage propagates into
             * the path. Ask for the length first, sanity-check it, and place
             * the terminator ourselves.
             */
            tn = 0;
            if (sqfs_readlink(fs, out, NULL, &tn) != SQFS_OK) {
                tcl_dbgf(L"tclwext4: sqfs: readlink size query failed for '%S'", cur);
                return false;
            }
            if (tn == 0 || tn > sizeof(target)) {
                tcl_dbgf(L"tclwext4: sqfs: implausible link length %zu for '%S' "
                         L"(inode symlink_size looks wrong)", tn, cur);
                return false;
            }
            if (sqfs_readlink(fs, out, target, &tn) != SQFS_OK) {
                tcl_dbgf(L"tclwext4: sqfs: readlink failed for '%S'", cur);
                return false;
            }
            target[tn - 1] = 0;     /* never trust the library's terminator */

            if (target[0] == '/') {
                /* Absolute inside the image: re-root, never a host path. */
                strcpy_s(cur, sizeof(cur), target + 1);
            } else {
                char *slash = strrchr(cur, '/');
                if (slash)
                    slash[1] = 0;
                else
                    cur[0] = 0;
                if (strlen(cur) + strlen(target) + 1 >= sizeof(cur))
                    return false;
                strcat_s(cur, sizeof(cur), target);
            }
            sqfs_norm(cur);
            tcl_dbgf(L"tclwext4: sqfs: link '%S' (len %zu) -> '%S'",
                     seg_dbg, tn - 1, cur);
        }
    }
    return true;
}

/* Wide-path wrapper: TC gives UTF-16 with backslashes. */
bool tcl_sqfs_lookup(tcl_volume *v, const wchar_t *rel, sqfs_inode *out)
{
    char *u8;
    bool ok;

    if (!rel || !*rel)
        return tcl_sqfs_resolve_u8(v, "", out);

    u8 = tcl_w_to_u8(rel);
    if (!u8)
        return false;
    for (char *c = u8; *c; c++)
        if (*c == '\\')
            *c = '/';
    ok = tcl_sqfs_resolve_u8(v, u8, out);
    LocalFree(u8);
    return ok;
}

/* ----------------------------------------------------------- stats */

void tcl_sqfs_fill(tcl_volume *v, sqfs_inode *ino, tcl_dirent *out)
{
    sqfs_mode_t m = sqfs_mode(ino->base.inode_type);

    out->is_dir    = S_ISDIR(m) != 0;
    out->is_link   = S_ISLNK(m) != 0;
    out->read_only = true;
    out->size      = S_ISREG(m) ? (uint64_t)ino->xtra.reg.file_size : 0;
    out->mtime     = ino->base.mtime;
    out->atime     = ino->base.mtime;   /* SquashFS stores one stamp only */
    out->ctime     = ino->base.mtime;
}

int tcl_sqfs_statfs(tcl_volume *v, uint64_t *total, uint64_t *freebytes)
{
    sqfs *fs = tcl_sqfs_of(v);

    if (!fs)
        return EIO;
    /* A SquashFS image is exactly full: everything is allocated, nothing is
       free. Reporting the partition size as total would overstate it. */
    *total     = (uint64_t)fs->sb.bytes_used;
    *freebytes = 0;
    return EOK;
}
