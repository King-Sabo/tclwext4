/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * config/squashfuse/win_decompress.c.inc
 *
 * Shadows squashfuse's own win_decompress.c.inc to add an xz decompressor,
 * without patching the submodule.
 *
 * HOW THE SHADOW WORKS
 * --------------------
 * squashfuse's decompress.c contains, on _WIN32:
 *
 *     #include "win_decompress.c.inc"
 *
 * Note the missing "win/" prefix: the file actually lives in
 * external/squashfuse/win/. A quoted include is resolved first against the
 * directory of the file containing the directive - external/squashfuse/ - where
 * it is NOT found, so resolution falls through to the include path. As long as
 * config/squashfuse comes before external/squashfuse/win there, this file wins,
 * and it pulls in the original explicitly below.
 *
 * WHY IT IS DONE THIS WAY
 * -----------------------
 * squashfuse selects decompressors through a static table in decompress.c, with
 * no registration hook. Supporting a codec whose library it does not already
 * know about therefore means either patching the submodule, which this project
 * never does, or injecting a definition into that translation unit - which is
 * what this file is.
 *
 * FRAGILITY - READ BEFORE BUMPING squashfuse
 * ------------------------------------------
 * If upstream ever changes that line to "win/win_decompress.c.inc", the include
 * resolves inside the submodule directory, this shadow is silently bypassed,
 * and xz support disappears with no build error. The symptom is an xz SquashFS
 * failing to mount with "unsupported compression" despite the scan having
 * advertised xz as supported; tcl_sqfs_mount() logs a pointer back to this
 * comment when it sees exactly that combination.
 */

/* The original, providing the zlib decompressor over tinfl. */
#include "../../external/squashfuse/win/win_decompress.c.inc"

#ifdef TCLWEXT4_WITH_XZ

#include "xz.h"

/*
 * Single-call xz decode. squashfuse passes one complete compressed block and a
 * buffer big enough for its uncompressed size, which is what XZ_SINGLE expects.
 *
 * The CRC tables are global to XZ Embedded and must be initialised once before
 * any decode. Total Commander calls into the plugin from several threads, but
 * every path reaching a decompressor already holds g_ext4_cs, so a plain flag
 * is sufficient here.
 */
static sqfs_err sqfs_decompressor_xz(void *in, size_t insz,
		void *out, size_t *outsz) {
	static int crc_ready = 0;
	struct xz_dec *dec;
	struct xz_buf b;
	enum xz_ret ret;

	if (!crc_ready) {
		xz_crc32_init();
#ifdef XZ_USE_CRC64
		xz_crc64_init();
#endif
		crc_ready = 1;
	}

	/* dict_max is unused in single-call mode: the output buffer is the
	   dictionary. */
	dec = xz_dec_init(XZ_SINGLE, 0);
	if (!dec)
		return SQFS_ERR;

	b.in = (const uint8_t *)in;
	b.in_pos = 0;
	b.in_size = insz;
	b.out = (uint8_t *)out;
	b.out_pos = 0;
	b.out_size = *outsz;

	ret = xz_dec_run(dec, &b);
	xz_dec_end(dec);

	if (ret != XZ_STREAM_END)
		return SQFS_ERR;

	*outsz = b.out_pos;
	return SQFS_OK;
}
#define CAN_DECOMPRESS_XZ 1

#endif /* TCLWEXT4_WITH_XZ */
