/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_blockdev.c - ext4_blockdev implementation over a Win32 HANDLE.
 *
 * lwext4 hands us absolute physical block indices (part_offset is already
 * folded in by ext4_blockdev.c), so blk_id * ph_bsize is a byte offset from
 * the start of the backing store.
 *
 * Physical drives require sector-aligned offsets and lengths; ph_bsize is set
 * to the drive's logical sector size, which guarantees that.
 */
#include "tclwext4.h"
#include <winioctl.h>

uint32_t tcl_query_sector_size(HANDLE h)
{
    DISK_GEOMETRY_EX gx;
    STORAGE_PROPERTY_QUERY q;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR ad;
    DWORD ret = 0;

    ZeroMemory(&q, sizeof(q));
    q.PropertyId = StorageAccessAlignmentProperty;
    q.QueryType  = PropertyStandardQuery;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                        &ad, sizeof(ad), &ret, NULL) && ad.BytesPerLogicalSector)
        return ad.BytesPerLogicalSector;

    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                        &gx, sizeof(gx), &ret, NULL) && gx.Geometry.BytesPerSector)
        return gx.Geometry.BytesPerSector;

    return 512;
}

bool tcl_read_at(HANDLE h, uint64_t off, void *buf, DWORD len, uint32_t sect)
{
    /* Align down to a sector boundary, read whole sectors, then memmove. */
    uint64_t start = (sect > 1) ? (off / sect) * (uint64_t)sect : off;
    DWORD    skew  = (DWORD)(off - start);
    DWORD    total = skew + len;
    DWORD    got   = 0;
    LARGE_INTEGER li;
    BYTE  *tmp;
    bool   ok = false;

    if (sect > 1)
        total = ((total + sect - 1) / sect) * sect;

    tmp = (BYTE *)VirtualAlloc(NULL, total, MEM_COMMIT, PAGE_READWRITE);
    if (!tmp)
        return false;

    li.QuadPart = (LONGLONG)start;
    if (SetFilePointerEx(h, li, NULL, FILE_BEGIN) &&
        ReadFile(h, tmp, total, &got, NULL) && got >= skew + len) {
        memcpy(buf, tmp + skew, len);
        ok = true;
    }
    VirtualFree(tmp, 0, MEM_RELEASE);
    return ok;
}

/* ------------------------------------------------------------ bdif ops */

static int bd_open(struct ext4_blockdev *bdev)
{
    (void)bdev;
    return EOK;   /* handle is opened eagerly in tcl_bdev_open() */
}

static int bd_close(struct ext4_blockdev *bdev)
{
    (void)bdev;
    return EOK;
}

static int bd_lock(struct ext4_blockdev *bdev)
{
    tcl_bdev *b = CONTAINING_RECORD(bdev, tcl_bdev, bd);
    EnterCriticalSection(&b->cs);
    return EOK;
}

static int bd_unlock(struct ext4_blockdev *bdev)
{
    tcl_bdev *b = CONTAINING_RECORD(bdev, tcl_bdev, bd);
    LeaveCriticalSection(&b->cs);
    return EOK;
}

static int bd_bread(struct ext4_blockdev *bdev, void *buf,
                    uint64_t blk_id, uint32_t blk_cnt)
{
    tcl_bdev *b = CONTAINING_RECORD(bdev, tcl_bdev, bd);
    LARGE_INTEGER li;
    DWORD len = blk_cnt * bdev->bdif->ph_bsize;
    DWORD got = 0;

    if (!blk_cnt)
        return EOK;

    li.QuadPart = (LONGLONG)(blk_id * bdev->bdif->ph_bsize);
    if (!SetFilePointerEx(b->h, li, NULL, FILE_BEGIN))
        return EIO;
    if (!ReadFile(b->h, buf, len, &got, NULL) || got != len)
        return EIO;
    return EOK;
}

static int bd_bwrite(struct ext4_blockdev *bdev, const void *buf,
                     uint64_t blk_id, uint32_t blk_cnt)
{
    tcl_bdev *b = CONTAINING_RECORD(bdev, tcl_bdev, bd);
    LARGE_INTEGER li;
    DWORD len = blk_cnt * bdev->bdif->ph_bsize;
    DWORD put = 0;

    if (!blk_cnt)
        return EOK;
    if (!b->writable)
        return EROFS;

    li.QuadPart = (LONGLONG)(blk_id * bdev->bdif->ph_bsize);
    if (!SetFilePointerEx(b->h, li, NULL, FILE_BEGIN))
        return EIO;
    if (!WriteFile(b->h, buf, len, &put, NULL) || put != len)
        return EIO;
    return EOK;
}

/* ---------------------------------------------------------------- open */

int tcl_bdev_open(tcl_bdev *b, const wchar_t *path, uint64_t part_offset,
                  uint64_t part_size, uint32_t sector_size, bool want_write)
{
    DWORD access = GENERIC_READ | (want_write ? GENERIC_WRITE : 0);
    LARGE_INTEGER sz;
    bool is_dev = (wcsncmp(path, L"\\\\.\\", 4) == 0);

    ZeroMemory(b, sizeof(*b));
    wcsncpy_s(b->path, MAX_PATH, path, _TRUNCATE);

    b->h = CreateFileW(path, access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING,
                       is_dev ? FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH : 0,
                       NULL);
    if (b->h == INVALID_HANDLE_VALUE && want_write) {
        /* Fall back to read-only access. */
        b->h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING,
                           is_dev ? FILE_FLAG_NO_BUFFERING : 0, NULL);
        want_write = false;
    }
    if (b->h == INVALID_HANDLE_VALUE)
        return EIO;

    b->writable = want_write;

    if (!sector_size)
        sector_size = is_dev ? tcl_query_sector_size(b->h) : 512;

    if (!part_size) {
        if (is_dev) {
            GET_LENGTH_INFORMATION gli;
            DWORD ret = 0;
            if (!DeviceIoControl(b->h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                                 &gli, sizeof(gli), &ret, NULL)) {
                CloseHandle(b->h);
                return EIO;
            }
            part_size = (uint64_t)gli.Length.QuadPart - part_offset;
        } else {
            if (!GetFileSizeEx(b->h, &sz)) {
                CloseHandle(b->h);
                return EIO;
            }
            part_size = (uint64_t)sz.QuadPart - part_offset;
        }
    }

    if (part_offset % sector_size) {
        CloseHandle(b->h);
        return EINVAL;
    }

    b->ph_bbuf = (uint8_t *)VirtualAlloc(NULL, sector_size, MEM_COMMIT, PAGE_READWRITE);
    if (!b->ph_bbuf) {
        CloseHandle(b->h);
        return ENOMEM;
    }

    InitializeCriticalSection(&b->cs);

    b->iface.open     = bd_open;
    b->iface.close    = bd_close;
    b->iface.bread    = bd_bread;
    b->iface.bwrite   = bd_bwrite;
    b->iface.lock     = bd_lock;
    b->iface.unlock   = bd_unlock;
    b->iface.ph_bsize = sector_size;
    b->iface.ph_bcnt  = (part_offset + part_size) / sector_size;
    b->iface.ph_bbuf  = b->ph_bbuf;

    b->bd.bdif        = &b->iface;
    b->bd.part_offset = part_offset;
    b->bd.part_size   = (part_size / sector_size) * sector_size;

    return EOK;
}

void tcl_bdev_close(tcl_bdev *b)
{
    if (b->h && b->h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(b->h);
        CloseHandle(b->h);
        b->h = INVALID_HANDLE_VALUE;
    }
    if (b->ph_bbuf) {
        VirtualFree(b->ph_bbuf, 0, MEM_RELEASE);
        b->ph_bbuf = NULL;
    }
    if (b->cs.DebugInfo || b->cs.LockCount)
        DeleteCriticalSection(&b->cs);
    ZeroMemory(&b->cs, sizeof(b->cs));
}
