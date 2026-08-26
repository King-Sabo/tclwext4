/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tclwext4.c - Total Commander WFX plugin exposing ext2/3/4 volumes.
 *
 * Layout presented to TC:
 *
 *   \                       one directory per discovered ext volume
 *   \Disk0p2\...            filesystem contents
 *   \[rescan]               pseudo-file, F3/Enter triggers a rescan
 *   \[add image...]         pseudo-file, opens a file dialog
 *
 * All lwext4 calls are serialised through g_ext4_cs. lwext4 is not
 * thread-safe and TC calls plugin entry points from several threads.
 */
#include "tclwext4.h"
#include "wfxplugin.h"
#include "ext4_inode.h"
#include "ext4_super.h"
#include "version.h"
#include <stdio.h>
#include <commdlg.h>
#include <shellapi.h>

#define PSEUDO_RESCAN L"[rescan]"
#define PSEUDO_ADDIMG L"[add image...]"
#define PSEUDO_UNMOUNT L"[unmount all images]"
#define PSEUDO_MANAGE  L"[manage images...]"

extern wchar_t g_images[TCL_MAX_IMAGES][MAX_PATH];
extern int     g_image_count;
void tcl_set_log_proc(int plugin_nr, tLogProcW proc);

static int             g_plugin_nr = 0;
static tProgressProcW  g_progress  = NULL;
static tRequestProcW   g_request   = NULL;
static char            g_ini[MAX_PATH] = { 0 };
static bool            g_inited    = false;

/* forward declarations (mutual use between entry points) */
int    __stdcall FsInitW(int, tProgressProcW, tLogProcW, tRequestProcW);
BOOL   __stdcall FsFindNextW(HANDLE, WIN32_FIND_DATAW *);
int    __stdcall FsFindClose(HANDLE);
BOOL   __stdcall FsDeleteFileW(WCHAR *);
static void      save_images(void);

/* tcl_dialog.c */
void tcl_manage_images(HWND parent, int preselect);
void tcl_remove_all_images(void);

/* Shared with tcl_dialog.c so the dialog can show where settings live. */
wchar_t   g_ini_path_w[MAX_PATH] = { 0 };
HINSTANCE g_hinst = NULL;
void tcl_save_images(void) { save_images(); }

/* --------------------------------------------------------- find handle */

typedef enum { FH_ROOT, FH_DIR } fh_kind;

typedef struct {
    fh_kind      kind;
    int          idx;          /* root: index into g_vol, then pseudo entries */
    tcl_volume  *vol;
    ext4_dir     dir;
    char         base[512];    /* lwext4 dir path, always ends in '/' */
} find_handle;

/* ------------------------------------------------------------- helpers */

static void fill_dir_entry(WIN32_FIND_DATAW *fd, const wchar_t *name,
                           bool is_dir, uint64_t size, uint32_t mtime,
                           uint32_t atime, uint32_t ctime, bool readonly)
{
    uint64_t ft;

    ZeroMemory(fd, sizeof(*fd));
    wcsncpy_s(fd->cFileName, MAX_PATH, name, _TRUNCATE);
    fd->dwFileAttributes = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (readonly)
        fd->dwFileAttributes |= FILE_ATTRIBUTE_READONLY;
    fd->nFileSizeLow  = (DWORD)(size & 0xFFFFFFFF);
    fd->nFileSizeHigh = (DWORD)(size >> 32);

    ft = tcl_filetime_from_unix(mtime);
    fd->ftLastWriteTime.dwLowDateTime  = (DWORD)ft;
    fd->ftLastWriteTime.dwHighDateTime = (DWORD)(ft >> 32);
    ft = tcl_filetime_from_unix(atime);
    fd->ftLastAccessTime.dwLowDateTime  = (DWORD)ft;
    fd->ftLastAccessTime.dwHighDateTime = (DWORD)(ft >> 32);
    ft = tcl_filetime_from_unix(ctime);
    fd->ftCreationTime.dwLowDateTime  = (DWORD)ft;
    fd->ftCreationTime.dwHighDateTime = (DWORD)(ft >> 32);
}

/*
 * Fill a WIN32_FIND_DATAW from an lwext4 path. Caller holds g_ext4_cs.
 *
 * Symlinks are followed: the row shows the target's type, size and timestamps,
 * so a symlinked directory is enterable and a symlinked file copies its
 * contents rather than the link text. REPARSE_POINT is still set so the panel
 * can distinguish it. A broken or looping link falls back to the link itself.
 */
static bool stat_into(tcl_volume *v, const char *path, const wchar_t *name,
                      uint8_t de_type, WIN32_FIND_DATAW *fd)
{
    struct ext4_inode  ino;
    struct ext4_sblock *sb = NULL;
    uint32_t inum = 0;
    uint64_t size = 0;
    uint32_t mt = 0, at = 0, ct = 0;
    char     resolved[768];
    const char *target = path;
    bool is_dir  = (de_type == EXT4_DE_DIR);
    bool is_link = (de_type == EXT4_DE_SYMLINK);

    if (is_link && tcl_realpath(v, path, resolved, sizeof(resolved))) {
        target = resolved;
        is_dir = (ext4_inode_exist(resolved, EXT4_DE_DIR) == EOK);
    }

    if (ext4_raw_inode_fill(target, &inum, &ino) == EOK &&
        ext4_get_sblock(v->mp, &sb) == EOK) {
        size = ext4_inode_get_size(sb, &ino);
        mt   = ext4_inode_get_modif_time(&ino);
        at   = ext4_inode_get_access_time(&ino);
        ct   = ext4_inode_get_change_inode_time(&ino);
    }
    fill_dir_entry(fd, name, is_dir, size, mt, at, ct, v->read_only);
    if (is_link)
        fd->dwFileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    return true;
}

/* Tell the user why a write was refused - once per volume per session. */
static void warn_read_only(tcl_volume *v)
{
    wchar_t msg[512], dummy[4] = { 0 };

    if (!g_request || v->ro_warned)
        return;
    v->ro_warned = true;

    swprintf_s(msg, _countof(msg),
               L"%s is mounted read-only.\r\n\r\nReason: %s\r\n\r\n"
               L"%s",
               v->part.label,
               v->part.ro_reason[0] ? v->part.ro_reason : L"backing device could not be opened for writing",
               (v->part.kind == TCL_SRC_DISK && !tcl_is_elevated())
                   ? L"Total Commander is not running elevated; raw writes to a physical drive need administrator rights."
                   : L"Run e2fsck -f on the volume under Linux to clear this.");

    g_request(g_plugin_nr, RT_MsgOK, L"tclwext4", msg, dummy, _countof(dummy));
}

static void join(char *dst, size_t n, const char *base, const char *leaf)
{
    strcpy_s(dst, n, base);
    strcat_s(dst, n, leaf);
}

/* ---------------------------------------------------------------- init */

int __stdcall FsInitW(int PluginNr, tProgressProcW pProgressProc,
                      tLogProcW pLogProc, tRequestProcW pRequestProc)
{
    g_plugin_nr = PluginNr;
    g_progress  = pProgressProc;
    g_request   = pRequestProc;
    tcl_set_log_proc(PluginNr, pLogProc);

    if (!g_inited) {
        InitializeCriticalSection(&g_ext4_cs);
        g_inited = true;
    }
    tcl_logf(L"tclwext4 " TCLWEXT4_VER_STRINGW L" (%d-bit)",
             (int)(sizeof(void *) * 8));
    return 0;
}

int __stdcall FsInit(int PluginNr, tProgressProc p, tLogProc l, tRequestProc r)
{
    (void)p; (void)l; (void)r;
    g_plugin_nr = PluginNr;
    if (!g_inited) {
        InitializeCriticalSection(&g_ext4_cs);
        g_inited = true;
    }
    return 0;
}

void __stdcall FsGetDefRootName(char *DefRootName, int maxlen)
{
    strncpy_s(DefRootName, maxlen, "ext4", _TRUNCATE);
}

void __stdcall FsSetDefaultParams(FsDefaultParamStruct *dps)
{
    wchar_t *ini;
    wchar_t buf[MAX_PATH];
    int i;

    strncpy_s(g_ini, sizeof(g_ini), dps->DefaultIniName, _TRUNCATE);
    ini = tcl_u8_to_w(g_ini);
    if (!ini)
        return;
    wcsncpy_s(g_ini_path_w, MAX_PATH, ini, _TRUNCATE);
    tcl_logf(L"tclwext4: settings file is %s", g_ini_path_w);

    g_global_ro = GetPrivateProfileIntW(L"tclwext4", L"readonly", 0, ini) != 0;

    g_image_count = 0;
    for (i = 0; i < TCL_MAX_IMAGES; i++) {
        wchar_t key[32];
        swprintf_s(key, _countof(key), L"image%d", i + 1);
        if (GetPrivateProfileStringW(L"tclwext4", key, L"", buf, MAX_PATH, ini) > 0)
            wcsncpy_s(g_images[g_image_count++], MAX_PATH, buf, _TRUNCATE);
    }
    LocalFree(ini);
}

static void save_images(void)
{
    wchar_t *ini;
    int i;

    if (!g_ini[0])
        return;
    ini = tcl_u8_to_w(g_ini);
    if (!ini)
        return;
    for (i = 0; i < TCL_MAX_IMAGES; i++) {
        wchar_t key[32];
        swprintf_s(key, _countof(key), L"image%d", i + 1);
        WritePrivateProfileStringW(L"tclwext4", key,
                                   i < g_image_count ? g_images[i] : NULL, ini);
    }
    LocalFree(ini);
}

int __stdcall FsGetBackgroundFlags(void)
{
    return 0;   /* lwext4 state is process-global; no background threads */
}

/* ------------------------------------------------------------ find/list */

HANDLE __stdcall FsFindFirstW(WCHAR *Path, WIN32_FIND_DATAW *FindData)
{
    find_handle *fh;

    if (!g_inited)
        FsInitW(0, NULL, NULL, NULL);

    fh = (find_handle *)LocalAlloc(LPTR, sizeof(*fh));
    if (!fh) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    if (!Path || !Path[0] || (Path[0] == L'\\' && !Path[1])) {
        EnterCriticalSection(&g_ext4_cs);
        if (g_vol_count == 0)
            tcl_vol_rescan();
        LeaveCriticalSection(&g_ext4_cs);

        fh->kind = FH_ROOT;
        fh->idx  = 0;
        if (!FsFindNextW((HANDLE)fh, FindData)) {
            LocalFree(fh);
            SetLastError(ERROR_NO_MORE_FILES);
            return INVALID_HANDLE_VALUE;
        }
        return (HANDLE)fh;
    }

    fh->kind = FH_DIR;
    EnterCriticalSection(&g_ext4_cs);
    fh->vol = tcl_vol_resolve(Path, fh->base, sizeof(fh->base));
    if (!fh->vol) {
        LeaveCriticalSection(&g_ext4_cs);
        LocalFree(fh);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (fh->base[strlen(fh->base) - 1] != '/')
        strcat_s(fh->base, sizeof(fh->base), "/");

    if (ext4_dir_open(&fh->dir, fh->base) != EOK) {
        LeaveCriticalSection(&g_ext4_cs);
        LocalFree(fh);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_ext4_cs);

    if (!FsFindNextW((HANDLE)fh, FindData)) {
        FsFindClose((HANDLE)fh);
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)fh;
}

BOOL __stdcall FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW *FindData)
{
    find_handle *fh = (find_handle *)Hdl;
    const ext4_direntry *de;

    if (!fh || fh == INVALID_HANDLE_VALUE)
        return FALSE;

    if (fh->kind == FH_ROOT) {
        if (fh->idx < g_vol_count) {
            tcl_volume *v = &g_vol[fh->idx++];
            /* Once mounted, report what actually happened rather than the
               scan-time prediction: elevation and write-open failures are only
               known after tcl_vol_mount(). */
            bool ro = v->mounted ? v->read_only
                                 : (!v->part.mountable || v->part.force_ro || g_global_ro);
            fill_dir_entry(FindData, v->part.label, true, 0, 0, 0, 0, ro);
            return TRUE;
        }
        if (fh->idx == g_vol_count) {
            fh->idx++;
            fill_dir_entry(FindData, PSEUDO_RESCAN, false, 0, 0, 0, 0, false);
            return TRUE;
        }
        if (fh->idx == g_vol_count + 1) {
            fh->idx++;
            fill_dir_entry(FindData, PSEUDO_ADDIMG, false, 0, 0, 0, 0, false);
            return TRUE;
        }
        if (fh->idx == g_vol_count + 2) {
            fh->idx++;
            fill_dir_entry(FindData, PSEUDO_MANAGE, false, 0, 0, 0, 0, false);
            return TRUE;
        }
        if (fh->idx == g_vol_count + 3) {
            fh->idx++;
            fill_dir_entry(FindData, PSEUDO_UNMOUNT, false, 0, 0, 0, 0, false);
            return TRUE;
        }
        return FALSE;
    }

    EnterCriticalSection(&g_ext4_cs);
    for (;;) {
        char full[768];
        wchar_t *wname;

        de = ext4_dir_entry_next(&fh->dir);
        if (!de) {
            LeaveCriticalSection(&g_ext4_cs);
            return FALSE;
        }
        if (de->name_length == 1 && de->name[0] == '.')
            continue;
        if (de->name_length == 2 && de->name[0] == '.' && de->name[1] == '.')
            continue;

        {
            char nm[256];
            memcpy(nm, de->name, de->name_length);
            nm[de->name_length] = 0;
            join(full, sizeof(full), fh->base, nm);
            wname = tcl_u8_to_w(nm);
        }
        if (!wname)
            continue;

        stat_into(fh->vol, full, wname, de->inode_type, FindData);
        LocalFree(wname);
        LeaveCriticalSection(&g_ext4_cs);
        return TRUE;
    }
}

int __stdcall FsFindClose(HANDLE Hdl)
{
    find_handle *fh = (find_handle *)Hdl;

    if (!fh || fh == INVALID_HANDLE_VALUE)
        return 0;
    if (fh->kind == FH_DIR) {
        EnterCriticalSection(&g_ext4_cs);
        ext4_dir_close(&fh->dir);
        LeaveCriticalSection(&g_ext4_cs);
    }
    LocalFree(fh);
    return 0;
}

/* ------------------------------------------------------------ transfers */

int __stdcall FsGetFileW(WCHAR *RemoteName, WCHAR *LocalName, int CopyFlags,
                         RemoteInfoStruct *ri)
{
    char path[768];
    tcl_volume *v;
    ext4_file f;
    HANDLE out;
    uint64_t total, done = 0;
    int rc = FS_FILE_OK;
    BYTE *buf;
    DWORD created;

    (void)ri;
    if (!(CopyFlags & FS_COPYFLAGS_OVERWRITE) &&
        GetFileAttributesW(LocalName) != INVALID_FILE_ATTRIBUTES)
        return FS_FILE_EXISTS;
    if (CopyFlags & FS_COPYFLAGS_RESUME)
        return FS_FILE_NOTSUPPORTED;

    EnterCriticalSection(&g_ext4_cs);
    v = tcl_vol_resolve(RemoteName, path, sizeof(path));
    if (!v || ext4_fopen(&f, path, "rb") != EOK) {
        LeaveCriticalSection(&g_ext4_cs);
        return FS_FILE_NOTFOUND;
    }
    total = ext4_fsize(&f);
    LeaveCriticalSection(&g_ext4_cs);

    created = (CopyFlags & FS_COPYFLAGS_OVERWRITE) ? CREATE_ALWAYS : CREATE_NEW;
    out = CreateFileW(LocalName, GENERIC_WRITE, 0, NULL, created,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        EnterCriticalSection(&g_ext4_cs);
        ext4_fclose(&f);
        LeaveCriticalSection(&g_ext4_cs);
        return FS_FILE_WRITEERROR;
    }

    buf = (BYTE *)VirtualAlloc(NULL, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    if (!buf) {
        CloseHandle(out);
        return FS_FILE_READERROR;
    }

    while (done < total) {
        size_t got = 0;
        DWORD  put = 0;
        int    r;

        EnterCriticalSection(&g_ext4_cs);
        r = ext4_fread(&f, buf, 1 << 20, &got);
        LeaveCriticalSection(&g_ext4_cs);
        if (r != EOK) { rc = FS_FILE_READERROR; break; }
        if (!got) break;
        if (!WriteFile(out, buf, (DWORD)got, &put, NULL) || put != got) {
            rc = FS_FILE_WRITEERROR; break;
        }
        done += got;
        if (g_progress && g_progress(g_plugin_nr, RemoteName, LocalName,
                                     total ? (int)((done * 100) / total) : 100)) {
            rc = FS_FILE_USERABORT;
            break;
        }
    }

    VirtualFree(buf, 0, MEM_RELEASE);

    /* carry mtime over */
    EnterCriticalSection(&g_ext4_cs);
    {
        uint32_t mt = 0;
        if (ext4_mtime_get(path, &mt) == EOK) {
            uint64_t ft = tcl_filetime_from_unix(mt);
            FILETIME w;
            w.dwLowDateTime  = (DWORD)ft;
            w.dwHighDateTime = (DWORD)(ft >> 32);
            SetFileTime(out, NULL, NULL, &w);
        }
    }
    ext4_fclose(&f);
    LeaveCriticalSection(&g_ext4_cs);
    CloseHandle(out);

    if (rc != FS_FILE_OK)
        DeleteFileW(LocalName);
    else if (CopyFlags & FS_COPYFLAGS_MOVE)
        FsDeleteFileW(RemoteName);

    return rc;
}

int __stdcall FsPutFileW(WCHAR *LocalName, WCHAR *RemoteName, int CopyFlags)
{
    char path[768];
    tcl_volume *v;
    ext4_file f;
    HANDLE in;
    LARGE_INTEGER sz;
    uint64_t done = 0;
    int rc = FS_FILE_OK;
    BYTE *buf;
    FILETIME wt;

    if (CopyFlags & FS_COPYFLAGS_RESUME)
        return FS_FILE_NOTSUPPORTED;

    in = CreateFileW(LocalName, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE)
        return FS_FILE_NOTFOUND;
    GetFileSizeEx(in, &sz);
    GetFileTime(in, NULL, NULL, &wt);

    EnterCriticalSection(&g_ext4_cs);
    v = tcl_vol_resolve(RemoteName, path, sizeof(path));
    if (!v) {
        LeaveCriticalSection(&g_ext4_cs);
        CloseHandle(in);
        return FS_FILE_NOTFOUND;
    }
    if (v->read_only) {
        warn_read_only(v);
        LeaveCriticalSection(&g_ext4_cs);
        CloseHandle(in);
        return FS_FILE_WRITEERROR;
    }
    if (!(CopyFlags & FS_COPYFLAGS_OVERWRITE) &&
        ext4_inode_exist(path, EXT4_DE_REG_FILE) == EOK) {
        LeaveCriticalSection(&g_ext4_cs);
        CloseHandle(in);
        return FS_FILE_EXISTS;
    }
    if (ext4_fopen(&f, path, "wb") != EOK) {
        LeaveCriticalSection(&g_ext4_cs);
        CloseHandle(in);
        return FS_FILE_WRITEERROR;
    }
    if (!v->wb_on) {
        ext4_cache_write_back(v->mp, 1);
        v->wb_on = true;
    }
    LeaveCriticalSection(&g_ext4_cs);

    buf = (BYTE *)VirtualAlloc(NULL, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    if (!buf) {
        CloseHandle(in);
        return FS_FILE_WRITEERROR;
    }

    for (;;) {
        DWORD got = 0;
        size_t put = 0;
        int r;

        if (!ReadFile(in, buf, 1 << 20, &got, NULL)) { rc = FS_FILE_READERROR; break; }
        if (!got) break;

        EnterCriticalSection(&g_ext4_cs);
        r = ext4_fwrite(&f, buf, got, &put);
        LeaveCriticalSection(&g_ext4_cs);
        if (r != EOK || put != got) { rc = FS_FILE_WRITEERROR; break; }

        done += got;
        if (g_progress && g_progress(g_plugin_nr, LocalName, RemoteName,
                                     sz.QuadPart ? (int)((done * 100) / sz.QuadPart) : 100)) {
            rc = FS_FILE_USERABORT;
            break;
        }
    }

    VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(in);

    EnterCriticalSection(&g_ext4_cs);
    ext4_fclose(&f);
    ext4_mtime_set(path, tcl_unix_from_filetime(&wt));
    /* Flush per file: a crash then costs one file, not the tree. */
    if (v->wb_on) {
        ext4_cache_write_back(v->mp, 0);
        v->wb_on = false;
    }
    ext4_cache_flush(v->mp);
    if (rc != FS_FILE_OK && rc != FS_FILE_USERABORT)
        ext4_fremove(path);
    LeaveCriticalSection(&g_ext4_cs);

    if (rc == FS_FILE_OK && (CopyFlags & FS_COPYFLAGS_MOVE))
        DeleteFileW(LocalName);

    return rc;
}

/* -------------------------------------------------------- modifications */

BOOL __stdcall FsMkDirW(WCHAR *Path)
{
    char p[768];
    tcl_volume *v;
    BOOL ok = FALSE;

    EnterCriticalSection(&g_ext4_cs);
    v = tcl_vol_resolve(Path, p, sizeof(p));
    if (v && v->read_only)
        warn_read_only(v);
    else if (v)
        ok = (ext4_dir_mk(p) == EOK);
    LeaveCriticalSection(&g_ext4_cs);
    return ok;
}

BOOL __stdcall FsDeleteFileW(WCHAR *RemoteName)
{
    char p[768];
    tcl_volume *v;
    BOOL ok = FALSE;

    EnterCriticalSection(&g_ext4_cs);
    v = tcl_vol_resolve(RemoteName, p, sizeof(p));
    if (v && v->read_only) {
        warn_read_only(v);
    } else if (v) {
        ok = (ext4_fremove(p) == EOK);
        ext4_cache_flush(v->mp);
    }
    LeaveCriticalSection(&g_ext4_cs);
    return ok;
}

BOOL __stdcall FsRemoveDirW(WCHAR *RemoteName)
{
    char p[768];
    tcl_volume *v;
    BOOL ok = FALSE;

    EnterCriticalSection(&g_ext4_cs);
    v = tcl_vol_resolve(RemoteName, p, sizeof(p));
    if (v && v->read_only) {
        warn_read_only(v);
    } else if (v) {
        ok = (ext4_dir_rm(p) == EOK);
        ext4_cache_flush(v->mp);
    }
    LeaveCriticalSection(&g_ext4_cs);
    return ok;
}

int __stdcall FsRenMovFileW(WCHAR *OldName, WCHAR *NewName, BOOL Move,
                            BOOL OverWrite, RemoteInfoStruct *ri)
{
    char op[768], np[768];
    tcl_volume *vo, *vn;
    int rc = FS_FILE_WRITEERROR;

    (void)ri;
    EnterCriticalSection(&g_ext4_cs);
    vo = tcl_vol_resolve(OldName, op, sizeof(op));
    vn = tcl_vol_resolve(NewName, np, sizeof(np));
    if (!vo || !vn) {
        LeaveCriticalSection(&g_ext4_cs);
        return FS_FILE_NOTFOUND;
    }
    if (vo != vn) {
        LeaveCriticalSection(&g_ext4_cs);
        return FS_FILE_NOTSUPPORTED;   /* TC falls back to copy+delete */
    }
    if (vo->read_only) {
        warn_read_only(vo);
        LeaveCriticalSection(&g_ext4_cs);
        return FS_FILE_WRITEERROR;
    }
    if (!OverWrite && ext4_inode_exist(np, EXT4_DE_REG_FILE) == EOK) {
        LeaveCriticalSection(&g_ext4_cs);
        return FS_FILE_EXISTS;
    }
    if (!Move) {
        LeaveCriticalSection(&g_ext4_cs);
        return FS_FILE_NOTSUPPORTED;   /* pure copy: let TC do get+put */
    }
    if (OverWrite)
        ext4_fremove(np);
    if (ext4_frename(op, np) == EOK) {
        ext4_cache_flush(vo->mp);
        rc = FS_FILE_OK;
    }
    LeaveCriticalSection(&g_ext4_cs);
    return rc;
}

/*
 * TC's "Change attributes" dialog routes here.
 *
 * CreationTime is deliberately ignored. ext4's ctime is the inode CHANGE time,
 * not a creation time - it is maintained by the filesystem whenever metadata
 * changes, and POSIX has no interface to set it. (ext4 does store a real birth
 * time in the 256-byte inode's extra fields, but lwext4 exposes no accessor.)
 * Writing the user's "created" value into ctime would put a wrong number into a
 * field Linux tools read for something else entirely, so we skip it and still
 * report success as long as the fields ext4 can represent were set.
 *
 * Note that tcl_vol_resolve() follows symlinks, so timestamps land on the
 * target, matching utimes() rather than lutimes().
 */
BOOL __stdcall FsSetTimeW(WCHAR *RemoteName, FILETIME *CreationTime,
                          FILETIME *LastAccessTime, FILETIME *LastWriteTime)
{
    char p[768];
    tcl_volume *v;
    BOOL ok = FALSE;

    (void)CreationTime;

    EnterCriticalSection(&g_ext4_cs);
    v = tcl_vol_resolve(RemoteName, p, sizeof(p));
    if (v && v->read_only) {
        warn_read_only(v);
    } else if (v) {
        ok = TRUE;
        /* mtime last: lwext4 touches ctime as a side effect of these, so the
           inode ends up with a change time that reflects this operation. */
        if (LastAccessTime)
            ok &= (ext4_atime_set(p, tcl_unix_from_filetime(LastAccessTime)) == EOK);
        if (LastWriteTime)
            ok &= (ext4_mtime_set(p, tcl_unix_from_filetime(LastWriteTime)) == EOK);
        if (ok)
            ext4_cache_flush(v->mp);
    }
    LeaveCriticalSection(&g_ext4_cs);
    return ok;
}

/*
 * Only the read-only bit maps onto anything in ext4. Windows' hidden, system
 * and archive bits have no equivalent and are silently ignored rather than
 * being invented as xattrs.
 *
 * Clearing read-only restores the owner write bit only. The original group and
 * other write bits are not recoverable once cleared, so we do not guess at
 * them; use chmod under Linux if the exact mode matters.
 */
BOOL __stdcall FsSetAttrW(WCHAR *RemoteName, int NewAttr)
{
    char p[768];
    tcl_volume *v;
    uint32_t mode = 0;
    BOOL ok = FALSE;

    EnterCriticalSection(&g_ext4_cs);
    v = tcl_vol_resolve(RemoteName, p, sizeof(p));
    if (v && v->read_only) {
        warn_read_only(v);
    } else if (v && ext4_mode_get(p, &mode) == EOK) {
        if (NewAttr & FILE_ATTRIBUTE_READONLY)
            mode &= ~0222u;
        else
            mode |= 0200u;
        ok = (ext4_mode_set(p, mode) == EOK);
        if (ok)
            ext4_cache_flush(v->mp);
    }
    LeaveCriticalSection(&g_ext4_cs);
    return ok;
}

/* ------------------------------------------------------------- session */

void __stdcall FsStatusInfoW(WCHAR *RemoteDir, int InfoStartEnd, int InfoOperation)
{
    char p[768];
    tcl_volume *v;

    if (InfoOperation != FS_STATUS_OP_PUT_MULTI &&
        InfoOperation != FS_STATUS_OP_RENMOV_MULTI &&
        InfoOperation != FS_STATUS_OP_DELETE)
        return;

    EnterCriticalSection(&g_ext4_cs);
    v = tcl_vol_resolve(RemoteDir, p, sizeof(p));
    if (v && !v->read_only) {
        if (InfoStartEnd == FS_STATUS_START && !v->wb_on) {
            ext4_cache_write_back(v->mp, 1);
            v->wb_on = true;
        } else if (InfoStartEnd == FS_STATUS_END && v->wb_on) {
            ext4_cache_write_back(v->mp, 0);
            v->wb_on = false;
            ext4_cache_flush(v->mp);
        }
    }
    LeaveCriticalSection(&g_ext4_cs);
}

BOOL __stdcall FsDisconnectW(WCHAR *DisconnectRoot)
{
    (void)DisconnectRoot;
    EnterCriticalSection(&g_ext4_cs);
    tcl_vol_unmount_all();
    LeaveCriticalSection(&g_ext4_cs);
    return TRUE;
}

/*
 * TC gives WFX plugins no way to add custom context-menu entries, so the
 * per-volume actions hang off the "properties" verb (Alt+Enter / right-click >
 * Properties) and the global ones off pseudo-files in the root listing.
 */
static int volume_properties(HWND MainWin, tcl_volume *v)
{
    wchar_t msg[1024], answer[8] = { 0 };
    wchar_t size[64];
    bool removable_image = (v->part.kind == TCL_SRC_IMAGE);

    swprintf_s(size, _countof(size), L"%.1f GiB",
               (double)v->part.size / (1024.0 * 1024.0 * 1024.0));

    swprintf_s(msg, _countof(msg),
        L"%s\r\n\r\n"
        L"Type:     %s\r\n"
        L"Backing:  %s\r\n"
        L"Offset:   %llu\r\n"
        L"Size:     %s\r\n"
        L"Label:    %s\r\n"
        L"State:    %s\r\n"
        L"%s%s\r\n"
        L"Features: c:%08X i:%08X r:%08X\r\n\r\n"
        L"%s",
        v->part.label,
        removable_image ? L"disk image" : L"physical partition",
        v->part.backing,
        (unsigned long long)v->part.offset,
        size,
        v->part.fslabel[0] ? v->part.fslabel : L"(none)",
        !v->part.mountable ? L"unsupported"
                           : (v->mounted ? (v->read_only ? L"mounted read-only"
                                                         : L"mounted read-write")
                                         : L"not mounted"),
        v->part.ro_reason[0] ? L"Reason:   " : L"",
        v->part.ro_reason[0] ? v->part.ro_reason : L"",
        v->part.f_compat, v->part.f_incompat, v->part.f_ro_compat,
        removable_image ? L"Choose Remove in the next dialog to forget this image."
                        : (v->mounted ? L"Unmount this volume now?"
                                      : L"(not mounted - nothing to unmount)"));

    if (!g_request)
        return FS_EXEC_OK;

    /* For an image, jump straight into the manage dialog with it selected, so
       "remove from list" is a button rather than a yes/no guess. */
    if (removable_image) {
        int i, sel = -1;
        for (i = 0; i < g_image_count; i++)
            if (_wcsicmp(g_images[i], v->part.backing) == 0) { sel = i; break; }
        g_request(g_plugin_nr, RT_MsgOK, L"tclwext4", msg, answer, _countof(answer));
        tcl_manage_images(MainWin, sel);
        return FS_EXEC_OK;
    }

    if (!v->mounted) {
        g_request(g_plugin_nr, RT_MsgOK, L"tclwext4", msg, answer, _countof(answer));
        return FS_EXEC_OK;
    }

    if (!g_request(g_plugin_nr, RT_MsgYesNo, L"tclwext4", msg, answer, _countof(answer)))
        return FS_EXEC_OK;

    EnterCriticalSection(&g_ext4_cs);
    tcl_vol_unmount(v);
    LeaveCriticalSection(&g_ext4_cs);
    return FS_EXEC_OK;
}

int __stdcall FsExecuteFileW(HWND MainWin, WCHAR *RemoteName, WCHAR *Verb)
{
    const wchar_t *leaf = wcsrchr(RemoteName, L'\\');
    leaf = leaf ? leaf + 1 : RemoteName;

    /* Alt+Enter on a volume: details, plus unmount / remove. */
    if (_wcsnicmp(Verb, L"properties", 10) == 0) {
        tcl_volume *v;
        EnterCriticalSection(&g_ext4_cs);
        v = tcl_vol_find(leaf);
        LeaveCriticalSection(&g_ext4_cs);
        if (v)
            return volume_properties(MainWin, v);
        return FS_EXEC_YOURSELF;
    }

    if (_wcsicmp(Verb, L"open") != 0)
        return FS_EXEC_YOURSELF;

    if (_wcsicmp(leaf, PSEUDO_RESCAN) == 0) {
        EnterCriticalSection(&g_ext4_cs);
        tcl_vol_rescan();
        LeaveCriticalSection(&g_ext4_cs);
        return FS_EXEC_OK;
    }

    if (_wcsicmp(leaf, PSEUDO_MANAGE) == 0) {
        tcl_manage_images(MainWin, -1);
        return FS_EXEC_OK;
    }

    /*
     * Images only. Physical partitions are not "removable" in any sense the
     * user means here - unmounting one just means it gets mounted again on the
     * next access, which looked like the command doing nothing at all.
     */
    if (_wcsicmp(leaf, PSEUDO_UNMOUNT) == 0) {
        wchar_t msg[256], answer[8] = { 0 };

        if (g_image_count == 0) {
            if (g_request)
                g_request(g_plugin_nr, RT_MsgOK, L"tclwext4",
                          L"No disk images are in the list.", answer, _countof(answer));
            return FS_EXEC_OK;
        }
        swprintf_s(msg, _countof(msg),
                   L"Unmount and forget all %d remembered disk image(s)?\r\n\r\n"
                   L"Physical partitions are not affected.", g_image_count);
        if (g_request &&
            !g_request(g_plugin_nr, RT_MsgYesNo, L"tclwext4", msg, answer, _countof(answer)))
            return FS_EXEC_OK;

        tcl_remove_all_images();
        tcl_logf(L"tclwext4: all disk images unmounted and removed from the list");
        return FS_EXEC_OK;
    }

    if (_wcsicmp(leaf, PSEUDO_ADDIMG) == 0) {
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH] = { 0 };

        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = MainWin;
        ofn.lpstrFilter = L"Disk images\0*.img;*.raw;*.bin;*.dd\0All files\0*.*\0\0";
        ofn.lpstrFile   = file;
        ofn.nMaxFile    = MAX_PATH;
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn) && g_image_count < TCL_MAX_IMAGES) {
            wcsncpy_s(g_images[g_image_count++], MAX_PATH, file, _TRUNCATE);
            save_images();
            EnterCriticalSection(&g_ext4_cs);
            tcl_vol_rescan();
            LeaveCriticalSection(&g_ext4_cs);
        }
        return FS_EXEC_OK;
    }

    return FS_EXEC_YOURSELF;
}

/*
 * Images get the shell icon for their file type, physical partitions keep the
 * default folder icon - so the two are distinguishable at a glance without
 * shipping an icon resource or relying on shell32 icon indices, which shift
 * between Windows versions.
 *
 * SHGFI_USEFILEATTRIBUTES keeps this off the disk: the icon comes from the
 * extension alone, so it is safe to do on TC's drawing thread.
 */
int __stdcall FsExtractCustomIconW(WCHAR *RemoteName, int ExtractFlags, HICON *TheIcon)
{
    wchar_t name[MAX_PATH];
    wchar_t *end;
    tcl_volume *v;
    SHFILEINFOW sfi;
    const wchar_t *leaf;

    wcsncpy_s(name, MAX_PATH, RemoteName, _TRUNCATE);
    end = name + wcslen(name);
    while (end > name && end[-1] == L'\\')
        *--end = 0;

    leaf = wcsrchr(name, L'\\');
    leaf = leaf ? leaf + 1 : name;
    if (!*leaf)
        return FS_ICON_USEDEFAULT;

    EnterCriticalSection(&g_ext4_cs);
    v = tcl_vol_find(leaf);
    LeaveCriticalSection(&g_ext4_cs);

    if (!v || v->part.kind != TCL_SRC_IMAGE)
        return FS_ICON_USEDEFAULT;

    ZeroMemory(&sfi, sizeof(sfi));
    if (!SHGetFileInfoW(v->part.backing, FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                        SHGFI_ICON | SHGFI_USEFILEATTRIBUTES |
                        ((ExtractFlags & FS_ICONFLAG_SMALL) ? SHGFI_SMALLICON
                                                            : SHGFI_LARGEICON)))
        return FS_ICON_USEDEFAULT;

    *TheIcon = sfi.hIcon;
    return FS_ICON_EXTRACTED_DESTROY;
}

/* ------------------------------------------------ ANSI compat wrappers */

static wchar_t *A2W(const char *s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)LocalAlloc(LPTR, (SIZE_T)n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

static void W2A(const wchar_t *w, char *out, int max)
{
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, max, NULL, NULL);
}

static void fd_w2a(const WIN32_FIND_DATAW *w, WIN32_FIND_DATAA *a)
{
    ZeroMemory(a, sizeof(*a));
    a->dwFileAttributes = w->dwFileAttributes;
    a->ftCreationTime   = w->ftCreationTime;
    a->ftLastAccessTime = w->ftLastAccessTime;
    a->ftLastWriteTime  = w->ftLastWriteTime;
    a->nFileSizeHigh    = w->nFileSizeHigh;
    a->nFileSizeLow     = w->nFileSizeLow;
    W2A(w->cFileName, a->cFileName, MAX_PATH);
}

HANDLE __stdcall FsFindFirst(char *Path, WIN32_FIND_DATAA *FindData)
{
    WIN32_FIND_DATAW fw;
    wchar_t *wp = A2W(Path);
    HANDLE h;

    if (!wp) return INVALID_HANDLE_VALUE;
    h = FsFindFirstW(wp, &fw);
    LocalFree(wp);
    if (h != INVALID_HANDLE_VALUE)
        fd_w2a(&fw, FindData);
    return h;
}

BOOL __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA *FindData)
{
    WIN32_FIND_DATAW fw;
    if (!FsFindNextW(Hdl, &fw))
        return FALSE;
    fd_w2a(&fw, FindData);
    return TRUE;
}

/* ---------------------------------------------------------------- entry */

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        g_hinst = h;
    if (reason == DLL_PROCESS_DETACH && g_inited) {
        tcl_vol_unmount_all();
        ext4_device_unregister_all();
    }
    return TRUE;
}
