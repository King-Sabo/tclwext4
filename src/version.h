/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * version.h - single source of truth for the plugin version.
 *
 * Included by both tclwext4.rc and the C sources, so the number reported in
 * Explorer's Properties tab and the one written to Total Commander's log can
 * never drift apart.
 */
#ifndef TCLWEXT4_VERSION_H_
#define TCLWEXT4_VERSION_H_

#define TCLWEXT4_VER_MAJOR 0
#define TCLWEXT4_VER_MINOR 1
#define TCLWEXT4_VER_PATCH 0
#define TCLWEXT4_VER_BUILD 0

/* RC needs the comma form for FILEVERSION / PRODUCTVERSION. */
#define TCLWEXT4_VER_RC TCLWEXT4_VER_MAJOR, TCLWEXT4_VER_MINOR, \
                        TCLWEXT4_VER_PATCH, TCLWEXT4_VER_BUILD

#define TCLWEXT4_STR2(x) #x
#define TCLWEXT4_STR(x)  TCLWEXT4_STR2(x)

#define TCLWEXT4_VER_STRING  TCLWEXT4_STR(TCLWEXT4_VER_MAJOR) "." \
                             TCLWEXT4_STR(TCLWEXT4_VER_MINOR) "." \
                             TCLWEXT4_STR(TCLWEXT4_VER_PATCH)

#define TCLWEXT4_VER_STRINGW L"" TCLWEXT4_VER_STRING

#endif /* TCLWEXT4_VERSION_H_ */
