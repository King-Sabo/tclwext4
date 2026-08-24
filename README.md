# tclwext4

Total Commander WFX file system plugin giving read/write access to ext2/ext3/ext4
volumes from Windows, backed by an unmodified [lwext4](https://github.com/gkostka/lwext4).

---

# ⚠️ USE AT YOUR OWN RISK ⚠️

**This plugin writes to ext2/3/4 filesystems using an independent, partial
reimplementation of ext4 — not the Linux kernel driver. Using it can cause
unrecoverable data loss.**

**Do not point it at a filesystem whose contents you are not prepared to lose.
Work on a copy or an image. Run `e2fsck -f` under Linux after every write
session.**

---

## Why the risk is real

The read-only gate in this plugin blocks filesystems that *declare* features
lwext4 does not implement. That is necessary but not sufficient — it says
nothing about bugs in the features lwext4 does claim to support, and several of
those are load-bearing metadata paths.

**lwext4 is a subset implementation.** It targets embedded use and supports a
deliberately narrow feature set (see `EXT4_SUPPORTED_FINCOM` /
`EXT4_SUPPORTED_FRO_COM` in `ext4_types.h`). The kernel driver has two decades
of production exposure and a dedicated fsck; lwext4's write paths — extent tree
splitting, block group descriptor accounting, htree directory index updates —
are far less exercised. A wrong free-block count or a mis-split extent node is
not detected at write time; you find out later.

**Checksums make partial writes worse, not better.** `metadata_csum` is inside
lwext4's supported set, so this plugin will happily mount and write such a
filesystem. If lwext4 updates a structure but computes or places its checksum
differently from the kernel in some corner, the kernel later rejects that
metadata outright rather than tolerating it. Checksums turn a subtle
inconsistency into a hard mount failure.

**Journalling here is not the kernel's journalling.** lwext4 implements a jbd2
subset. This plugin calls `ext4_recover()` and `ext4_journal_start()`, and
refuses write access when `INCOMPAT_RECOVER` is set, but a crash, BSOD or a
yanked USB stick mid-transaction can still leave a state whose replay semantics
differ from what the kernel expects. The window is widened by write-back
caching, which is enabled for bulk copies (see `FsStatusInfoW`).

**Nothing here validates the filesystem first.** There is no fsck. A filesystem
that is already subtly damaged will be written to anyway, compounding the
damage.

**Concurrent access is unprotected.** MMP is not honoured — the plugin forces
read-only if the MMP feature bit is set, but MMP is rarely enabled. If the same
device is simultaneously visible to a VM, WSL2, or a hypervisor doing block
passthrough, two writers will destroy the filesystem and neither will notice.

**Raw disk writes bypass Windows' safety nets.** Writes go to
`\\.\PhysicalDriveN` at sector level. Windows does not arbitrate these against
its own volume cache for any other partition on the same disk, and removable
media can be pulled at any moment.

**This plugin is new and lightly tested.** The scanner, the block device shim,
the path resolution and the symlink following are all recent code.

### Reducing the risk

- Prefer image files (`[add image...]`) over live devices; a corrupted image is
  a deleted file, not a lost disk.
- Keep `readonly=1` in the ini unless you specifically need to write.
- Unmount cleanly: use `FsDisconnect` (TC's disconnect) rather than closing TC
  or pulling the stick, so caches flush and the journal stops.
- Run `e2fsck -f` on the volume after writing, before trusting it.
- Never write to a filesystem that is mounted anywhere else at the same time.

---


## No binary releases

**None are published, and none will be. Build it yourself.**

The reason is the one above: this plugin writes raw sectors through a partial
reimplementation of ext4, with no fsck to catch the results. A downloadable
`.wfx` is something a person drops into their plugins directory and points at a
live disk without reading a word of this file. Requiring a Visual Studio build
is a crude filter, but it selects for people who have at least seen the risks
before their filesystem is at stake.

There is a second, smaller reason: an unsigned DLL that writes to
`\\.\PhysicalDriveN` is exactly the shape of thing you should refuse to run
from a stranger. Building from source you have read is the honest answer to
that, and no code-signing certificate changes it.

### CI artifacts are not releases

`.github/workflows/build.yml` builds both bitnesses on every push and pull
request and attaches the results as workflow artifacts. Those exist so a
contributor can confirm a change compiles. They expire after seven days, need a
GitHub login to download, are unsigned, and are named
`unsigned-ci-build-...` so nobody mistakes one for something to install. Treat
them as build logs that happen to contain a DLL.

The workflow also runs a `submodule bootstrap` job that checks out *without*
submodules on purpose, to prove the `FetchLwext4` target in `lwext4.vcxproj`
still works. That path once produced an empty static library rather than an
error, so it gets its own job instead of being assumed correct.

**Licensing is not the reason.** The GPL places no obstacle in the way of binary
releases — publishing the source is already distribution, and attaching a binary
to a release next to complete corresponding source satisfies it cleanly. Anyone
forking this and choosing to ship binaries is free to do so under GPLv2 terms.
One trap if you do: GitHub's auto-generated "Source code (zip)" on a release
**omits submodule contents**, so `external/lwext4` arrives empty and the archive
is not complete corresponding source. Attach an archive built with
`git-archive-all` or an equivalent instead.

## Build

`git submodule update --init --recursive` runs automatically as a pre-build step
if `external/lwext4` is empty, so a fresh clone builds without setup. It needs
`git` on PATH and a real git working copy; for a plain source download, clone
lwext4 into `external/lwext4` by hand.

Open `tclwext4.sln` in Visual Studio 2022 and build Release for x64 and Win32,
or from a developer prompt:

```
msbuild tclwext4.sln /p:Configuration=Release /p:Platform=x64
msbuild tclwext4.sln /p:Configuration=Release /p:Platform=Win32
```

CMake is kept as an alternative (same defines, same outputs):

```
cmake --preset vs2022-x64 && cmake --build --preset x64-release
cmake --preset vs2022-x86 && cmake --build --preset x86-release
```

Produces `tclwext4.wfx64` and `tclwext4.wfx`. Put both in one directory and
point Total Commander at either; TC picks the matching bitness itself.

Both build systems compile `external/lwext4/src/*.c` into a separate static lib
target rather than using lwext4's own build files. The submodule is never
patched.

### lwext4 configuration

All of lwext4's build configuration lives in **`config/generated/ext4_config.h`**.
lwext4's own `include/ext4_config.h` begins with

```c
#if !CONFIG_USE_DEFAULT_CFG
#include "generated/ext4_config.h"
#endif
```

which is its documented hook for an external configuration. `config/` is on the
include path of both projects, `CONFIG_USE_DEFAULT_CFG` is left at 0, and
lwext4's `include/` has no `generated/` subdirectory of its own, so the include
resolves to ours unambiguously. Everything the header does not define keeps
upstream's default, since each knob in `ext4_config.h` is `#ifndef`-guarded.

Put configuration changes in that header, never in per-project preprocessor
definitions. The header is reached from the plugin sources too (via `ext4.h`),
so a single definition site is what guarantees the static library and the DLL
agree on struct layouts — a `CONFIG_EXT4_MAX_MP_NAME` that differed between them
would change `struct ext4_mountpoint` silently, with no link error and memory
corruption at runtime.

`lwext4.vcxproj` lists the lwext4 sources explicitly rather than globbing them.
MSBuild expands wildcards during project *evaluation*, before any target runs, so
on a fresh clone a glob would resolve to nothing and the pre-build fetch would
land too late to affect that same build. Re-sync the list against
`external/lwext4/src/*.c` on a submodule bump. The `CONFIG_*` list is duplicated in both `.vcxproj` files and must stay
identical: these headers are shared, and a mismatch in e.g.
`CONFIG_EXT4_MAX_MP_NAME` silently changes struct layouts between the two.

## What gets found

`tcl_scan_all()` walks `\\.\PhysicalDrive0..63`. Partitions come from
`IOCTL_DISK_GET_DRIVE_LAYOUT_EX`, so Windows does MBR (including the EBR chain)
and GPT parsing. Partition **type codes are ignored** — every partition is
probed for an ext superblock, and so is the whole device when there is no
partition table (mkfs straight onto a USB stick).

Raw disk images added through `[add image...]` are parsed here instead: GPT
first, then MBR plus logical partitions, and if neither is present the whole
file is treated as one dd-style partition image.

## Read-only policy

The plugin drops a volume to read-only, rather than risking corruption, when
any of these hold:

| Condition | Why |
|---|---|
| `ro_compat` bits outside `EXT4_SUPPORTED_FRO_COM` | lwext4 would ignore semantics it does not implement |
| `INCOMPAT_RECOVER` set | lwext4 lists this as *ignored* and will mount over an unreplayed journal |
| `INCOMPAT_MMP` set | multi-mount protection cannot be honoured |
| `s_state` not `EXT4_VALID_FS` | filesystem was not cleanly unmounted |
| physical drive, process not elevated | raw disk writes need admin |
| `readonly=1` in the ini | user override |

`incompat` bits outside `EXT4_SUPPORTED_FINCOM` are not read-only-able —
those volumes are listed but refuse to mount at all.

**Note on `orphan_file`:** `mkfs.ext4` from e2fsprogs 1.47 enables `orphan_file`,
but that is `COMPAT_ORPHAN_FILE` — a *compat* bit, so it does not affect this gate
and such filesystems mount read-write fine. The separate `RO_COMPAT_ORPHAN_PRESENT`
bit is set by the kernel only while the fs is writeably mounted and cleared on clean
unmount, so seeing it means the volume was not cleanly unmounted and the orphan file
may hold inodes still pending deletion or truncation. Do **not** clear it to force
read-write; run `e2fsck -f` on Linux, which processes the orphans and clears the bit.

## Custom columns

The plugin also implements TC's content interface, so volume state is visible in
the panel rather than only in the log. In Total Commander: *Configuration >
Options > Display > Custom columns*, or Ctrl+F1/F2 on a custom view, then pick
fields from the `tclwext4` plugin.

| Field | Notes |
|---|---|
| Status | `rw`, `ro`, `unsupported: incompat 0x…`, or `… (not mounted)` |
| Read-only why | superblock reason, or `not elevated` / `device opened read-only` |
| Volume label | `s_volume_name` from the superblock |
| UUID | `s_uuid` |
| Block size | filesystem block size |
| Size | partition size in bytes |
| Free | requires a mount; returns `ft_delayed` under `CONTENT_DELAYIFSLOW` |
| Features | raw `compat` / `incompat` / `ro_compat` words |
| Backing store | `\\.\PhysicalDriveN` or image path |

Fields are populated for the root-level volume entries only; rows inside a
volume report empty.

## Volumes: unmounting, identifying, inspecting

Total Commander offers WFX plugins no API for custom context-menu items or
tooltips, so these hang off the hooks that do exist:

**Managing images.** Images are remembered in the ini between sessions, so
"forget this image" is what you usually want rather than a bare unmount — an
unmounted image is simply mounted again on next access.

- `[manage images...]` opens a dialog listing every remembered image, with
  **Remove selected** and **Remove all** buttons. It also shows where the
  settings file lives.
- `[unmount all images]` unmounts and forgets all of them after a confirmation.
  Physical partitions are never touched by either.
- Alt+Enter (or right-click > Properties) on an image shows its details and then
  opens the manage dialog with that image preselected.
- Alt+Enter on a physical partition offers a plain unmount.

Removal is deliberately **not** wired to the Delete key or the context menu's
Delete entry. TC deletes a directory by recursing through the plugin and
deleting its *contents* first, then calling `FsRemoveDir` — so mapping that to
"remove image from list" would erase the filesystem before the plugin ever heard
about it.

**Telling images from partitions.** Images show the shell icon for their file
type; physical partitions keep the default folder icon. This avoids shipping an
icon resource and avoids `shell32.dll` icon indices, which move between Windows
versions. The `Type` custom column says `image` or `partition` outright.

**Seeing an image's path.** There is no tooltip API. The `Backing store` custom
column carries the full path, and TC can show content-plugin fields in its
custom file tips as well as in columns — configure it there if you want it on
hover. The properties dialog also shows it.

## Ini

Total Commander tells the plugin where to store its settings through
`FsSetDefaultParams`; the plugin does not choose the location. In a default
install that is `wincmd.ini`'s directory — typically
`%APPDATA%\GHISLER\` — but a portable install or a `wincmd.ini` moved via the
`-i` switch or the `CMDINI` environment variable puts it elsewhere.

To see the actual path: it is written to TC's log at startup, and shown at the
top of the `[manage images...]` dialog.

Settings live under `[tclwext4]`:

```
readonly=0
image1=D:\sd.img
image2=D:\rootfs.ext4
```

## Symlinks

Symlinks are followed. `tcl_realpath()` resolves every path component, not just
the last, so a directory reached through a symlinked parent is enterable.
Resolution is depth-capped at `TCL_SYMLINK_MAX` (8), so loops terminate; a
broken or looping link falls back to the unresolved path, which surfaces as a
normal "not found" from lwext4 rather than a silent wrong target. Rows still
carry `FILE_ATTRIBUTE_REPARSE_POINT` so the panel can tell them apart.

Absolute targets are re-rooted at the volume's mount point, so `/usr/lib` inside
the volume means the volume's own `/usr/lib`, never a host path. Targets that
point outside the filesystem cannot be represented and resolve to nothing.

## Attributes and timestamps

TC's *Files > Change attributes* works, with two mappings that are lossy by
nature:

- **Creation time is ignored.** ext4's `ctime` is the inode *change* time, not a
  creation time, and POSIX offers no way to set it. ext4 does store a real birth
  time in the 256-byte inode's extra fields, but lwext4 exposes no accessor for
  it. Setting access and modification time works.
- **Only the read-only bit maps to ext4.** It clears or restores write bits in
  the mode. Hidden, system and archive have no equivalent and are ignored rather
  than being invented as extended attributes. Clearing read-only restores the
  owner write bit only — the original group and other bits are gone once
  cleared, so use `chmod` under Linux if the exact mode matters.

Timestamps follow symlinks (`utimes()` semantics, not `lutimes()`), because path
resolution follows them everywhere.

## Known limits

- Cross-volume rename returns `FS_FILE_NOTSUPPORTED`; TC falls back to copy+delete.
- Resume is not supported for either direction.
- lwext4 is not thread-safe; all calls are serialised through one critical
  section, so parallel panel operations do not overlap.
- Write-back caching is enabled for multi-file operations and flushed at the end
  of each file, so an abort costs at most the file in flight.

## License

**tclwext4 is GPL-2.0-or-later.** Not by preference — lwext4 is mixed-licensed,
and two of the files this plugin links (`ext4_extent.c`, `ext4_xattr.c`) are
GPL-2.0-or-later while the rest is BSD-3-Clause. `ext4_extent.c` cannot be
dropped: extents are how every modern ext4 filesystem stores its block map.

The obligation attaches on **distribution**, not on use. Internal use triggers
nothing; shipping a binary means shipping complete corresponding source for the
whole plugin.

See [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for the per-file
breakdown, the note on `src/wfxplugin.h` not being an SDK copy, and the
unresolved question of a GPL plugin loaded into proprietary Total Commander.
That question is one for counsel, not for this README.
