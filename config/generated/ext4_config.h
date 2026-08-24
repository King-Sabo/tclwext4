/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * config/generated/ext4_config.h
 *
 * Single source of truth for lwext4's build configuration.
 *
 * lwext4's own include/ext4_config.h starts with:
 *
 *     #if !CONFIG_USE_DEFAULT_CFG
 *     #include "generated/ext4_config.h"
 *     #endif
 *
 * That is the documented hook for supplying a configuration without touching
 * the submodule. We leave CONFIG_USE_DEFAULT_CFG at 0 so the include fires, and
 * put "config" on the include path so it resolves here. lwext4's include/
 * directory has no generated/ subdirectory of its own, so there is no ambiguity
 * and nothing in external/lwext4 is modified.
 *
 * Everything not defined here keeps upstream's default: each knob in
 * ext4_config.h is wrapped in #ifndef, so this header only overrides.
 *
 * IMPORTANT: this file is reached from the plugin sources too (via ext4.h), so
 * the static library and the DLL are guaranteed to agree on struct layouts.
 * That is the whole point of doing it this way rather than with per-project
 * preprocessor definitions - a mismatch in e.g. CONFIG_EXT4_MAX_MP_NAME would
 * silently change struct ext4_mountpoint between the two and corrupt memory
 * without any link error.
 */
#ifndef TCLWEXT4_GENERATED_EXT4_CONFIG_H_
#define TCLWEXT4_GENERATED_EXT4_CONFIG_H_

/* ---- feature level ------------------------------------------------------
 * F_SET_EXT4 (4) selects the widest supported feature masks lwext4 offers:
 * EXT4_SUPPORTED_FCOM / _FINCOM / _FRO_COM in ext4_types.h. The read-only
 * gate in tcl_scan.c compares superblock bits against exactly these macros,
 * so lowering this here automatically tightens the gate as well.
 */
#define CONFIG_EXT_FEATURE_SET_LVL 4

/* ---- optional subsystems ---- */
#define CONFIG_JOURNALING_ENABLE 1
#define CONFIG_XATTR_ENABLE      1
#define CONFIG_EXTENTS_ENABLE    1

/* ---- host characteristics ----
 * x86/x64 tolerate unaligned loads, which lets lwext4 skip byte-wise access.
 */
#define CONFIG_UNALIGNED_ACCESS 1

/* Use the CRT's errno values and O_* flags where MSVC provides them. */
#define CONFIG_HAVE_OWN_ERRNO  0
#define CONFIG_HAVE_OWN_OFLAGS 1
#define CONFIG_HAVE_OWN_ASSERT 1

/* ---- diagnostics ----
 * A DLL loaded into Total Commander has no usable stdout, and lwext4's assert
 * path calls abort(), which would take the whole file manager down.
 */
#define CONFIG_DEBUG_PRINTF 0
#define CONFIG_DEBUG_ASSERT 0

/* ---- sizing ----
 * Defaults are tuned for a single filesystem on a microcontroller. We may have
 * every partition of every attached disk plus a handful of images mounted at
 * once, and we are on a desktop with memory to spare.
 */
#define CONFIG_BLOCK_DEV_ENABLE_STATS   1
#define CONFIG_BLOCK_DEV_CACHE_SIZE     256
#define CONFIG_EXT4_BLOCKDEVS_COUNT     32
#define CONFIG_EXT4_MOUNTPOINTS_COUNT   32
#define CONFIG_EXT4_MAX_BLOCKDEV_NAME   64
#define CONFIG_EXT4_MAX_MP_NAME         64

/* Cap on a single truncate transaction (32 MiB). */
#define CONFIG_MAX_TRUNCATE_SIZE (32ul * 1024ul * 1024ul)

#endif /* TCLWEXT4_GENERATED_EXT4_CONFIG_H_ */
