/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements the SW-DP interface. */

#include "general.h"
#include "timing.h"
#include "adiv5.h"
#include "exception.h"
#include <libopencm3/stm32/spi.h>
#include <gdb_packet.h>

/* kludge */
#define adiv5_debug_port_s ADIv5_DP_t
#define size_t             int
//#define DEBUG gdb_outf
#define DEBUG(...)

//#define ATTRIB_OPTIMIZE __attribute__((optimize(3)))
#define ATTRIB_OPTIMIZE

static uint32_t swdptap_seq_in(size_t clock_cycles) ATTRIB_OPTIMIZE;
static bool swdptap_seq_in_parity(uint32_t *ret, size_t clock_cycles) ATTRIB_OPTIMIZE;
static void swdptap_seq_out(uint32_t tms_states, size_t clock_cycles) ATTRIB_OPTIMIZE;
static void swdptap_seq_out_parity(uint32_t tms_states, size_t clock_cycles) ATTRIB_OPTIMIZE;

static const uint32_t data_size[] = {0, 0, 0, 0, SPI_CR2_DS_4BIT, SPI_CR2_DS_5BIT, SPI_CR2_DS_6BIT, SPI_CR2_DS_7BIT,
       SPI_CR2_DS_8BIT, SPI_CR2_DS_9BIT, SPI_CR2_DS_10BIT, SPI_CR2_DS_11BIT, SPI_CR2_DS_12BIT, SPI_CR2_DS_13BIT,
       SPI_CR2_DS_14BIT, SPI_CR2_DS_15BIT, SPI_CR2_DS_16BIT};

static uint32_t next_bit = 0;
static bool next_bit_valid = false;
static bool mode_tx = false;
static uint32_t swd_data_size = 8;
uint32_t spi_clock_divisor = SPI_CR1_BR_FPCLK_DIV_64;

static void ll_swd_init()
{
	gpio_mode_setup(SWD_MOSI_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, SWD_MOSI_PIN);
	gpio_set_af(SWD_MOSI_PORT, GPIO_AF5, SWD_MOSI_PIN);
	gpio_set_output_options(SWD_MOSI_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, SWD_MOSI_PIN);

	//gpio_mode_setup(SWD_MISO_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, SWD_MISO_PIN);

	gpio_mode_setup(SWD_MISO_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, SWD_MISO_PIN);
	gpio_set_af(SWD_MISO_PORT, GPIO_AF5, SWD_MISO_PIN);
	gpio_set_output_options(SWD_MISO_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, SWD_MISO_PIN);

	gpio_mode_setup(SWD_SCK_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, SWD_SCK_PIN);
	gpio_set_af(SWD_SCK_PORT, GPIO_AF5, SWD_SCK_PIN);
	gpio_set_output_options(SWD_SCK_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, SWD_SCK_PIN);

	spi_reset(SWD_SPI);
	spi_init_master(SWD_SPI, spi_clock_divisor << 3, SPI_CR1_CPOL_CLK_TO_1_WHEN_IDLE,
		SPI_CR1_CPHA_CLK_TRANSITION_2, SPI_CR1_LSBFIRST);
	spi_set_data_size(SWD_SPI, SPI_CR2_DS_8BIT);
	spi_disable_crc(SWD_SPI);
	spi_fifo_reception_threshold_8bit(SWD_SPI);
	spi_set_unidirectional_mode(SWD_SPI);
	spi_set_full_duplex_mode(SWD_SPI);
	spi_enable_software_slave_management(SWD_SPI);
	spi_set_nss_high(SWD_SPI);
	spi_enable(SWD_SPI);
	DEBUG("ll_swd_init()\r\n");
}

/* inline versions of libopencm3 spi v2 library */

static inline void ll_spi_disable()
{
	SPI_CR1(SWD_SPI) = SPI_CR1(SWD_SPI) & ~(SPI_CR1_SPE); /* disable spi */
}

static inline void ll_spi_enable()
{
	SPI_CR1(SWD_SPI) = SPI_CR1(SWD_SPI) | SPI_CR1_SPE; /* enable spi. */
}

void ll_spi_set_data_size(uint16_t data_s)
{
        SPI_CR2(SWD_SPI) = (SPI_CR2(SWD_SPI) & ~SPI_CR2_DS_MASK) |
                       (data_s & SPI_CR2_DS_MASK);
}

static inline void ll_spi_send8(uint8_t data)
{
	/* Wait for transfer finished. */
	while (!(SPI_SR(SWD_SPI) & SPI_SR_TXE));
	SPI_DR8(SWD_SPI) = data;
}

static inline uint8_t ll_spi_read8()
{
	/* Wait for transfer finished. */
	while (!(SPI_SR(SWD_SPI) & SPI_SR_RXNE));
	return SPI_DR8(SWD_SPI);
}

/* MOSI and MISO are connected.
   set MOSI pin as input when receiving */

static inline void ll_mode_rx()
{
	if (mode_tx) {
		DEBUG(">ll_mode_rx\r\n");
		gpio_mode_setup(SWD_MOSI_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, SWD_MOSI_PIN);
		mode_tx = false;
		DEBUG("<ll_mode_rx\r\n");
	}
}

/* set MOSI pin as spi output when transmitting */

static inline void ll_mode_tx()
{
	if (!mode_tx) {
		DEBUG(">ll_mode_tx\r\n");
		gpio_mode_setup(SWD_MOSI_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, SWD_MOSI_PIN);
		gpio_set_af(SWD_MOSI_PORT, GPIO_AF5, SWD_MOSI_PIN);
		gpio_set_output_options(SWD_MOSI_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, SWD_MOSI_PIN);
		mode_tx = true;
		DEBUG("<ll_mode_tx\r\n");
	}
}

/* static */ void ll_drain_rx()
{
	(void) SPI_DR8(SWD_SPI);
	(void) SPI_DR8(SWD_SPI);
	(void) SPI_DR8(SWD_SPI);
	(void) SPI_DR8(SWD_SPI);
}

static inline void ll_data_size(uint32_t new_data_size)
{
	if (swd_data_size != new_data_size) {
		DEBUG(">ll_data_size(%d)\r\n", new_data_size);
		ll_spi_disable();
		ll_spi_set_data_size(data_size[new_data_size]);
		ll_spi_enable();
		swd_data_size = new_data_size;
		DEBUG("<ll_data_size(%d)\r\n", new_data_size);
	}
}

static inline uint32_t ll_seq_in(int clock_cycles)
{
	uint32_t retval;
	DEBUG(">ll_seq_in(clock_cycles = %d)\r\n", clock_cycles);
	ll_mode_rx();
	ll_data_size(clock_cycles);
	ll_spi_send8(0);
	retval = ll_spi_read8(SWD_SPI);
	DEBUG("<ll_seq_in(clock_cycles = %d) = %08x\r\n", clock_cycles, retval);
	return retval;
}

static inline void ll_seq_out(const uint32_t tms_states, const int clock_cycles)
{
	DEBUG(">ll_seq_out(clock_cycles = %d)\r\n", clock_cycles);
	ll_mode_tx();
	ll_data_size(clock_cycles);
	ll_spi_send8(tms_states);
	(void) ll_spi_read8();
	DEBUG("<ll_seq_out(clock_cycles = %d)\r\n", clock_cycles);
	return;
}

static uint32_t swdptap_seq_in(size_t clock_cycles)
{
	uint32_t ack_frame;
	if (clock_cycles != 3)
		raise_exception(EXCEPTION_ERROR, "seq_in clock_cycles != 3");

	/* We need to read an ack, which is 3 bits, but the spi hardware can only read 4 to 16 bits.
	so we read one more bit than necessary, and store the last bit as the first bit of the next operation.
	if the next operation is seq_in or seq_in_parity, the last bit read is a data bit.
	if the next operation is seq_out or seq_out_parity, the last bit read is a turn bit. */

	DEBUG(">swdptap_seq_in(clock_cycles = %d)\n", clock_cycles);
	if (next_bit_valid) {
		/* If the previous operation was a read, read 4 bits.
		the first 3 bits are the ack frame, keep.
		the last bit is either a turn bit or a data bit, keep. */
		ack_frame = ll_seq_in(4);
	} else {
		/* If the previous operation was a write, read 5 bit.
		the first bit is a turn bit, discard.
		the next 3 bits are the ack frame, keep.
		the last bit is either a turn bit or a data bit, keep.  */
		ack_frame = ll_seq_in(5) >> 1;
	}

	uint32_t retval = ack_frame & 0x7;
	next_bit = ack_frame >> 3 & 0x1;
	next_bit_valid = true;
	DEBUG("<swdptap_seq_in(clock_cycles = %d) = %d\n", clock_cycles, retval);

	return retval;
}

static bool swdptap_seq_in_parity(uint32_t *ret, size_t clock_cycles)
{
	if (clock_cycles != 32)
		raise_exception(EXCEPTION_ERROR, "seq_in_parity clock_cycles != 32");

	if (!next_bit_valid)
		raise_exception(EXCEPTION_ERROR, "seq_in_parity !next_bit_valid");

	/* If this is seq_in_parity(32), then the previous operation was seq_in(3),
	and next_bit stores the first bit of seq_in_parity(32) */

	DEBUG("<swdptap_seq_in_parity(clock_cycles = %d)\n", clock_cycles);
	const uint32_t rd1 = ll_seq_in(8);  // bits 1..8, next_bit is bit 0
	const uint32_t rd9 = ll_seq_in(8);  // bits 9..16
	const uint32_t rd17 = ll_seq_in(8); // bits 17..24
	const uint32_t rd25 = ll_seq_in(5); // bits 25..29
	const uint32_t rd30 = ll_seq_in(4); // bits 30..31 + parity + turn
	const uint32_t parity_bit = rd30 >> 2 & 0x1;
	const uint32_t result = rd30 << 30 | rd25 << 25 | rd17 << 17 | rd9 << 9 | rd1 << 1 | next_bit;
	size_t parity = __builtin_parity(result);
	parity += parity_bit;
	*ret = result;
	next_bit_valid = false;
	DEBUG(">swdptap_seq_in_parity(clock_cycles = %d) = %08x\n", clock_cycles, result);

	return parity & 1U;
}

static void swdptap_seq_out(const uint32_t tms_states, const size_t clock_cycles)
{
	uint32_t dta = tms_states;

	DEBUG("<swdptap_seq_out(tms_states = %08x, clock_cycles = %d)\n", tms_states, clock_cycles);
	ll_drain_rx();
	ll_mode_tx();
	ll_data_size(8);
	switch (clock_cycles) {
	case 32:
		ll_spi_send8(dta & 0xff);
		ll_spi_send8(dta >> 8 & 0xff);
		(void)ll_spi_read8();
		ll_spi_send8(dta >> 16 & 0xff);
		(void)ll_spi_read8();
		ll_spi_send8(dta >> 24 & 0xff);
		(void)ll_spi_read8();
		(void)ll_spi_read8();
		break;
	case 16:
		ll_spi_send8(dta & 0xff);
		ll_spi_send8(dta >> 8 & 0xff);
		(void)ll_spi_read8();
		(void)ll_spi_read8();
		break;
	case 12:
		ll_spi_send8(dta & 0xff);
		ll_data_size(4);
		ll_spi_send8(dta >> 8 & 0xf);
		(void)ll_spi_read8();
		(void)ll_spi_read8();
		break;
	case 8:
		ll_spi_send8(dta & 0xff);
		(void)ll_spi_read8();
		break;
	default:
		raise_exception(EXCEPTION_ERROR, "seq_out clock_cycles");
	}
	next_bit_valid = false;
	DEBUG(">swdptap_seq_out(tms_states = %08x, clock_cycles = %d)\n", tms_states, clock_cycles);

	return;
}

static void swdptap_seq_out_parity(const uint32_t tms_states, const size_t clock_cycles)
{
	//swd_out_parity_cnt[clock_cycles]++;
	DEBUG("<swdptap_seq_out_parity(tms_states = %08x, clock_cycles = %d)\n", tms_states, clock_cycles);
	if (clock_cycles != 32)
		raise_exception(EXCEPTION_ERROR, "seq_out_parity clock_cycles != 32");

	uint32_t parity = __builtin_parity(tms_states);
	ll_drain_rx();
	ll_mode_tx();
	ll_data_size(8);
	ll_spi_send8(tms_states & 0xff);
	ll_spi_send8(tms_states >> 8 & 0xff);
	(void)ll_spi_read8();
	ll_spi_send8(tms_states >> 16 & 0xff);
	(void)ll_spi_read8();
	ll_spi_send8(tms_states >> 24 & 0xff);
	(void)ll_spi_read8();
	ll_spi_send8(parity); // 7 extra clock cycles
	(void)ll_spi_read8();
	(void)ll_spi_read8();
	next_bit_valid = false;
	DEBUG(">swdptap_seq_out_parity(tms_states = %08x, clock_cycles = %d)\n", tms_states, clock_cycles);

	return;
}

int swdptap_init(adiv5_debug_port_s *dp)
{
	dp->seq_in = swdptap_seq_in;
	dp->seq_in_parity = swdptap_seq_in_parity;
	dp->seq_out = swdptap_seq_out;
	dp->seq_out_parity = swdptap_seq_out_parity;
	ll_swd_init();
	return 0;
}
