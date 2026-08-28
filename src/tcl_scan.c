/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_scan.c - find ext2/3/4 partitions on physical drives and in images.
 *
 * Physical drives: Windows already parses MBR (incl. logical partitions in
 * the EBR chain) and GPT for us via IOCTL_DISK_GET_DRIVE_LAYOUT_EX, so we use
 * that and then probe each partition for an ext superblock. We do NOT filter
 * on partition type - a 0x83 / Linux-filesystem-data GUID is a hint, not a
 * guarantee, and plenty of ext filesystems live under other type codes.
 *
 * Images: Windows will not parse a file, so MBR + EBR chain + GPT are parsed
 * here. An image that is a bare dd of a single partition has no table; the
 * whole file is probed as one volume.
 */
#include "tclwext4.h"
#include <winioctl.h>
#include <stdio.h>

/* ------------------------------------------------------ ext superblock */

#define SB_OFFSET            1024
#define SB_MAGIC             0xEF53

#define SBO_S_BLOCKS_LO      0x004
#define SBO_S_LOG_BLOCK_SIZE 0x018
#define SBO_S_MAGIC          0x038
#define SBO_S_STATE          0x03A
#define SBO_S_FEATURE_COMPAT 0x05C
#define SBO_S_FEATURE_INCOMP 0x060
#define SBO_S_FEATURE_ROCOMP 0x064
#define SBO_S_UUID           0x068
#define SBO_S_VOLUME_NAME    0x078
#define SBO_S_BLOCKS_HI      0x150

#define EXT4_VALID_FS        0x0001

/* ro_compat bits e2fsprogs writes that lwext4 has no notion of. Named here
 * only so the "why is this read-only" message can be specific. */
#define RO_COM_QUOTA         0x0100
#define RO_COM_BIGALLOC      0x0200
#define RO_COM_READONLY      0x1000
#define RO_COM_PROJECT       0x2000
#define RO_COM_SHARED_BLOCKS 0x4000
#define RO_COM_VERITY        0x8000
#define RO_COM_ORPHAN_PRESENT 0x10000  /* verify against your ext2_fs.h */

#define INCOM_ENCRYPT        0x10000
#define INCOM_CASEFOLD       0x20000

static uint16_t rd16(const uint8_t *p, int off) {
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}
static uint32_t rd32(const uint8_t *p, int off) {
    return (uint32_t)p[off] | ((uint32_t)p[off+1] << 8) |
           ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
}
static uint64_t rd64(const uint8_t *p, int off) {
    return (uint64_t)rd32(p, off) | ((uint64_t)rd32(p, off + 4) << 32);
}

static void describe_ro(tcl_part *p)
{
    wchar_t *d = p->ro_reason;
    size_t   n = _countof(p->ro_reason);

    d[0] = 0;
    if (p->ro_unsup & RO_COM_ORPHAN_PRESENT) wcscat_s(d, n, L"orphan_present ");
    if (p->ro_unsup & RO_COM_READONLY)      wcscat_s(d, n, L"read-only ");
    if (p->ro_unsup & RO_COM_QUOTA)         wcscat_s(d, n, L"quota ");
    if (p->ro_unsup & RO_COM_BIGALLOC)      wcscat_s(d, n, L"bigalloc ");
    if (p->ro_unsup & RO_COM_PROJECT)       wcscat_s(d, n, L"project ");
    if (p->ro_unsup & RO_COM_SHARED_BLOCKS) wcscat_s(d, n, L"shared_blocks ");
    if (p->ro_unsup & RO_COM_VERITY)        wcscat_s(d, n, L"verity ");
    if (!d[0] && p->ro_unsup)
        swprintf_s(d, n, L"ro_compat 0x%08X ", p->ro_unsup);
}

bool tcl_probe_ext(HANDLE h, uint64_t off, uint64_t size, uint32_t sect, tcl_part *p)
{
    uint8_t sb[1024];
    uint32_t incomp, rocomp, compat, log_bs;
    uint16_t state;
    int i;

    if (size < 2 * 1024 * 1024)
        return false;
    if (!tcl_read_at(h, off + SB_OFFSET, sb, sizeof(sb), sect))
        return false;
    if (rd16(sb, SBO_S_MAGIC) != SB_MAGIC)
        return false;

    compat = rd32(sb, SBO_S_FEATURE_COMPAT);
    incomp = rd32(sb, SBO_S_FEATURE_INCOMP);
    rocomp = rd32(sb, SBO_S_FEATURE_ROCOMP);
    state  = rd16(sb, SBO_S_STATE);
    log_bs = rd32(sb, SBO_S_LOG_BLOCK_SIZE);

    p->offset     = off;
    p->size       = size;
    p->sector     = sect;
    p->f_compat   = compat;
    p->f_incompat = incomp;
    p->f_ro_compat= rocomp;
    p->block_size = 1024u << (log_bs & 31);
    p->blocks     = (uint64_t)rd32(sb, SBO_S_BLOCKS_LO) |
                    ((uint64_t)rd32(sb, SBO_S_BLOCKS_HI) << 32);
    memcpy(p->uuid, sb + SBO_S_UUID, 16);

    /* volume label -> wide, sanitised */
    for (i = 0; i < 16; i++) {
        char c = (char)sb[SBO_S_VOLUME_NAME + i];
        if (!c) break;
        p->fslabel[i] = (unsigned char)c;
    }
    p->fslabel[i] = 0;

    /*
     * Feature gate. Masks come straight from lwext4's own ext4_types.h, so
     * this stays in sync with whatever the submodule supports.
     */
    p->incompat_unsup = incomp & ~(uint32_t)(CONFIG_SUPPORTED_FINCOM);
    p->ro_unsup       = rocomp & ~(uint32_t)(CONFIG_SUPPORTED_FRO_COM);

    p->mountable = (p->incompat_unsup == 0);
    p->force_ro  = false;
    p->ro_reason[0] = 0;

    if (p->ro_unsup) {
        p->force_ro = true;
        describe_ro(p);
    }
    if (incomp & EXT4_FINCOM_RECOVER) {
        /* lwext4 lists RECOVER as "ignored" and will happily mount over an
         * unreplayed journal. Refuse to write in that state. */
        p->force_ro = true;
        wcscat_s(p->ro_reason, _countof(p->ro_reason), L"needs-journal-recovery ");
    }
    if (incomp & EXT4_FINCOM_MMP) {
        p->force_ro = true;
        wcscat_s(p->ro_reason, _countof(p->ro_reason), L"mmp ");
    }
    if (!(state & EXT4_VALID_FS)) {
        p->force_ro = true;
        wcscat_s(p->ro_reason, _countof(p->ro_reason), L"not-cleanly-unmounted ");
    }
    return true;
}

/* ------------------------------------------------------------ FAT probe */

/*
 * Recognise a FAT12/16/32 BPB. Deliberately strict: a loose check would claim
 * partitions that merely look plausible, and this plugin writes.
 *
 * Only ever called for image partitions. On a physical disk Windows mounts FAT
 * itself and caches the same sectors, with nothing arbitrating between its
 * cache and ours; an image file has no second writer.
 */
bool tcl_probe_fat(HANDLE h, uint64_t off, uint64_t size, uint32_t sect, tcl_part *p)
{
    uint8_t bs[512];
    uint16_t bytes_per_sec, rsvd, root_ents;
    uint8_t  spc, nfats;
    uint32_t tot16, tot32, fatsz16, fatsz32, fatsz, tot, rootsecs, datasecs, clusters;
    const wchar_t *why = NULL;
    bool looks_fatty = false;
    int i;

    /*
     * Every rejection below is logged when the sector carries a boot signature,
     * i.e. when it plausibly was meant to be a filesystem. Silent rejection here
     * is what makes "my ESP does not show up" impossible to diagnose.
     */
    if (size < 64 * 1024) {
        why = L"partition smaller than 64 KiB";
        goto reject;
    }
    if (!tcl_read_at(h, off, bs, sizeof(bs), sect)) {
        why = L"could not read boot sector";
        goto reject;
    }
    if (rd16(bs, 510) != 0xAA55) {
        why = L"no 0xAA55 boot signature";
        goto reject;
    }
    looks_fatty = true;

    bytes_per_sec = rd16(bs, 11);
    spc           = bs[13];
    rsvd          = rd16(bs, 14);
    nfats         = bs[16];
    root_ents     = rd16(bs, 17);
    tot16         = rd16(bs, 19);
    fatsz16       = rd16(bs, 22);
    tot32         = rd32(bs, 32);
    fatsz32       = rd32(bs, 36);

    /* A boot jump is conventional but not universal: some images are built by
       copying a filesystem without one. Warn rather than reject. */
    if (bs[0] != 0xEB && bs[0] != 0xE9 && bs[0] != 0x49)
        tcl_logf(L"tclwext4: scan: partition at %llu has no boot jump (0x%02X), continuing",
                 (unsigned long long)off, bs[0]);

    if (bytes_per_sec != 512 && bytes_per_sec != 1024 &&
        bytes_per_sec != 2048 && bytes_per_sec != 4096) {
        why = L"bytes-per-sector not 512/1024/2048/4096";
        goto reject;
    }
    if (!spc || (spc & (spc - 1))) {
        why = L"sectors-per-cluster not a power of two";
        goto reject;
    }
    if (!rsvd || nfats < 1 || nfats > 2) {
        why = L"reserved sectors or FAT count out of range";
        goto reject;
    }

    fatsz = fatsz16 ? fatsz16 : fatsz32;
    tot   = tot16 ? tot16 : tot32;
    if (!fatsz || !tot) {
        why = L"FAT size or total sector count is zero";
        goto reject;
    }

    rootsecs = ((uint32_t)root_ents * 32 + bytes_per_sec - 1) / bytes_per_sec;
    if (tot < rsvd + (uint32_t)nfats * fatsz + rootsecs) {
        why = L"geometry does not fit the declared sector count";
        goto reject;
    }
    datasecs = tot - (rsvd + (uint32_t)nfats * fatsz + rootsecs);
    clusters = datasecs / spc;
    if (!clusters) {
        why = L"no data clusters";
        goto reject;
    }

    p->offset     = off;
    p->size       = size;
    p->sector     = sect;
    p->fskind     = TCL_FSK_FAT;
    p->block_size = (uint32_t)bytes_per_sec * spc;
    p->blocks     = clusters;
    p->mountable  = true;
    p->force_ro   = false;
    p->ro_reason[0] = 0;

    /* Volume label: FAT32 keeps it at 71, FAT12/16 at 43. Space padded, not
       NUL terminated. */
    {
        int lbl = (clusters >= 65525) ? 71 : 43;
        for (i = 0; i < 11; i++)
            p->fslabel[i] = (unsigned char)bs[lbl + i];
        p->fslabel[11] = 0;
        for (i = 10; i >= 0 && p->fslabel[i] == L' '; i--)
            p->fslabel[i] = 0;
    }

    tcl_logf(L"tclwext4: scan: FAT at %llu, %u clusters of %u bytes, label '%s'",
             (unsigned long long)off, clusters, p->block_size, p->fslabel);
    return true;

reject:
    if (looks_fatty)
        tcl_logf(L"tclwext4: scan: partition at %llu is not usable FAT: %s",
                 (unsigned long long)off, why);
    return false;
}

/* --------------------------------------------------- image table parse */

static void add_part(tcl_part *out, int max, int *n, HANDLE h,
                     const wchar_t *backing, tcl_src_kind kind, int disk_no,
                     int part_no, uint64_t off, uint64_t size, uint32_t sect)
{
    tcl_part p;
    const wchar_t *base;

    if (*n >= max)
        return;

    ZeroMemory(&p, sizeof(p));
    if (tcl_probe_ext(h, off, size, sect, &p)) {
        p.fskind = TCL_FSK_EXT;
    } else if (kind == TCL_SRC_IMAGE && tcl_probe_fat(h, off, size, sect, &p)) {
        /* images only - deliberately not attempted on physical disks */
    } else {
        tcl_logf(L"tclwext4: scan: skipping partition %d at %llu (%llu bytes) - "
                 L"no recognised filesystem%s",
                 part_no, (unsigned long long)off, (unsigned long long)size,
                 kind == TCL_SRC_DISK ? L" (FAT not attempted on physical disks)" : L"");
        return;
    }
    tcl_logf(L"tclwext4: scan: partition %d at %llu -> %s", part_no,
             (unsigned long long)off, p.fskind == TCL_FSK_FAT ? L"FAT" : L"ext");

    wcsncpy_s(p.backing, MAX_PATH, backing, _TRUNCATE);
    p.kind    = kind;
    p.disk_no = disk_no;
    p.part_no = part_no;

    if (kind == TCL_SRC_DISK) {
        if (part_no)
            swprintf_s(p.label, _countof(p.label), L"Disk%dp%d", disk_no, part_no);
        else
            swprintf_s(p.label, _countof(p.label), L"Disk%d", disk_no);
    } else {
        base = wcsrchr(backing, L'\\');
        base = base ? base + 1 : backing;
        if (part_no)
            swprintf_s(p.label, _countof(p.label), L"%.40s.p%d", base, part_no);
        else
            swprintf_s(p.label, _countof(p.label), L"%.40s", base);
    }
    /* TC directory names may not contain these */
    for (wchar_t *c = p.label; *c; c++)
        if (wcschr(L"\\/:*?\"<>|", *c)) *c = L'_';

    out[(*n)++] = p;
}

static void scan_gpt_image(tcl_part *out, int max, int *n, HANDLE h,
                           const wchar_t *path, uint32_t sect)
{
    uint8_t hdr[512];
    uint8_t *tbl;
    uint64_t ent_lba;
    uint32_t ent_cnt, ent_sz;
    uint32_t i;

    if (!tcl_read_at(h, (uint64_t)sect, hdr, sizeof(hdr), sect))
        return;
    if (memcmp(hdr, "EFI PART", 8) != 0)
        return;

    ent_lba = rd64(hdr, 0x48);
    ent_cnt = rd32(hdr, 0x50);
    ent_sz  = rd32(hdr, 0x54);
    if (!ent_sz || ent_sz > 4096 || ent_cnt > 512)
        return;

    tbl = (uint8_t *)LocalAlloc(LPTR, (SIZE_T)ent_cnt * ent_sz);
    if (!tbl)
        return;
    if (tcl_read_at(h, ent_lba * sect, tbl, ent_cnt * ent_sz, sect)) {
        for (i = 0; i < ent_cnt; i++) {
            uint8_t *e = tbl + (SIZE_T)i * ent_sz;
            uint64_t first, last;
            static const uint8_t zero[16] = { 0 };
            if (memcmp(e, zero, 16) == 0)
                continue;
            first = rd64(e, 32);
            last  = rd64(e, 40);
            if (last < first)
                continue;
            add_part(out, max, n, h, path, TCL_SRC_IMAGE, -1, (int)i + 1,
                     first * sect, (last - first + 1) * sect, sect);
        }
    }
    LocalFree(tbl);
}

static void scan_mbr_image(tcl_part *out, int max, int *n, HANDLE h,
                           const wchar_t *path, uint32_t sect)
{
    uint8_t mbr[512];
    int i, part_no = 0, guard = 0;
    uint64_t ext_base = 0, ebr = 0;

    if (!tcl_read_at(h, 0, mbr, sizeof(mbr), sect))
        return;
    if (rd16(mbr, 510) != 0xAA55)
        return;

    for (i = 0; i < 4; i++) {
        uint8_t *e = mbr + 0x1BE + i * 16;
        uint8_t  type = e[4];
        uint64_t lba  = rd32(e, 8);
        uint64_t cnt  = rd32(e, 12);
        part_no++;
        if (!cnt)
            continue;
        if (type == 0x05 || type == 0x0F || type == 0x85) {
            ext_base = ebr = lba;
            continue;
        }
        if (type == 0xEE)   /* protective MBR; GPT pass handles it */
            continue;
        add_part(out, max, n, h, path, TCL_SRC_IMAGE, -1, part_no,
                 lba * sect, cnt * sect, sect);
    }

    /* logical partitions */
    part_no = 4;
    while (ebr && guard++ < 64) {
        uint8_t b[512];
        uint64_t lba, cnt, next;
        if (!tcl_read_at(h, ebr * sect, b, sizeof(b), sect))
            break;
        if (rd16(b, 510) != 0xAA55)
            break;
        lba  = rd32(b + 0x1BE, 8);
        cnt  = rd32(b + 0x1BE, 12);
        next = rd32(b + 0x1CE, 8);
        part_no++;
        if (cnt)
            add_part(out, max, n, h, path, TCL_SRC_IMAGE, -1, part_no,
                     (ebr + lba) * sect, cnt * sect, sect);
        ebr = next ? ext_base + next : 0;
    }
}

static void scan_image(tcl_part *out, int max, int *n, const wchar_t *path)
{
    HANDLE h;
    LARGE_INTEGER sz;
    int before = *n;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return;
    }

    scan_gpt_image(out, max, n, h, path, 512);
    scan_mbr_image(out, max, n, h, path, 512);

    /* Bare dd of a single partition: no table, superblock at file offset 0. */
    if (*n == before)
        add_part(out, max, n, h, path, TCL_SRC_IMAGE, -1, 0,
                 0, (uint64_t)sz.QuadPart, 512);

    CloseHandle(h);
}

/* --------------------------------------------------- physical drives */

static void scan_disk(tcl_part *out, int max, int *n, int disk_no)
{
    wchar_t path[64];
    HANDLE h;
    uint32_t sect;
    BYTE buf[8192];
    DRIVE_LAYOUT_INFORMATION_EX *lay = (DRIVE_LAYOUT_INFORMATION_EX *)buf;
    DWORD ret = 0;
    DWORD i;
    int before = *n;

    swprintf_s(path, _countof(path), L"\\\\.\\PhysicalDrive%d", disk_no);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    sect = tcl_query_sector_size(h);

    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0,
                        buf, sizeof(buf), &ret, NULL)) {
        for (i = 0; i < lay->PartitionCount; i++) {
            PARTITION_INFORMATION_EX *pi = &lay->PartitionEntry[i];
            if (pi->PartitionLength.QuadPart == 0)
                continue;
            if (pi->PartitionStyle == PARTITION_STYLE_MBR &&
                !pi->Mbr.RecognizedPartition && pi->Mbr.PartitionType == 0)
                continue;
            add_part(out, max, n, h, path, TCL_SRC_DISK, disk_no,
                     (int)pi->PartitionNumber,
                     (uint64_t)pi->StartingOffset.QuadPart,
                     (uint64_t)pi->PartitionLength.QuadPart, sect);
        }
    }

    /* Unpartitioned device written with mkfs directly (common on USB sticks). */
    if (*n == before) {
        GET_LENGTH_INFORMATION gli;
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                            &gli, sizeof(gli), &ret, NULL))
            add_part(out, max, n, h, path, TCL_SRC_DISK, disk_no, 0,
                     0, (uint64_t)gli.Length.QuadPart, sect);
    }

    CloseHandle(h);
}

int tcl_scan_all(tcl_part *out, int max, const wchar_t images[][MAX_PATH], int n_images)
{
    int n = 0, i;

    for (i = 0; i < 64 && n < max; i++)
        scan_disk(out, max, &n, i);

    for (i = 0; i < n_images && n < max; i++)
        scan_image(out, max, &n, images[i]);

    return n;
}
