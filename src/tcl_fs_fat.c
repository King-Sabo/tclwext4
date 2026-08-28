/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_fs_fat.c - FAT backend (FatFs) and its diskio glue.
 *
 * Images only. On a physical disk Windows has its own FAT driver with its own
 * cache for the same sectors, and nothing arbitrates between the two; on an
 * image file there is no second writer, so the hazard does not exist. The
 * scanner enforces this - see tcl_scan.c.
 *
 * FatFs addresses volumes by a physical drive number handed to disk_read /
 * disk_write, which is exactly the per-volume context lwext4's blockdev gets
 * through its struct pointer. g_pdrv[] maps one to the other.
 */
#include "tcl_fs.h"
#include "ff.h"
#include "diskio.h"
#include <stdio.h>

#define TCL_FAT_MAX FF_VOLUMES

typedef struct {
    tcl_volume *vol;
    FATFS       fs;
    bool        in_use;
} fat_slot;

static fat_slot g_pdrv[TCL_FAT_MAX];

static tcl_volume *pdrv_vol(BYTE pdrv)
{
    if (pdrv >= TCL_FAT_MAX || !g_pdrv[pdrv].in_use)
        return NULL;
    return g_pdrv[pdrv].vol;
}

/* ------------------------------------------------------ diskio glue */

DSTATUS disk_status(BYTE pdrv)
{
    tcl_volume *v = pdrv_vol(pdrv);

    if (!v)
        return STA_NOINIT;
    return v->read_only ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    tcl_volume *v = pdrv_vol(pdrv);
    LARGE_INTEGER li;
    DWORD len, got = 0;

    if (!v)
        return RES_NOTRDY;

    /* Sectors are relative to the partition; fold in its offset here so FatFs
       sees the partition as a whole device (super-floppy layout). */
    li.QuadPart = (LONGLONG)(v->part.offset + (uint64_t)sector * v->part.sector);
    len = count * v->part.sector;

    if (!SetFilePointerEx(v->bdev.h, li, NULL, FILE_BEGIN))
        return RES_ERROR;
    if (!ReadFile(v->bdev.h, buff, len, &got, NULL) || got != len)
        return RES_ERROR;
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    tcl_volume *v = pdrv_vol(pdrv);
    LARGE_INTEGER li;
    DWORD len, put = 0;

    if (!v)
        return RES_NOTRDY;
    if (v->read_only || !v->bdev.writable)
        return RES_WRPRT;

    li.QuadPart = (LONGLONG)(v->part.offset + (uint64_t)sector * v->part.sector);
    len = count * v->part.sector;

    if (!SetFilePointerEx(v->bdev.h, li, NULL, FILE_BEGIN))
        return RES_ERROR;
    if (!WriteFile(v->bdev.h, buff, len, &put, NULL) || put != len)
        return RES_ERROR;
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    tcl_volume *v = pdrv_vol(pdrv);

    if (!v)
        return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        FlushFileBuffers(v->bdev.h);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)v->part.sector;
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = (LBA_t)(v->part.size / v->part.sector);
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    }
    return RES_PARERR;
}

/*
 * FatFs stamps this on every file it creates or modifies. Local time is what
 * FAT stores - it has no timezone concept at all.
 */
DWORD get_fattime(void)
{
    SYSTEMTIME st;

    GetLocalTime(&st);
    if (st.wYear < 1980)
        return ((DWORD)0 << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);

    return ((DWORD)(st.wYear - 1980) << 25)
         | ((DWORD)st.wMonth          << 21)
         | ((DWORD)st.wDay            << 16)
         | ((DWORD)st.wHour           << 11)
         | ((DWORD)st.wMinute         <<  5)
         | ((DWORD)st.wSecond / 2);
}

/* ------------------------------------------------- mount / unmount */

int tcl_fat_mount(tcl_volume *v)
{
    int slot;
    wchar_t drv[8];
    bool want_write;
    FRESULT r;

    for (slot = 0; slot < TCL_FAT_MAX; slot++)
        if (!g_pdrv[slot].in_use)
            break;
    if (slot == TCL_FAT_MAX)
        return ENOSPC;

    want_write = !g_global_ro && !v->part.force_ro;

    if (tcl_bdev_open(&v->bdev, v->part.backing, v->part.offset,
                      v->part.size, v->part.sector, want_write) != EOK)
        return EIO;

    v->read_only = !v->bdev.writable;

    g_pdrv[slot].vol    = v;
    g_pdrv[slot].in_use = true;
    v->fat_pdrv = slot;

    swprintf_s(drv, _countof(drv), L"%d:", slot);
    r = f_mount(&g_pdrv[slot].fs, drv, 1 /* mount now */);
    if (r != FR_OK) {
        g_pdrv[slot].in_use = false;
        tcl_bdev_close(&v->bdev);
        tcl_logf(L"tclwext4: FAT mount of %s failed (FRESULT %d)", v->part.label, r);
        return EIO;
    }

    v->mounted = true;
    tcl_logf(L"tclwext4: mounted %s as FAT%s %s", v->part.label,
             g_pdrv[slot].fs.fs_type == FS_FAT32 ? L"32" :
             g_pdrv[slot].fs.fs_type == FS_FAT16 ? L"16" : L"12",
             v->read_only ? L"read-only" : L"read-write");
    return EOK;
}

void tcl_fat_unmount(tcl_volume *v)
{
    wchar_t drv[8];

    if (!v->mounted)
        return;

    swprintf_s(drv, _countof(drv), L"%d:", v->fat_pdrv);
    f_mount(NULL, drv, 0);
    FlushFileBuffers(v->bdev.h);

    if (v->fat_pdrv >= 0 && v->fat_pdrv < TCL_FAT_MAX)
        g_pdrv[v->fat_pdrv].in_use = false;
    v->fat_pdrv = -1;

    tcl_bdev_close(&v->bdev);
    v->mounted = false;
}

/* ------------------------------------------------------ path mapping */

/* "\Vol\dir\file" tail -> "N:/dir/file" */
void tcl_fat_path(tcl_volume *v, const wchar_t *rel, wchar_t *out, size_t n)
{
    size_t i;

    swprintf_s(out, n, L"%d:/", v->fat_pdrv);
    if (rel && *rel) {
        size_t base = wcslen(out);
        wcsncat_s(out, n, rel, _TRUNCATE);
        for (i = base; out[i]; i++)
            if (out[i] == L'\\')
                out[i] = L'/';
    }
}

/* ---------------------------------------------------- time helpers */

void tcl_fat_time_to_unix(WORD fdate, WORD ftime, uint32_t *unix_out)
{
    SYSTEMTIME st;
    FILETIME   loc, ft;

    ZeroMemory(&st, sizeof(st));
    st.wYear   = (WORD)(1980 + ((fdate >> 9) & 0x7F));
    st.wMonth  = (WORD)((fdate >> 5) & 0x0F);
    st.wDay    = (WORD)(fdate & 0x1F);
    st.wHour   = (WORD)((ftime >> 11) & 0x1F);
    st.wMinute = (WORD)((ftime >> 5) & 0x3F);
    st.wSecond = (WORD)((ftime & 0x1F) * 2);

    if (!st.wMonth || !st.wDay ||
        !SystemTimeToFileTime(&st, &loc) ||
        !LocalFileTimeToFileTime(&loc, &ft)) {
        *unix_out = 0;
        return;
    }
    *unix_out = tcl_unix_from_filetime(&ft);
}

void tcl_fat_time_from_unix(uint32_t secs, WORD *fdate, WORD *ftime)
{
    FILETIME ft, loc;
    SYSTEMTIME st;
    uint64_t v = ((uint64_t)secs * 10000000ULL) + 116444736000000000ULL;

    ft.dwLowDateTime  = (DWORD)v;
    ft.dwHighDateTime = (DWORD)(v >> 32);

    if (!FileTimeToLocalFileTime(&ft, &loc) || !FileTimeToSystemTime(&loc, &st) ||
        st.wYear < 1980) {
        *fdate = (1 << 5) | 1;   /* 1980-01-01 */
        *ftime = 0;
        return;
    }
    *fdate = (WORD)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    *ftime = (WORD)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
}
