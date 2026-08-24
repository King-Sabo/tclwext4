/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wfxplugin.h - Total Commander WFX file system plugin interface.
 * Trimmed to what tclwext4 uses; matches Ghisler's SDK layout (interface 2.0).
 */
#ifndef WFXPLUGIN_H_
#define WFXPLUGIN_H_

#include <windows.h>

/* FsGetFile / FsPutFile / FsRenMovFile return codes */
#define FS_FILE_OK                    0
#define FS_FILE_EXISTS                1
#define FS_FILE_NOTFOUND              2
#define FS_FILE_READERROR             3
#define FS_FILE_WRITEERROR            4
#define FS_FILE_USERABORT             5
#define FS_FILE_NOTSUPPORTED          6
#define FS_FILE_EXISTSRESUMEALLOWED   7

/* CopyFlags */
#define FS_COPYFLAGS_OVERWRITE              1
#define FS_COPYFLAGS_RESUME                 2
#define FS_COPYFLAGS_MOVE                   4
#define FS_COPYFLAGS_EXISTS_SAMECASE        8
#define FS_COPYFLAGS_EXISTS_DIFFERENTCASE  16

/* FsExecuteFile */
#define FS_EXEC_OK        0
#define FS_EXEC_ERROR     1
#define FS_EXEC_YOURSELF  (-1)
#define FS_EXEC_SYMLINK   (-2)

/* FsStatusInfo InfoStartEnd */
#define FS_STATUS_START 0
#define FS_STATUS_END   1

/* FsStatusInfo InfoOperation */
#define FS_STATUS_OP_LIST            1
#define FS_STATUS_OP_GET_SINGLE      2
#define FS_STATUS_OP_GET_MULTI       3
#define FS_STATUS_OP_PUT_SINGLE      4
#define FS_STATUS_OP_PUT_MULTI       5
#define FS_STATUS_OP_RENMOV_SINGLE   6
#define FS_STATUS_OP_RENMOV_MULTI    7
#define FS_STATUS_OP_DELETE          8
#define FS_STATUS_OP_ATTRIB          9
#define FS_STATUS_OP_MKDIR          10
#define FS_STATUS_OP_EXEC           11
#define FS_STATUS_OP_CALCSIZE       12

/* Background transfer flags */
#define BG_DOWNLOAD 1
#define BG_UPLOAD   2
#define BG_ASK_USER 4

/* RequestProc request types */
#define RT_Other          0
#define RT_UserName       1
#define RT_Password       2
#define RT_Account        3
#define RT_MsgOK          8
#define RT_MsgYesNo       9
#define RT_MsgOKCancel   10

/* Icon extraction */
#define FS_ICONFLAG_SMALL      1
#define FS_ICONFLAG_BACKGROUND 2
#define FS_ICON_USEDEFAULT     0
#define FS_ICON_EXTRACTED      1
#define FS_ICON_EXTRACTED_DESTROY 2
#define FS_ICON_DELAYED        3

/* ---- content plugin interface (custom columns) ---- */
#define ft_nomorefields      0
#define ft_numeric_32        1
#define ft_numeric_64        2
#define ft_numeric_floating  3
#define ft_date              4
#define ft_time              5
#define ft_boolean           6
#define ft_multiplechoice    7
#define ft_string            8
#define ft_fulltext          9
#define ft_datetime         10
#define ft_stringw          11

/* FsContentGetValue return codes */
#define ft_nosuchfield     (-1)
#define ft_fileerror       (-2)
#define ft_fieldempty      (-3)
#define ft_ondemand        (-4)
#define ft_notsupported    (-5)
#define ft_setcancel       (-6)
#define ft_delayed           0

/* FsContentGetValue flags */
#define CONTENT_DELAYIFSLOW  1
#define CONTENT_PASSTHROUGH  2

typedef struct {
    DWORD SizeLow, SizeHigh;
    FILETIME LastWriteTime;
    int Attr;
} RemoteInfoStruct;

typedef struct {
    int   size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char  DefaultIniName[MAX_PATH];
} FsDefaultParamStruct;

typedef int  (__stdcall *tProgressProc)(int PluginNr, char* SourceName, char* TargetName, int PercentDone);
typedef void (__stdcall *tLogProc)(int PluginNr, int MsgType, char* LogString);
typedef BOOL (__stdcall *tRequestProc)(int PluginNr, int RequestType, char* CustomTitle, char* CustomText, char* ReturnedText, int maxlen);

typedef int  (__stdcall *tProgressProcW)(int PluginNr, WCHAR* SourceName, WCHAR* TargetName, int PercentDone);
typedef void (__stdcall *tLogProcW)(int PluginNr, int MsgType, WCHAR* LogString);
typedef BOOL (__stdcall *tRequestProcW)(int PluginNr, int RequestType, WCHAR* CustomTitle, WCHAR* CustomText, WCHAR* ReturnedText, int maxlen);

#endif /* WFXPLUGIN_H_ */
