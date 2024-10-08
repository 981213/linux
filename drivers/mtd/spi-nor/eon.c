// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/mtd/spi-nor.h>

#include "core.h"

static void
eon_default_init(struct spi_nor *nor)
{
	nor->flags &= ~SNOR_F_HAS_16BIT_SR;
	nor->params->quad_enable = NULL;
}

static int
eon_post_bfpt_fixups(struct spi_nor *nor,
		     const struct sfdp_parameter_header *bfpt_header,
		     const struct sfdp_bfpt *bfpt)
{
	/* EON BFPT reports 4-4-4 Fast Read support, however it requires a
	 * vendor-specific opcode 0x38 (EQPI) which statefully turns every
	 * operation into 4-4-4 mode, in which single input op won't work
	 * any more.
	 * For backward-compatibility, only use the stateless 1-4-4 mode.
	 *
	 * It also reports wrong wait states for quad reads (31 instead of 4).
	 */
	nor->params->hwcaps.mask &= ~SNOR_HWCAPS_READ_4_4_4;
	nor->params->reads[SNOR_CMD_READ_1_4_4].num_wait_states = 4;

	return 0;
}

static const struct spi_nor_fixups eon_fixups = {
	.default_init	= eon_default_init,
	.post_bfpt	= eon_post_bfpt_fixups,
};

static const struct flash_info eon_nor_parts[] = {
	/* EON -- en25xxx */
	{ "en25f32",    INFO(0x1c3116, 0, 64 * 1024,   64)
		NO_SFDP_FLAGS(SECT_4K) },
	{ "en25p32",    INFO(0x1c2016, 0, 64 * 1024,   64) },
	{ "en25q32b",   INFO(0x1c3016, 0, 64 * 1024,   64) },
	{ "en25p64",    INFO(0x1c2017, 0, 64 * 1024,  128) },
	{ "en25q64",    INFO(0x1c3017, 0, 64 * 1024,  128)
		NO_SFDP_FLAGS(SECT_4K) },
	{ "en25q128",   INFO(0x1c3018, 0, 64 * 1024,  256)
		NO_SFDP_FLAGS(SECT_4K) },
	{ "en25qx128a", INFO(0x1c7118, 0, 64 * 1024, 256) },
	{ "en25q80a",   INFO(0x1c3014, 0, 64 * 1024,   16)
		NO_SFDP_FLAGS(SECT_4K | SPI_NOR_DUAL_READ) },
	{ "en25qh16",   INFO(0x1c7015, 0, 64 * 1024,   32)
		NO_SFDP_FLAGS(SECT_4K | SPI_NOR_DUAL_READ) },
	{ "en25qh32",   INFO(0x1c7016, 0, 64 * 1024,   64) },
	{ "en25qh64",   INFO(0x1c7017, 0, 64 * 1024,  128)
		NO_SFDP_FLAGS(SECT_4K | SPI_NOR_DUAL_READ) },
	{ "en25qh128",  INFO(0x1c7018, 0, 64 * 1024,  256)
		NO_SFDP_FLAGS(SECT_4K | SPI_NOR_DUAL_READ) },
	{ "en25qh256",  INFO(0x1c7019, 0, 64 * 1024,  512)
		PARSE_SFDP },
	{ "en25s64",	INFO(0x1c3817, 0, 64 * 1024,  128)
		NO_SFDP_FLAGS(SECT_4K) },
};

const struct spi_nor_manufacturer spi_nor_eon = {
	.name = "eon",
	.parts = eon_nor_parts,
	.nparts = ARRAY_SIZE(eon_nor_parts),
	.fixups = &eon_fixups,
};
