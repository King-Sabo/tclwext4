/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_content.c - TC content plugin interface (custom columns).
 *
 * Fields apply to the root-level volume entries only, i.e. paths of the form
 * "\Disk1p1". Anything deeper reports ft_fieldempty: per-file ext attributes
 * (mode, uid/gid) would be useful too, but they need an inode read per row and
 * belong behind CONTENT_DELAYIFSLOW as a separate change.
 *
 * "Free" is the only field that requires the volume to be mounted. When TC asks
 * with CONTENT_DELAYIFSLOW we return ft_delayed rather than mounting on the
 * panel-drawing thread; TC then re-asks from its background thread.
 */
#include "tclwext4.h"
#include "wfxplugin.h"
#include <stdio.h>

enum {
    FLD_STATUS = 0,
    FLD_TYPE,
    FLD_RO_REASON,
    FLD_LABEL,
    FLD_UUID,
    FLD_BLOCKSIZE,
    FLD_SIZE,
    FLD_FREE,
    FLD_FEATURES,
    FLD_BACKING,
    FLD_COUNT
};

static const struct {
    const char *name;
    const char *units;
    int         type;
} g_fields[FLD_COUNT] = {
    { "Status",        "",      ft_stringw    },
    { "Type",          "",      ft_stringw    },
    { "Read-only why", "",      ft_stringw    },
    { "Volume label",  "",      ft_stringw    },
    { "UUID",          "",      ft_stringw    },
    { "Block size",    "bytes", ft_numeric_32 },
    { "Size",          "bytes", ft_numeric_64 },
    { "Free",          "bytes", ft_numeric_64 },
    { "Features",      "",      ft_stringw    },
    { "Backing store", "",      ft_stringw    },
};

/*
 * Resolve a root-level volume entry. Returns NULL for the root itself and for
 * anything below a volume, so per-file rows simply come back empty.
 */
static tcl_volume *root_volume(const wchar_t *path)
{
    const wchar_t *p = path;
    wchar_t name[64];
    size_t len;

    if (!p || !*p)
        return NULL;
    if (*p == L'\\')
        p++;
    if (!*p)
        return NULL;
    if (wcschr(p, L'\\'))
        return NULL;          /* deeper than the volume level */

    len = wcslen(p);
    if (len >= _countof(name))
        return NULL;
    wcscpy_s(name, _countof(name), p);
    return tcl_vol_find(name);
}

static void put_w(void *dst, int maxlen, const wchar_t *src)
{
    size_t cch = (size_t)maxlen / sizeof(wchar_t);
    if (cch)
        wcsncpy_s((wchar_t *)dst, cch, src, _TRUNCATE);
}

int __stdcall FsContentGetSupportedField(int FieldIndex, char *FieldName,
                                         char *Units, int maxlen)
{
    if (FieldIndex < 0 || FieldIndex >= FLD_COUNT)
        return ft_nomorefields;

    strncpy_s(FieldName, maxlen, g_fields[FieldIndex].name, _TRUNCATE);
    strncpy_s(Units, maxlen, g_fields[FieldIndex].units, _TRUNCATE);
    return g_fields[FieldIndex].type;
}

int __stdcall FsContentGetSupportedFieldFlags(int FieldIndex)
{
    (void)FieldIndex;
    return 0;   /* nothing editable, no size substitution */
}

int __stdcall FsContentGetValueW(WCHAR *FileName, int FieldIndex, int UnitIndex,
                                 void *FieldValue, int maxlen, int flags)
{
    tcl_volume *v;
    wchar_t buf[256];

    (void)UnitIndex;
    if (FieldIndex < 0 || FieldIndex >= FLD_COUNT)
        return ft_nosuchfield;

    EnterCriticalSection(&g_ext4_cs);
    v = root_volume(FileName);
    if (!v) {
        LeaveCriticalSection(&g_ext4_cs);
        return ft_fieldempty;
    }

    switch (FieldIndex) {

    case FLD_STATUS:
        if (!v->part.mountable)
            swprintf_s(buf, _countof(buf), L"unsupported: incompat 0x%08X",
                       v->part.incompat_unsup);
        else if (v->mounted)
            wcscpy_s(buf, _countof(buf), v->read_only ? L"ro" : L"rw");
        else
            wcscpy_s(buf, _countof(buf),
                     (v->part.force_ro || g_global_ro) ? L"ro (not mounted)"
                                                       : L"rw (not mounted)");
        put_w(FieldValue, maxlen, buf);
        LeaveCriticalSection(&g_ext4_cs);
        return ft_stringw;

    case FLD_TYPE:
        put_w(FieldValue, maxlen,
              v->part.kind == TCL_SRC_IMAGE ? L"image" : L"partition");
        LeaveCriticalSection(&g_ext4_cs);
        return ft_stringw;

    case FLD_RO_REASON:
        /* After mount this can be true without a superblock reason: elevation
           or a failed write-open are only discovered at mount time. */
        if (v->part.ro_reason[0])
            wcscpy_s(buf, _countof(buf), v->part.ro_reason);
        else if (v->mounted && v->read_only)
            wcscpy_s(buf, _countof(buf),
                     (v->part.kind == TCL_SRC_DISK && !tcl_is_elevated())
                         ? L"not elevated" : L"device opened read-only");
        else if (g_global_ro)
            wcscpy_s(buf, _countof(buf), L"readonly=1 in ini");
        else
            buf[0] = 0;
        LeaveCriticalSection(&g_ext4_cs);
        if (!buf[0])
            return ft_fieldempty;
        put_w(FieldValue, maxlen, buf);
        return ft_stringw;

    case FLD_LABEL:
        wcscpy_s(buf, _countof(buf), v->part.fslabel);
        LeaveCriticalSection(&g_ext4_cs);
        if (!buf[0])
            return ft_fieldempty;
        put_w(FieldValue, maxlen, buf);
        return ft_stringw;

    case FLD_UUID: {
        const uint8_t *u = v->part.uuid;
        swprintf_s(buf, _countof(buf),
                   L"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                   u[0],u[1],u[2],u[3], u[4],u[5], u[6],u[7],
                   u[8],u[9], u[10],u[11],u[12],u[13],u[14],u[15]);
        LeaveCriticalSection(&g_ext4_cs);
        put_w(FieldValue, maxlen, buf);
        return ft_stringw;
    }

    case FLD_BLOCKSIZE:
        *(int *)FieldValue = (int)v->part.block_size;
        LeaveCriticalSection(&g_ext4_cs);
        return ft_numeric_32;

    case FLD_SIZE:
        *(int64_t *)FieldValue = (int64_t)v->part.size;
        LeaveCriticalSection(&g_ext4_cs);
        return ft_numeric_64;

    case FLD_FREE: {
        struct ext4_mount_stats st;
        int64_t freeb;

        if (!v->mounted) {
            if (flags & CONTENT_DELAYIFSLOW) {
                LeaveCriticalSection(&g_ext4_cs);
                return ft_delayed;      /* TC re-asks on its background thread */
            }
            if (tcl_vol_mount(v) != EOK) {
                LeaveCriticalSection(&g_ext4_cs);
                return ft_fieldempty;
            }
        }
        if (ext4_mount_point_stats(v->mp, &st) != EOK) {
            LeaveCriticalSection(&g_ext4_cs);
            return ft_fieldempty;
        }
        freeb = (int64_t)st.free_blocks_count * (int64_t)st.block_size;
        LeaveCriticalSection(&g_ext4_cs);
        *(int64_t *)FieldValue = freeb;
        return ft_numeric_64;
    }

    case FLD_FEATURES:
        swprintf_s(buf, _countof(buf), L"c:%08X i:%08X r:%08X",
                   v->part.f_compat, v->part.f_incompat, v->part.f_ro_compat);
        LeaveCriticalSection(&g_ext4_cs);
        put_w(FieldValue, maxlen, buf);
        return ft_stringw;

    case FLD_BACKING:
        wcscpy_s(buf, _countof(buf), v->part.backing);
        LeaveCriticalSection(&g_ext4_cs);
        put_w(FieldValue, maxlen, buf);
        return ft_stringw;
    }

    LeaveCriticalSection(&g_ext4_cs);
    return ft_nosuchfield;
}

/* ANSI fallback: TC uses the W variant when present, but export both. */
int __stdcall FsContentGetValue(char *FileName, int FieldIndex, int UnitIndex,
                                void *FieldValue, int maxlen, int flags)
{
    wchar_t wpath[MAX_PATH];
    int rc;

    MultiByteToWideChar(CP_ACP, 0, FileName, -1, wpath, MAX_PATH);
    rc = FsContentGetValueW(wpath, FieldIndex, UnitIndex, FieldValue, maxlen, flags);

    /* Collapse wide strings back to ANSI in place. */
    if (rc == ft_stringw) {
        char tmp[512];
        WideCharToMultiByte(CP_ACP, 0, (wchar_t *)FieldValue, -1,
                            tmp, sizeof(tmp), NULL, NULL);
        strncpy_s((char *)FieldValue, maxlen, tmp, _TRUNCATE);
        return ft_string;
    }
    return rc;
}

void __stdcall FsContentStopGetValueW(WCHAR *FileName)
{
    (void)FileName;   /* nothing here blocks long enough to need cancelling */
}

void __stdcall FsContentPluginUnloading(void)
{
}
