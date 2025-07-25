// This file is Copyright (c) 2017-2020 Florent Kermarrec <florent@enjoy-digital.fr>
// This file is Copyright (c) 2019-2020 Gabriel L. Somlo <gsomlo@gmail.com>
// This file is Copyright (c) 2019 Kees Jongenburger <kees.jongenburger@gmail.com>
// This file is Copyright (c) 2018 bunnie <bunnie@kosagi.com>
// This file is Copyright (c) 2020 Antmicro <www.antmicro.com>
// License: BSD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <generated/csr.h>
#include <generated/mem.h>
#include <generated/soc.h>
#include <system.h>

#include <libfatfs/ff.h>
#include <libfatfs/diskio.h>
#include "sdcard.h"

#ifdef CSR_SDCARD_BASE

//#define SDCARD_DEBUG
//#define SDCARD_CMD23_SUPPORT /* SET_BLOCK_COUNT */
#define SDCARD_CMD18_SUPPORT /* READ_MULTIPLE_BLOCK */
#define SDCARD_CMD25_SUPPORT /* WRITE_MULTIPLE_BLOCK */

#ifndef SDCARD_CLK_FREQ_INIT
#define SDCARD_CLK_FREQ_INIT 400000
#endif

#ifndef SDCARD_CLK_FREQ
#define SDCARD_CLK_FREQ 25000000
#endif

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/*-----------------------------------------------------------------------*/
/* SDCard command helpers                                                */
/*-----------------------------------------------------------------------*/

int sdcard_wait_cmd_done(void) {
	unsigned int event;
#ifdef SDCARD_DEBUG
	uint32_t r[SD_CMD_RESPONSE_SIZE/4];
	printf("cmdevt: wait for event & 0x1\n");
#endif
	for (;;) {
		event = sdcard_core_cmd_event_read();
		busy_wait_us(10);
		if (event & 0x1)
			break;
	}
#ifdef SDCARD_DEBUG
	printf("cmdevt: %08x\n", event);
	csr_rd_buf_uint32(CSR_SDCARD_CORE_CMD_RESPONSE_ADDR,
			  r, SD_CMD_RESPONSE_SIZE/4);
	printf("%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n", r[0], r[1], r[2], r[3]);
#endif
	if (event & 0x4)
		return SD_TIMEOUT;
	if (event & 0x8)
		return SD_CRCERROR;
	return SD_OK;
}

int sdcard_wait_data_done(void) {
	unsigned int event;
#ifdef SDCARD_DEBUG
	printf("dataevt: wait for event & 0x1\n");
#endif
	for (;;) {
		event = sdcard_core_data_event_read();
		if (event & 0x1)
			break;
		busy_wait_us(10);
	}
#ifdef SDCARD_DEBUG
	printf("dataevt: %08x\n", event);
#endif
	if (event & 0x4)
		return SD_TIMEOUT;
	else if (event & 0x8)
		return SD_CRCERROR;
	return SD_OK;
}

/*-----------------------------------------------------------------------*/
/* SDCard clocker functions                                              */
/*-----------------------------------------------------------------------*/

void sdcard_set_clk_freq(unsigned long clk_freq, int show) {
	uint32_t divider;
	divider = clk_freq ? DIV_ROUND_UP(CONFIG_CLOCK_FREQUENCY, clk_freq) : 256;
	divider = min(max(divider, 2), 256);
#ifdef SDCARD_DEBUG
	show = 1;
#endif
	if (show) {
		/* this is the *effective* new clk_freq */
		clk_freq = CONFIG_CLOCK_FREQUENCY/ ((divider + 1) & ~1);
		printf("Setting SDCard clk freq to ");
		if (clk_freq > 1000000)
			printf("%ld MHz\n", clk_freq/1000000);
		else
			printf("%ld KHz\n", clk_freq/1000);
	}
	sdcard_phy_clocker_divider_write(divider);
}

/*-----------------------------------------------------------------------*/
/* SDCard commands functions                                             */
/*-----------------------------------------------------------------------*/

static inline int sdcard_send_command(uint32_t arg, uint8_t cmd, uint8_t rsp) {
	sdcard_core_cmd_argument_write(arg);
	sdcard_core_cmd_command_write((cmd << 8) | rsp);
	sdcard_core_cmd_send_write(1);
	return sdcard_wait_cmd_done();
}

int sdcard_go_idle(void) {
#ifdef SDCARD_DEBUG
	printf("CMD0: GO_IDLE\n");
#endif
	return sdcard_send_command(0, 0, SDCARD_CTRL_RESPONSE_NONE);
}

int sdcard_send_ext_csd(void) {
	uint32_t arg = 0x000001aa;
#ifdef SDCARD_DEBUG
	printf("CMD8: SEND_EXT_CSD, arg: 0x%08" PRIx32 "\n", arg);
#endif
	return sdcard_send_command(arg, 8, SDCARD_CTRL_RESPONSE_SHORT);
}

int sdcard_app_cmd(uint16_t rca) {
#ifdef SDCARD_DEBUG
	printf("CMD55: APP_CMD\n");
#endif
	return sdcard_send_command(rca << 16, 55, SDCARD_CTRL_RESPONSE_SHORT);
}

int sdcard_app_send_op_cond(int hcs) {
	uint32_t arg = 0x10ff8000;
	if (hcs)
		arg |= 0x60000000;
#ifdef SDCARD_DEBUG
	printf("ACMD41: APP_SEND_OP_COND, arg: %08" PRIx32 "\n", arg);
#endif
	return sdcard_send_command(arg, 41, SDCARD_CTRL_RESPONSE_SHORT_BUSY);
}

int sdcard_all_send_cid(void) {
#ifdef SDCARD_DEBUG
	printf("CMD2: ALL_SEND_CID\n");
#endif
	return sdcard_send_command(0, 2, SDCARD_CTRL_RESPONSE_LONG);
}

int sdcard_set_relative_address(void) {
#ifdef SDCARD_DEBUG
	printf("CMD3: SET_RELATIVE_ADDRESS\n");
#endif
	return sdcard_send_command(0, 3, SDCARD_CTRL_RESPONSE_SHORT);
}

int sdcard_send_cid(uint16_t rca) {
#ifdef SDCARD_DEBUG
	printf("CMD10: SEND_CID\n");
#endif
	return sdcard_send_command(rca << 16, 10, SDCARD_CTRL_RESPONSE_LONG);
}

int sdcard_send_csd(uint16_t rca) {
#ifdef SDCARD_DEBUG
	printf("CMD9: SEND_CSD\n");
#endif
	return sdcard_send_command(rca << 16, 9, SDCARD_CTRL_RESPONSE_LONG);
}

int sdcard_select_card(uint16_t rca) {
#ifdef SDCARD_DEBUG
	printf("CMD7: SELECT_CARD\n");
#endif
	return sdcard_send_command(rca << 16, 7, SDCARD_CTRL_RESPONSE_SHORT_BUSY);
}

int sdcard_app_set_bus_width(void) {
#ifdef SDCARD_DEBUG
	printf("ACMD6: SET_BUS_WIDTH\n");
#endif
	return sdcard_send_command(2, 6, SDCARD_CTRL_RESPONSE_SHORT);
}

int sdcard_switch(unsigned int mode, unsigned int group, unsigned int value) {
	unsigned int arg;
	arg = (mode << 31) | 0xffffff;
	arg &= ~(0xf << (group * 4));
	arg |= value << (group * 4);
#ifdef SDCARD_DEBUG
	printf("CMD6: SWITCH_FUNC\n");
#endif
	sdcard_core_block_length_write(64);
	sdcard_core_block_count_write(1);
	while (sdcard_send_command(arg, 6,
		(SDCARD_CTRL_DATA_TRANSFER_READ << 5) |
		SDCARD_CTRL_RESPONSE_SHORT) != SD_OK);
	return sdcard_wait_data_done();
}

int sdcard_app_send_scr(void) {
#ifdef SDCARD_DEBUG
	printf("CMD51: APP_SEND_SCR\n");
#endif
	sdcard_core_block_length_write(8);
	sdcard_core_block_count_write(1);
	while (sdcard_send_command(0, 51,
		(SDCARD_CTRL_DATA_TRANSFER_READ << 5) |
		SDCARD_CTRL_RESPONSE_SHORT) != SD_OK);
	return sdcard_wait_data_done();
}

int sdcard_app_set_blocklen(unsigned int blocklen) {
#ifdef SDCARD_DEBUG
	printf("CMD16: SET_BLOCKLEN\n");
#endif
	return sdcard_send_command(blocklen, 16, SDCARD_CTRL_RESPONSE_SHORT);
}

int sdcard_write_single_block(unsigned int blockaddr) {
#ifdef SDCARD_DEBUG
	printf("CMD24: WRITE_SINGLE_BLOCK\n");
#endif
	sdcard_core_block_length_write(512);
	sdcard_core_block_count_write(1);
	while (sdcard_send_command(blockaddr, 24,
	    (SDCARD_CTRL_DATA_TRANSFER_WRITE << 5) |
	    SDCARD_CTRL_RESPONSE_SHORT) != SD_OK);
	return SD_OK;
}

int sdcard_write_multiple_block(unsigned int blockaddr, unsigned int blockcnt) {
#ifdef SDCARD_DEBUG
	printf("CMD25: WRITE_MULTIPLE_BLOCK\n");
#endif
	sdcard_core_block_length_write(512);
	sdcard_core_block_count_write(blockcnt);
	while (sdcard_send_command(blockaddr, 25,
	    (SDCARD_CTRL_DATA_TRANSFER_WRITE << 5) |
	    SDCARD_CTRL_RESPONSE_SHORT) != SD_OK);
	return SD_OK;
}

int sdcard_read_single_block(unsigned int blockaddr) {
#ifdef SDCARD_DEBUG
	printf("CMD17: READ_SINGLE_BLOCK\n");
#endif
	sdcard_core_block_length_write(512);
	sdcard_core_block_count_write(1);
	while (sdcard_send_command(blockaddr, 17,
	    (SDCARD_CTRL_DATA_TRANSFER_READ << 5) |
	    SDCARD_CTRL_RESPONSE_SHORT) != SD_OK);
	return sdcard_wait_data_done();
}

int sdcard_read_multiple_block(unsigned int blockaddr, unsigned int blockcnt) {
#ifdef SDCARD_DEBUG
	printf("CMD18: READ_MULTIPLE_BLOCK\n");
#endif
	sdcard_core_block_length_write(512);
	sdcard_core_block_count_write(blockcnt);
	while (sdcard_send_command(blockaddr, 18,
	    (SDCARD_CTRL_DATA_TRANSFER_READ << 5) |
	    SDCARD_CTRL_RESPONSE_SHORT) != SD_OK);
	return sdcard_wait_data_done();
}

int sdcard_stop_transmission(void) {
#ifdef SDCARD_DEBUG
	printf("CMD12: STOP_TRANSMISSION\n");
#endif
	return sdcard_send_command(0, 12, SDCARD_CTRL_RESPONSE_SHORT_BUSY);
}

int sdcard_send_status(uint16_t rca) {
#ifdef SDCARD_DEBUG
	printf("CMD13: SEND_STATUS\n");
#endif
	return sdcard_send_command(rca << 16, 13, SDCARD_CTRL_RESPONSE_SHORT);
}

int sdcard_set_block_count(unsigned int blockcnt) {
#ifdef SDCARD_DEBUG
	printf("CMD23: SET_BLOCK_COUNT\n");
#endif
	return sdcard_send_command(blockcnt, 23, SDCARD_CTRL_RESPONSE_SHORT);
}

uint16_t sdcard_decode_rca(void) {
	uint32_t r[SD_CMD_RESPONSE_SIZE/4];
	csr_rd_buf_uint32(CSR_SDCARD_CORE_CMD_RESPONSE_ADDR,
			  r, SD_CMD_RESPONSE_SIZE/4);
	return (r[3] >> 16) & 0xffff;
}

#ifdef SDCARD_DEBUG
void sdcard_decode_cid(void) {
	uint32_t r[SD_CMD_RESPONSE_SIZE/4];
	csr_rd_buf_uint32(CSR_SDCARD_CORE_CMD_RESPONSE_ADDR,
			  r, SD_CMD_RESPONSE_SIZE/4);
	printf(
		"CID Register: 0x%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n"
		"Manufacturer ID: 0x%" PRIx16 "\n"
		"Application ID 0x%" PRIx16 "\n"
		"Product name: %c%c%c%c%c\n"
		"CRC: %02" PRIx8 "\n"
		"Production date(m/yy): %" PRIu8 "/%" PRIu8 "\n"
		"PSN: %08" PRIx32 "\n"
		"OID: %c%c\n",

		r[0], r[1], r[2], r[3],

		(uint16_t)((r[0] >> 16) & 0xffff),

		(uint16_t)(r[0] & 0xffff),

		(char)((r[1] >> 24) & 0xff), (char)((r[1] >> 16) & 0xff),
		(char)((r[1] >>  8) & 0xff), (char)((r[1] >>  0) & 0xff), (char)((r[2] >> 24) & 0xff),

		(uint8_t)(r[3] & 0xff),

		(uint8_t)((r[3] >>  8) & 0x0f), (uint8_t)((r[3] >> 12) & 0xff),

		(r[3] >> 24) | (r[2] <<  8),

		(char)((r[0] >> 16)) & 0xff, (char)((r[0] >>  8) & 0xff)
	);
}

void sdcard_decode_csd(void) {
	uint32_t r[SD_CMD_RESPONSE_SIZE/4];
	csr_rd_buf_uint32(CSR_SDCARD_CORE_CMD_RESPONSE_ADDR,
			  r, SD_CMD_RESPONSE_SIZE/4);
	/* FIXME: only support CSR structure version 2.0 */
	printf(
		"CSD Register: 0x%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n"
		"Max data transfer rate: %" PRIu8 " MB/s\n"
		"Max read block length: %" PRIu32 " bytes\n"
		"Device size: %" PRIu32 " GB\n",

		r[0], r[1], r[2], r[3],

		(uint8_t)((r[0] >> 24) & 0xff),

		(uint32_t)(1 << ((r[1] >> 16) & 0xf)),

		(uint32_t)(((r[2] >> 16) + ((r[1] & 0xff) << 16) + 1) * 512 / (1024 * 1024))
	);
}
#endif

/*-----------------------------------------------------------------------*/
/* SDCard user functions                                                 */
/*-----------------------------------------------------------------------*/

int sdcard_init(void) {
	uint16_t rca, timeout;
	uint32_t r[SD_CMD_RESPONSE_SIZE/4];

	/* Set SD clk freq to Initialization frequency */
	sdcard_set_clk_freq(SDCARD_CLK_FREQ_INIT, 0);
	busy_wait(1);

	for (timeout=1000; timeout>0; timeout--) {
		/* Set SDCard in SPI Mode (generate 80 dummy clocks) */
		sdcard_phy_init_initialize_write(1);
		busy_wait(1);

		/* Set SDCard in Idle state */
		if (sdcard_go_idle() == SD_OK)
			break;
		busy_wait(1);
	}
	if (timeout == 0)
		return 0;

	/* Set SDCard voltages, only supported by ver2.00+ SDCards */
	if (sdcard_send_ext_csd() != SD_OK)
		return 0;

	/* Set SD clk freq to Operational frequency */
	sdcard_set_clk_freq(SDCARD_CLK_FREQ, 0);
	busy_wait(1);

	/* Set SDCard in Operational state */
	for (timeout=1000; timeout>0; timeout--) {
		sdcard_app_cmd(0);
		if (sdcard_app_send_op_cond(1) == SD_OK) {
			csr_rd_buf_uint32(CSR_SDCARD_CORE_CMD_RESPONSE_ADDR,
			  r, SD_CMD_RESPONSE_SIZE/4);

			if (r[3] & 0x80000000) /* Busy bit, set when init is complete */
				break;
		}
		busy_wait(1);
	}
	if (timeout == 0)
		return 0;

	/* Send identification */
	if (sdcard_all_send_cid() != SD_OK)
		return 0;
#ifdef SDCARD_DEBUG
	sdcard_decode_cid();
#endif
	/* Set Relative Card Address (RCA) */
	if (sdcard_set_relative_address() != SD_OK)
		return 0;
	rca = sdcard_decode_rca();

	/* Set CID */
	if (sdcard_send_cid(rca) != SD_OK)
		return 0;
#ifdef SDCARD_DEBUG
	/* FIXME: add cid decoding (optional) */
#endif

	/* Set CSD */
	if (sdcard_send_csd(rca) != SD_OK)
		return 0;
#ifdef SDCARD_DEBUG
	sdcard_decode_csd();
#endif

	/* Select card */
	if (sdcard_select_card(rca) != SD_OK)
		return 0;

	/* Set 4-bit bus width */
	if (sdcard_app_cmd(rca) != SD_OK)
		return 0;
	if(sdcard_app_set_bus_width() != SD_OK)
		return 0;
	sdcard_phy_settings_write(SD_PHY_SPEED_4X);

	/* Switch speed */
	if (sdcard_switch(SD_SWITCH_SWITCH, SD_GROUP_ACCESSMODE, SD_SPEED_SDR25) != SD_OK)
		return 0;

	/* Send SCR */
	/* FIXME: add scr decoding (optional) */
	if (sdcard_app_cmd(rca) != SD_OK)
		return 0;
	if (sdcard_app_send_scr() != SD_OK)
		return 0;

	/* Set block length */
	if (sdcard_app_set_blocklen(512) != SD_OK)
		return 0;

	return 1;
}

#ifdef CSR_SDCARD_BLOCK2MEM_DMA_BASE_ADDR

void sdcard_read(uint32_t block, uint32_t count, uint8_t* buf)
{
	while (count) {
		uint32_t nblocks;
#ifdef SDCARD_CMD18_SUPPORT
		nblocks = count;
#else
		nblocks = 1;
#endif
		/* Initialize DMA Writer */
		sdcard_block2mem_dma_enable_write(0);
		sdcard_block2mem_dma_base_write((uint64_t)(uintptr_t) buf);
		sdcard_block2mem_dma_length_write(512*nblocks);
		sdcard_block2mem_dma_enable_write(1);

		/* Read Block(s) from SDCard */
#ifdef SDCARD_CMD23_SUPPORT
		sdcard_set_block_count(nblocks);
#endif
		if (nblocks > 1)
			sdcard_read_multiple_block(block, nblocks);
		else
			sdcard_read_single_block(block);

		/* Wait for DMA Writer to complete */
		while ((sdcard_block2mem_dma_done_read() & 0x1) == 0);

		/* Stop transmission (Only for multiple block reads) */
		if (nblocks > 1)
			sdcard_stop_transmission();

		/* Update Block/Buffer/Count */
		block += nblocks;
		buf   += 512*nblocks;
		count -= nblocks;
	}

#ifndef CONFIG_CPU_HAS_DMA_BUS
	/* Flush caches */
	flush_cpu_dcache();
	flush_l2_cache();
#endif
}

#endif

#ifdef CSR_SDCARD_MEM2BLOCK_DMA_BASE_ADDR

void sdcard_write(uint32_t block, uint32_t count, uint8_t* buf)
{
	while (count) {
		uint32_t nblocks;
#ifdef SDCARD_CMD25_SUPPORT
		nblocks = count;
#else
		nblocks = 1;
#endif
		/* Initialize DMA Reader */
		sdcard_mem2block_dma_enable_write(0);
		sdcard_mem2block_dma_base_write((uint64_t)(uintptr_t) buf);
		sdcard_mem2block_dma_length_write(512*nblocks);
		sdcard_mem2block_dma_enable_write(1);

		/* Write Block(s) to SDCard */
#ifdef SDCARD_CMD23_SUPPORT
		sdcard_set_block_count(nblocks);
#endif
		if (nblocks > 1)
			sdcard_write_multiple_block(block, nblocks);
		else
			sdcard_write_single_block(block);

		/* Stop transmission (Only for multiple block writes) */
		if (nblocks > 1)
			sdcard_stop_transmission();

		/* Wait for DMA Reader to complete */
		while ((sdcard_mem2block_dma_done_read() & 0x1) == 0);

		/* Update Block/Buffer/Count */
		block += nblocks;
		buf   += 512*nblocks;
		count -= nblocks;
	}
}
#endif

/*-----------------------------------------------------------------------*/
/* SDCard FatFs disk functions                                           */
/*-----------------------------------------------------------------------*/

static DSTATUS sdcardstatus = STA_NOINIT;

static DSTATUS sd_disk_status(BYTE drv) {
	if (drv) return STA_NOINIT;
	return sdcardstatus;
}

static DSTATUS sd_disk_initialize(BYTE drv) {
	if (drv) return STA_NOINIT;
	if (sdcardstatus)
		sdcardstatus = sdcard_init() ? 0 : STA_NOINIT;
	return sdcardstatus;
}

static DRESULT sd_disk_read(BYTE drv, BYTE *buf, LBA_t block, UINT count) {
	sdcard_read(block, count, buf);
	return RES_OK;
}

static DISKOPS SdCardDiskOps = {
	.disk_initialize = sd_disk_initialize,
	.disk_status = sd_disk_status,
	.disk_read = sd_disk_read,
};

void fatfs_set_ops_sdcard(void) {
	FfDiskOps = &SdCardDiskOps;
}

#endif /* CSR_SDCARD_BASE */
