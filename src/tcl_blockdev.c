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
    DWORD  err = 0;

    if (sect > 1)
        total = ((total + sect - 1) / sect) * sect;

    tmp = (BYTE *)VirtualAlloc(NULL, total, MEM_COMMIT, PAGE_READWRITE);
    if (!tmp)
        return false;

    li.QuadPart = (LONGLONG)start;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
        err = GetLastError();
    } else if (!ReadFile(h, tmp, total, &got, NULL)) {
        err = GetLastError();
    } else if (got < skew + len) {
        err = 0;                    /* short read, not a Win32 failure */
    } else {
        memcpy(buf, tmp + skew, len);
        ok = true;
    }

    /* Capture the error before VirtualFree, which can overwrite it even when
       it succeeds. */
    VirtualFree(tmp, 0, MEM_RELEASE);
    if (!ok)
        tcl_dbgf(L"tclwext4: read at %llu (%u bytes, sector %u) failed: "
                 L"error %u, got %u",
                 (unsigned long long)off, len, sect, err, got);
    SetLastError(err);
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

/*
 * FILE_FLAG_NO_BUFFERING, which physical drives are opened with, requires the
 * DESTINATION BUFFER to be sector-aligned in memory - not merely the file
 * offset and the length. lwext4's block cache buffers come from its own
 * allocator with no such guarantee, so an unaligned one makes ReadFile fail
 * with ERROR_INVALID_PARAMETER, which surfaces as ext4_mount() returning EIO.
 *
 * Images are opened buffered and have no alignment requirement at all, which is
 * why this only ever bit physical disks - and why the scanner never saw it: its
 * buffers come from VirtualAlloc and are page-aligned by construction.
 *
 * Grow a page-aligned scratch buffer on demand and bounce through it. The
 * blockdev lock is already held by lwext4 around these calls, so one scratch
 * per device is safe.
 */
static uint8_t *bounce_for(tcl_bdev *b, size_t len)
{
    if (b->bounce_len >= len)
        return b->bounce;
    if (b->bounce)
        VirtualFree(b->bounce, 0, MEM_RELEASE);
    b->bounce = (uint8_t *)VirtualAlloc(NULL, len, MEM_COMMIT, PAGE_READWRITE);
    b->bounce_len = b->bounce ? len : 0;
    return b->bounce;
}

static bool needs_bounce(tcl_bdev *b, const void *buf)
{
    if (!b->unbuffered)
        return false;
    return ((uintptr_t)buf & (uintptr_t)(b->iface.ph_bsize - 1)) != 0;
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

    if (needs_bounce(b, buf)) {
        uint8_t *tmp = bounce_for(b, len);
        if (!tmp)
            return ENOMEM;
        if (!ReadFile(b->h, tmp, len, &got, NULL) || got != len) {
            tcl_logf(L"tclwext4: read of %u bytes at block %llu failed: error %u",
                     len, (unsigned long long)blk_id, GetLastError());
            return EIO;
        }
        memcpy(buf, tmp, len);
        return EOK;
    }

    if (!ReadFile(b->h, buf, len, &got, NULL) || got != len) {
        tcl_logf(L"tclwext4: read of %u bytes at block %llu failed: error %u",
                 len, (unsigned long long)blk_id, GetLastError());
        return EIO;
    }
    return EOK;
}

/*
 * Windows lets a write-protected device be OPENED with GENERIC_WRITE; the
 * refusal only arrives at the first write, as ERROR_WRITE_PROTECT. Record that
 * on the device so callers can retry read-only instead of failing outright.
 */
static int bd_write_failed(tcl_bdev *b, DWORD len, uint64_t blk_id)
{
    DWORD err = GetLastError();

    tcl_logf(L"tclwext4: write of %u bytes at block %llu failed: error %u%s",
             len, (unsigned long long)blk_id, err,
             err == ERROR_WRITE_PROTECT ? L" (media is write protected)" : L"");

    if (err == ERROR_WRITE_PROTECT) {
        b->writable = false;
        return EROFS;
    }
    return EIO;
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
    if (!b->writable) {
        tcl_dbgf(L"tclwext4: write to block %llu refused: device is read-only",
                 (unsigned long long)blk_id);
        return EROFS;
    }

    li.QuadPart = (LONGLONG)(blk_id * bdev->bdif->ph_bsize);
    if (!SetFilePointerEx(b->h, li, NULL, FILE_BEGIN))
        return EIO;

    if (needs_bounce(b, (void *)buf)) {
        uint8_t *tmp = bounce_for(b, len);
        if (!tmp)
            return ENOMEM;
        memcpy(tmp, buf, len);
        if (!WriteFile(b->h, tmp, len, &put, NULL) || put != len)
            return bd_write_failed(b, len, blk_id);
        return EOK;
    }

    if (!WriteFile(b->h, buf, len, &put, NULL) || put != len)
        return bd_write_failed(b, len, blk_id);
    return EOK;
}

/* ---------------------------------------------------------------- open */

int tcl_bdev_open(tcl_bdev *b, const wchar_t *path, uint64_t part_offset,
                  uint64_t part_size, uint32_t sector_size, bool want_write)
{
    DWORD access = GENERIC_READ | (want_write ? GENERIC_WRITE : 0);
    LARGE_INTEGER sz;
    bool is_dev = (wcsncmp(path, L"\\\\.\\", 4) == 0);
    DWORD devflags = is_dev && g_no_buffering
                       ? (FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH)
                       : (is_dev ? FILE_FLAG_WRITE_THROUGH : 0);

    ZeroMemory(b, sizeof(*b));
    /*
     * ZeroMemory leaves h as NULL, and NULL is NOT INVALID_HANDLE_VALUE. Every
     * "did the open fail?" test below compares against INVALID_HANDLE_VALUE, so
     * without this the read-only path skips its CreateFileW entirely and the
     * device is used with a NULL handle - which ReadFile rejects with
     * ERROR_INVALID_HANDLE long after the real mistake.
     */
    b->h = INVALID_HANDLE_VALUE;
    wcsncpy_s(b->path, MAX_PATH, path, _TRUNCATE);

    /*
     * Ask the device whether it is writable BEFORE trying to write to it.
     *
     * IOCTL_DISK_IS_WRITABLE answers without transferring anything, and fails
     * with ERROR_WRITE_PROTECT on locked media. Without this the first sign of
     * trouble is lwext4's superblock update failing mid-mount, which leaves the
     * mount half-built and forces a tear-down-and-retry that has its own ways
     * of going wrong.
     */
    if (is_dev && want_write) {
        HANDLE probe = CreateFileW(path, GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, 0, NULL);
        if (probe != INVALID_HANDLE_VALUE) {
            DWORD ret = 0;
            if (!DeviceIoControl(probe, IOCTL_DISK_IS_WRITABLE, NULL, 0,
                                 NULL, 0, &ret, NULL)) {
                DWORD err = GetLastError();
                if (err == ERROR_WRITE_PROTECT) {
                    tcl_logf(L"tclwext4: %s reports write protection; "
                             L"opening read-only", path);
                    want_write = false;
                    access = GENERIC_READ;
                }
            }
            CloseHandle(probe);
        }
    }

    /*
     * Shared access on purpose. An exclusive handle was tried and made no
     * difference to whether writes are accepted, while it does stop imaging
     * tools from locking the disk for as long as a volume is mounted here.
     */
    b->h = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, devflags, NULL);

    b->writable   = want_write;
    b->unbuffered = is_dev && g_no_buffering;
    if (is_dev)
        tcl_logf(L"tclwext4: %s opened %s, %s I/O (handle %p)", path,
                 b->writable ? L"read-write" : L"read-only",
                 g_no_buffering ? L"direct" : L"buffered", b->h);

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
    if (b->bounce) {
        VirtualFree(b->bounce, 0, MEM_RELEASE);
        b->bounce = NULL;
        b->bounce_len = 0;
    }
    if (b->cs.DebugInfo || b->cs.LockCount)
        DeleteCriticalSection(&b->cs);
    ZeroMemory(&b->cs, sizeof(b->cs));
}
