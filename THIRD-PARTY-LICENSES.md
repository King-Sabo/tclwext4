# Third-party licenses

## Summary

**tclwext4 as a whole is GPL-2.0-or-later.** This is not a choice — it follows
from linking two GPL-licensed files out of lwext4.

The obligation attaches on **distribution**, not on use. Building and running
this internally triggers nothing. Shipping a binary to anyone outside your
organisation means shipping complete corresponding source for the whole plugin
under GPLv2 terms.

## lwext4

Upstream: https://github.com/gkostka/lwext4 (vendored unmodified as a submodule
at `external/lwext4`)

lwext4 is **mixed-licensed**. Its top-level `LICENSE` file contains GPLv2, with
a preface stating that files carrying their own license statement are governed
by that statement instead. In practice:

| Component | License |
|---|---|
| `src/ext4_extent.c` | **GPL-2.0-or-later** |
| `src/ext4_xattr.c` | **GPL-2.0-or-later** |
| Most other `src/*.c` and `include/*.h` | BSD-3-Clause (Grzegorz Kostka, Kaho Ng, others) |
| `src/ext4_hash.c` | BSD, derived from FreeBSD (Zheng Liu, Vyacheslav Matyushin) |
| `src/ext4_crc32.c` | BSD, derived from FreeBSD |
| `include/misc/tree.h` | BSD-2-Clause (Niels Provos), from NetBSD/OpenBSD |
| `include/misc/queue.h` | BSD-3-Clause, Regents of the University of California |

### Why the GPL files cannot simply be dropped

`ext4_extent.c` implements the extent tree. Extents are how every ext4
filesystem created in the last fifteen years stores its block map, so a build
without it cannot read the media this plugin exists to access. It is not
optional in any meaningful sense.

`ext4_xattr.c` *can* be excluded by setting `CONFIG_XATTR_ENABLE 0` in
`config/generated/ext4_config.h`, at the cost of extended-attribute support.
That does not change the outcome on its own, since `ext4_extent.c` remains.

If the BSD-only subset ever matters to you, it is `CONFIG_EXT_FEATURE_SET_LVL 2`
(ext2, no extents, no xattr) — a genuinely different and much less useful
product.

## FatFs

Upstream: http://elm-chan.org/fsw/ff/ (vendored unmodified as a submodule at
`external/fatfs`, via the https://github.com/abbrev/fatfs mirror)

FatFs is **BSD-1-Clause** (the "FatFs License"): a single condition requiring the
copyright notice to be retained in source redistributions. It is GPL-compatible
and adds no obligation beyond what lwext4 already imposes, so tclwext4 as a whole
remains GPL-2.0-or-later.

`config/ffconf.h` is derived from upstream's `ffconf.h` and carries FatFs's
license, not the GPL.

Note that FatFs was chosen over [fat_io_lib](https://github.com/ultraembedded/fat_io_lib)
partly on licensing grounds: fat_io_lib ships a GPLv3 `LICENSE` file while every
source header states GPLv2-or-later, an ambiguity that would have had to be
resolved before it could be combined with lwext4's GPL-2.0-or-later code. It also
keeps its filesystem state in a single global, so only one FAT volume could be
mounted at a time.

## Total Commander plugin interface

`src/wfxplugin.h` was written against Ghisler's publicly documented WFX API. It
is **not** a copy of the `wfxplugin.h` shipped in the Total Commander plugin
SDK, and no SDK file is vendored here. The constants and function signatures it
declares are interface facts required for binary compatibility.

## Open question: GPL plugin inside a proprietary host

Total Commander is proprietary and loads this plugin with `LoadLibrary` into its
own address space. Whether that constitutes a "combined work" under the GPL is
genuinely contested:

- The FSF's position is that a plugin sharing an address space with the host,
  exchanging data structures and calling each other's functions, forms a single
  combined work.
- The opposing reading is that a documented, stable plugin ABI is an
  arm's-length interface, and that the plugin is a separate program the user
  chooses to load.

This project does not link against any Total Commander library and imports
nothing from the host; TC calls exported entry points. That is a point in favour
of the second reading, but it does not settle the question.

**Nobody involved in writing this is a lawyer.** If distribution is being
considered, have counsel look at it rather than relying on this file.

## Recording the obligation

If you distribute a build, you need to:

1. Ship complete corresponding source for the plugin, including `src/`,
   `config/`, and the build files.
2. Identify the exact lwext4 commit used (the submodule gitlink records it) and
   make that source available.
3. Include this file and `LICENSE`.
4. Keep the per-file BSD notices intact — the BSD-licensed majority of lwext4 is
   still separately available under BSD terms, and downstream users retain that
   option for those files.
