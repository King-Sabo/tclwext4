/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tclwext4.h - internal shared declarations.
 */
#ifndef TCLWEXT4_H_
#define TCLWEXT4_H_

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#include "ext4.h"
#include "ext4_blockdev.h"
#include "ext4_types.h"
#include "ext4_errno.h"

#define TCL_MAX_VOLUMES 32
#define TCL_MAX_IMAGES  16

/* ---------------------------------------------------------------- utils */

/* Caller frees with LocalFree(). Return NULL on failure. */
wchar_t *tcl_u8_to_w(const char *s);
char    *tcl_w_to_u8(const wchar_t *s);

void tcl_logf(const wchar_t *fmt, ...);      /* -> TC log window */
void tcl_set_log(void *plugin_nr_and_proc);  /* set by FsInitW */

uint64_t tcl_filetime_from_unix(uint32_t t);
uint32_t tcl_unix_from_filetime(const FILETIME *ft);
bool     tcl_is_elevated(void);

/* ------------------------------------------------------------ blockdev */

typedef struct tcl_bdev {
    struct ext4_blockdev     bd;
    struct ext4_blockdev_iface iface;
    uint8_t                 *ph_bbuf;

    HANDLE   h;                 /* disk or image file handle */
    wchar_t  path[MAX_PATH];    /* \\.\PhysicalDrive0 or C:\img.raw */
    bool     writable;          /* handle opened with GENERIC_WRITE */
    CRITICAL_SECTION cs;
} tcl_bdev;

/*
 * Open a Win32 backing store as an lwext4 block device.
 * part_offset/part_size are byte offsets into that store (0/0 = whole store).
 * sector_size must divide part_offset. Pass 0 to auto-detect.
 */
int  tcl_bdev_open(tcl_bdev *b, const wchar_t *path, uint64_t part_offset,
                   uint64_t part_size, uint32_t sector_size, bool want_write);
void tcl_bdev_close(tcl_bdev *b);

/* Raw helper used by the scanner before any lwext4 device exists. */
bool tcl_read_at(HANDLE h, uint64_t off, void *buf, DWORD len, uint32_t sect);
uint32_t tcl_query_sector_size(HANDLE h);

/* ---------------------------------------------------------------- scan */

typedef enum {
    TCL_SRC_DISK,       /* \\.\PhysicalDriveN */
    TCL_SRC_IMAGE       /* raw disk image with MBR/GPT */
} tcl_src_kind;

typedef struct {
    wchar_t   backing[MAX_PATH];   /* \\.\PhysicalDrive1 or D:\sd.img */
    wchar_t   label[64];           /* TC directory name, filename-safe */
    wchar_t   fslabel[32];         /* ext volume name from superblock */
    tcl_src_kind kind;
    int       disk_no;             /* -1 for images */
    int       part_no;             /* 0 = whole device (dd-style) */
    uint64_t  offset;              /* bytes */
    uint64_t  size;                /* bytes */
    uint32_t  sector;              /* bytes per sector of backing store */

    uint32_t  f_compat, f_incompat, f_ro_compat;
    uint32_t  incompat_unsup, ro_unsup;
    bool      mountable;           /* false: unsupported INCOMPAT bits */
    bool      force_ro;            /* true: mount read-only */
    wchar_t   ro_reason[128];
    uint8_t   uuid[16];
    uint64_t  blocks;
    uint32_t  block_size;
} tcl_part;

/* Rescan everything. Returns number of ext partitions found. */
int  tcl_scan_all(tcl_part *out, int max, const wchar_t images[][MAX_PATH], int n_images);

/* Probe a candidate range for an ext superblock and fill feature info. */
bool tcl_probe_ext(HANDLE h, uint64_t off, uint64_t size, uint32_t sect, tcl_part *p);

/* -------------------------------------------------------------- volume */

typedef struct {
    bool      in_use;
    bool      mounted;
    tcl_part  part;
    tcl_bdev  bdev;
    char      dev_name[64];   /* lwext4 device name  */
    char      mp[64];         /* lwext4 mount point, e.g. "/disk0p2/" */
    bool      read_only;
    bool      journal_on;
    bool      wb_on;
    bool      ro_warned;   /* refusal dialog already shown */
} tcl_volume;

extern tcl_volume  g_vol[TCL_MAX_VOLUMES];
extern int         g_vol_count;
extern CRITICAL_SECTION g_ext4_cs;   /* serialises ALL lwext4 calls */
extern bool        g_global_ro;      /* ini: force read-only everywhere */

void tcl_vol_rescan(void);
void tcl_vol_unmount_all(void);
tcl_volume *tcl_vol_find(const wchar_t *name);

/*
 * Split a TC path (\Disk0p2\dir\file) into a volume and an lwext4 path
 * ("/disk0p2/dir/file"). Mounts the volume on demand.
 * Returns NULL if the first component is not a known volume.
 */
tcl_volume *tcl_vol_resolve(const wchar_t *tc_path, char *out, size_t outsz);

/*
 * Resolve every component of an lwext4 path, following symlinks (depth-capped,
 * so loops terminate). 'in' and 'out' are lwext4 paths including the mount
 * point. Returns false if a link is broken or the chain is too deep, in which
 * case 'out' holds the unresolved path and the caller should treat the entry
 * as the link itself.
 */
#define TCL_SYMLINK_MAX 8
bool tcl_realpath(tcl_volume *v, const char *in, char *out, size_t outsz);

int  tcl_vol_mount(tcl_volume *v);
void tcl_vol_unmount(tcl_volume *v);

#endif /* TCLWEXT4_H_ */
