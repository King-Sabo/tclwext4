/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_dialog.c - "Manage images" dialog.
 *
 * Total Commander offers WFX plugins no way to add custom entries to the panel
 * context menu. The obvious-looking hook, mapping removal onto Delete, is a
 * trap: TC deletes a directory by recursing through the plugin and deleting its
 * *contents* first, then calling FsRemoveDir. Wiring "remove image from list"
 * to that would destroy the filesystem before the plugin was ever told what was
 * being asked. So removal lives in a dialog instead, reachable from the root
 * listing and from a volume's properties.
 *
 * The template is built in memory rather than as an .rc resource so the plugin
 * stays a handful of .c files with no resource compiler step.
 */
#include "tclwext4.h"
#include "wfxplugin.h"
#include <windowsx.h>
#include <stdio.h>
#include "version.h"

#define ID_LIST      1001
#define ID_REMOVE    1002
#define ID_REMOVEALL 1003
#define ID_INIPATH   1004

extern wchar_t g_images[TCL_MAX_IMAGES][MAX_PATH];
extern int     g_image_count;
extern wchar_t   g_ini_path_w[MAX_PATH];
extern HINSTANCE g_hinst;

void tcl_save_images(void);

/* ------------------------------------------------- template assembly */

typedef struct {
    BYTE  *buf;
    size_t used;
} tmpl;

static void t_align(tmpl *t)
{
    while (t->used & 3)
        t->buf[t->used++] = 0;
}

static void t_word(tmpl *t, WORD w)
{
    memcpy(t->buf + t->used, &w, sizeof(w));
    t->used += sizeof(w);
}

static void t_dword(tmpl *t, DWORD d)
{
    memcpy(t->buf + t->used, &d, sizeof(d));
    t->used += sizeof(d);
}

static void t_str(tmpl *t, const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    memcpy(t->buf + t->used, s, n);
    t->used += n;
}

static void t_item(tmpl *t, DWORD style, short x, short y, short cx, short cy,
                   WORD id, WORD cls, const wchar_t *text)
{
    t_align(t);
    t_dword(t, style);
    t_dword(t, 0);              /* exstyle */
    t_word(t, (WORD)x); t_word(t, (WORD)y);
    t_word(t, (WORD)cx); t_word(t, (WORD)cy);
    t_word(t, id);
    t_word(t, 0xFFFF); t_word(t, cls);
    t_str(t, text);
    t_word(t, 0);               /* no creation data */
}

/* ------------------------------------------------------- dialog proc */

static void fill_list(HWND dlg)
{
    HWND lb = GetDlgItem(dlg, ID_LIST);
    int i;

    ListBox_ResetContent(lb);
    for (i = 0; i < g_image_count; i++)
        ListBox_AddString(lb, g_images[i]);

    EnableWindow(GetDlgItem(dlg, ID_REMOVE), g_image_count > 0);
    EnableWindow(GetDlgItem(dlg, ID_REMOVEALL), g_image_count > 0);
}

/* Unmount every volume backed by this image, then drop it from the list. */
static void remove_image(int idx)
{
    int i;

    if (idx < 0 || idx >= g_image_count)
        return;

    EnterCriticalSection(&g_ext4_cs);
    for (i = 0; i < g_vol_count; i++) {
        if (g_vol[i].in_use && g_vol[i].part.kind == TCL_SRC_IMAGE &&
            _wcsicmp(g_vol[i].part.backing, g_images[idx]) == 0)
            tcl_vol_unmount(&g_vol[i]);
    }
    LeaveCriticalSection(&g_ext4_cs);

    for (i = idx; i < g_image_count - 1; i++)
        wcscpy_s(g_images[i], MAX_PATH, g_images[i + 1]);
    g_image_count--;
    g_images[g_image_count][0] = 0;
}

void tcl_remove_all_images(void)
{
    while (g_image_count > 0)
        remove_image(g_image_count - 1);
    tcl_save_images();

    EnterCriticalSection(&g_ext4_cs);
    tcl_vol_rescan();
    LeaveCriticalSection(&g_ext4_cs);
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        wchar_t ini[MAX_PATH + 32];
        swprintf_s(ini, _countof(ini), L"Settings: %s",
                   g_ini_path_w[0] ? g_ini_path_w : L"(not set by Total Commander)");
        SetDlgItemTextW(dlg, ID_INIPATH, ini);
        fill_list(dlg);
        if (lp)
            ListBox_SetCurSel(GetDlgItem(dlg, ID_LIST), (int)lp);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REMOVE: {
            int sel = ListBox_GetCurSel(GetDlgItem(dlg, ID_LIST));
            if (sel >= 0) {
                remove_image(sel);
                tcl_save_images();
                fill_list(dlg);
            }
            return TRUE;
        }
        case ID_REMOVEALL:
            while (g_image_count > 0)
                remove_image(g_image_count - 1);
            tcl_save_images();
            fill_list(dlg);
            return TRUE;

        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, 0);
        return TRUE;
    }
    return FALSE;
}

/* preselect < 0 for none */
void tcl_manage_images(HWND parent, int preselect)
{
    BYTE  raw[2048];
    tmpl  t = { raw, 0 };

    ZeroMemory(raw, sizeof(raw));

    /* DLGTEMPLATE header */
    t_dword(&t, DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP |
                WS_CAPTION | WS_SYSMENU);
    t_dword(&t, 0);             /* exstyle */
    t_word(&t, 5);              /* item count */
    t_word(&t, 0); t_word(&t, 0);
    t_word(&t, 340); t_word(&t, 170);
    t_word(&t, 0);              /* no menu */
    t_word(&t, 0);              /* default class */
    t_str(&t, L"tclwext4 " TCLWEXT4_VER_STRINGW L" - remembered disk images");
    t_word(&t, 8);
    t_str(&t, L"Segoe UI");

    t_item(&t, WS_CHILD | WS_VISIBLE, 7, 7, 326, 10, ID_INIPATH, 0x0082, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP |
               LBS_NOTIFY | LBS_HASSTRINGS, 7, 22, 326, 110, ID_LIST, 0x0083, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
           7, 140, 90, 16, ID_REMOVE, 0x0080, L"&Remove selected");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
           103, 140, 90, 16, ID_REMOVEALL, 0x0080, L"Remove &all");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
           253, 140, 80, 16, IDCANCEL, 0x0080, L"&Close");

    DialogBoxIndirectParamW(g_hinst, (LPCDLGTEMPLATEW)raw,
                            parent, dlg_proc, (LPARAM)preselect);

    EnterCriticalSection(&g_ext4_cs);
    tcl_vol_rescan();
    LeaveCriticalSection(&g_ext4_cs);
}
