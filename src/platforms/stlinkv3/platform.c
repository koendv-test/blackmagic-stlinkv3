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

/* This file provides the platform specific functions for the ST-Link V3
 * implementation.
 */

#include "general.h"
#include "cdcacm.h"
#include "usbuart.h"
#include "gdb_if.h"

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/scs.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/stm32/adc.h>

uint16_t led_idle_run;
uint16_t srst_pin;
static uint32_t hw_version;

#define SCB_CCR_IC_Pos                      17U                                           /*!< SCB CCR: Instruction cache enable bit Position */
#define SCB_CCR_IC_Msk                     (1UL << SCB_CCR_IC_Pos)                        /*!< SCB CCR: Instruction cache enable bit Mask */

#define SCB_CCR_DC_Pos                      16U                                           /*!< SCB CCR: Cache enable bit Position */
#define SCB_CCR_DC_Msk                     (1UL << SCB_CCR_DC_Pos)                        /*!< SCB CCR: DC Mask */

#define SCB_CCSIDR_NUMSETS_Msk             (0x7FFFUL << SCB_CCSIDR_NUMSETS_Pos)           /*!< SCB CCSIDR: NumSets Mask */
#define SCB_CCSIDR_NUMSETS_Pos             13U                                            /*!< SCB CCSIDR: NumSets Position */

#define SCB_CCSIDR_ASSOCIATIVITY_Pos        3U                                            /*!< SCB CCSIDR: Associativity Position */
#define SCB_CCSIDR_ASSOCIATIVITY_Msk       (0x3FFUL << SCB_CCSIDR_ASSOCIATIVITY_Pos)      /*!< SCB CCSIDR: Associativity Mask */
#define CCSIDR_WAYS(x)         (((x) & SCB_CCSIDR_ASSOCIATIVITY_Msk) >> SCB_CCSIDR_ASSOCIATIVITY_Pos)

#define SCB_DCISW_SET_Pos                   5U                                            /*!< SCB DCISW: Set Position */
#define SCB_DCISW_SET_Msk                  (0x1FFUL << SCB_DCISW_SET_Pos)                 /*!< SCB DCISW: Set Mask */

#define SCB_DCISW_WAY_Pos                  30U                                            /*!< SCB DCISW: Way Position */
#define SCB_DCISW_WAY_Msk                  (3UL << SCB_DCISW_WAY_Pos)                     /*!< SCB DCISW: Way Mask */

#define CCSIDR_SETS(x)         (((x) & SCB_CCSIDR_NUMSETS_Msk      ) >> SCB_CCSIDR_NUMSETS_Pos      )

static void __DSB(void)
{
	asm volatile ("dsb 0xF":::"memory");
}

static void __ISB(void)
{
	asm volatile ("isb 0xF":::"memory");
}

static void SCB_EnableICache (void)
{
	volatile uint32_t *SCB_ICIALLU =  (volatile uint32_t *)(SCB_BASE + 0x250);
	__DSB();
	__ISB();
	*SCB_ICIALLU = 0UL;                     /* invalidate I-Cache */
	__DSB();
	__ISB();
	SCB_CCR |=  (uint32_t)SCB_CCR_IC_Msk;  /* enable I-Cache */
	__DSB();
	__ISB();
}

static void SCB_EnableDCache (void)
{
	volatile uint32_t *SCB_CCSIDR = (volatile uint32_t *)(SCB_BASE +  0x80);
	volatile uint32_t *SCB_CSSELR = (volatile uint32_t *)(SCB_BASE +  0x84);
	volatile uint32_t *SCB_DCISW  =  (volatile uint32_t *)(SCB_BASE + 0x260);

	uint32_t ccsidr;
	uint32_t sets;
	uint32_t ways;

	*SCB_CSSELR = 0U; /*(0U << 1U) | 0U;*/  /* Level 1 data cache */
	__DSB();

	ccsidr = *SCB_CCSIDR;

	sets = (uint32_t)(CCSIDR_SETS(ccsidr));
	do {
		ways = (uint32_t)(CCSIDR_WAYS(ccsidr));
		do {
			*SCB_DCISW = (((sets << SCB_DCISW_SET_Pos) & SCB_DCISW_SET_Msk) |
					((ways << SCB_DCISW_WAY_Pos) & SCB_DCISW_WAY_Msk)  );
#if defined ( __CC_ARM )
			__schedule_barrier();
#endif
		} while (ways-- != 0U);
	} while(sets-- != 0U);
	__DSB();

	SCB_CCR |=  (uint32_t)SCB_CCR_DC_Msk;  /* enable D-Cache */

	__DSB();
	__ISB();
}

int platform_hwversion(void)
{
	return hw_version;
}

void platform_srst_set_val(bool assert)
{
	gpio_set_val(SRST_PORT, SRST_PIN, !assert);
	if (assert)
		for(int i = 0; i < 10000; i++)
			asm("nop");
}

bool platform_srst_get_val()
{
	return gpio_get(SRST_PORT, SRST_PIN) == 0;
}

const char *platform_target_voltage(void)
{
	/* On the stlinkv3, the target input voltage is divided by two.
	 * The ADC is sampling at 12 bit resolution.
	 * Vref+ input is assumed to be 3.3 volts. */
	static char ret[] = "0.0V";
	uint8_t channels[] = { ADC_CHANNEL0, };
	unsigned value;

	adc_set_regular_sequence(ADC1, 1, channels);
	adc_start_conversion_regular(ADC1);
	while (!adc_eoc(ADC1));
	value = adc_read_regular(ADC1);

	value *= 3379; /* 3.3 * 1024 == 3379.2 */
	value += 104858; /* round, 0.05V * 2 ^ 21 == 104857.6 */
	ret[0] = (value >> 21) + '0';
	value &= (1 << 21) - 1;
	value *= 10;
	ret[2] = (value >> 21) + '0';

	return ret;
}

void platform_request_boot(void)
{
	/* Use top of ITCM RAM as magic marker*/
	volatile uint32_t *magic = (volatile uint32_t *) 0x3ff8;
	magic[0] = BOOTMAGIC0;
	magic[1] = BOOTMAGIC1;
	scb_reset_system();
}

void platform_init(void)
{
	rcc_periph_clock_enable(RCC_APB2ENR_SYSCFGEN);
	rcc_clock_setup_hse(rcc_3v3 + RCC_CLOCK_3V3_216MHZ, 25);
	SCB_EnableICache();
	SCB_EnableDCache();
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_GPIOD);
	rcc_periph_clock_enable(RCC_GPIOH);
	rcc_periph_clock_enable(RCC_GPIOF);

	/* Initialize ADC. */
	gpio_mode_setup(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO0);
	rcc_periph_clock_enable(RCC_ADC1);
	adc_power_off(ADC1);
	adc_disable_scan_mode(ADC1);
	adc_set_sample_time(ADC1, ADC_CHANNEL0, ADC_SMPR_SMP_3CYC);
	adc_power_on(ADC1);

	/* Configure srst pin. */
	gpio_set_output_options(SRST_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_2MHZ, SRST_PIN);
	gpio_mode_setup(SRST_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, SRST_PIN);

	TMS_SET_MODE();
	/* Configure TDI pin. */
	gpio_mode_setup(TDI_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, TDI_PIN);
	gpio_set_output_options(TDI_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, TDI_PIN);

	/* Drive the swck pin low. */
	gpio_mode_setup(STLINKV3_MINI_SPI_SCK_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_SCK_PIN);
	gpio_set_output_options(STLINKV3_MINI_SPI_SCK_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, STLINKV3_MINI_SPI_SCK_PIN);

	/* Set up green/red led to steady green to indicate application active
	 * FIXME: Allow RED and yellow constant and blinking,
	 * e.g. by PWM onTIM1_CH3 (PA10)
	 */
	gpio_mode_setup(LED_RG_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LED_RG_PIN);
	gpio_set_output_options(LED_RG_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ,
							LED_RG_PIN);

	/* Relocate interrupt vector table here */
	extern int vector_table;
	SCB_VTOR = (uint32_t)&vector_table;

	platform_timing_init();
	cdcacm_init();
	usbuart_init();
	/* By default, do not drive the swd bus too fast. */
	platform_max_frequency_set(6000000);
}
