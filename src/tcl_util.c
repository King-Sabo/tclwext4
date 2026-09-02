/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcl_util.c - small helpers shared across the plugin.
 */
#include "tclwext4.h"
#include "wfxplugin.h"
#include <stdio.h>

static tLogProcW g_log_proc = NULL;
static int       g_plugin_nr = 0;

void tcl_set_log_proc(int plugin_nr, tLogProcW proc)
{
    g_plugin_nr = plugin_nr;
    g_log_proc  = proc;
}

bool g_debug_log = false;

static void tcl_emit(const wchar_t *buf, bool to_debugger)
{
    if (to_debugger) {
        OutputDebugStringW(buf);
        OutputDebugStringW(L"\r\n");
    }
    if (g_log_proc)
        g_log_proc(g_plugin_nr, 3 /* msgtype_details */, buf);
}

/*
 * Kept in Release. Reaches Total Commander's log whenever TC gave us a LogProc,
 * and OutputDebugString in Debug builds or when debuglog=1 is set in the ini -
 * so a volume that mounts read-only can still be diagnosed in the field without
 * shipping a debug build.
 */
void tcl_logf(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    bool dbg;

#ifdef _DEBUG
    dbg = true;
#else
    dbg = g_debug_log;
#endif

    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    tcl_emit(buf, dbg);
}

#if defined(_DEBUG) || defined(TCLWEXT4_VERBOSE)
/* Per-entry chatter. The whole function is absent from a Release build. */
void tcl_dbgf(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    tcl_emit(buf, true);
}
#endif

wchar_t *tcl_u8_to_w(const char *s)
{
    int n;
    wchar_t *w;

    if (!s)
        return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    w = (wchar_t *)LocalAlloc(LPTR, (SIZE_T)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *tcl_w_to_u8(const wchar_t *s)
{
    int n;
    char *a;

    if (!s)
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    a = (char *)LocalAlloc(LPTR, (SIZE_T)n);
    if (!a)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, a, n, NULL, NULL);
    return a;
}

/* Unix epoch seconds -> FILETIME ticks */
uint64_t tcl_filetime_from_unix(uint32_t t)
{
    return ((uint64_t)t * 10000000ULL) + 116444736000000000ULL;
}

uint32_t tcl_unix_from_filetime(const FILETIME *ft)
{
    uint64_t v = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;

    if (v < 116444736000000000ULL)
        return 0;
    return (uint32_t)((v - 116444736000000000ULL) / 10000000ULL);
}

bool tcl_is_elevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD sz = sizeof(el);
    bool r = false;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sz, &sz))
            r = el.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return r;
}
