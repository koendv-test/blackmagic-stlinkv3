#include "general.h"

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/spi.h>

#include "engine.h"
#include "sf-word-wizard.h"

/* SPI5 pins on the stlinkv3-mini. */
#define STLINKV3_MINI_SPI		SPI5
#define STLINKV3_MINI_SPI_RCC		RCC_SPI5
/* Alternate function number for the spi pins. */
#define STLINKV3_MINI_SPI_AF_NUMBER	GPIO_AF5

#define	STLINKV3_MINI_SPI_MOSI_PORT	GPIOF
#define	STLINKV3_MINI_SPI_MOSI_PIN	GPIO9
#define	STLINKV3_MINI_SPI_MISO_PORT	GPIOF
#define	STLINKV3_MINI_SPI_MISO_PIN	GPIO8
#define	STLINKV3_MINI_SPI_SCK_PORT	GPIOH
#define	STLINKV3_MINI_SPI_SCK_PIN	GPIO6

#define CS_PORT				GPIOA
#define CS_PIN				GPIO7
#define RESET_PORT			GPIOA
#define RESET_PIN			GPIO6

/* These are used for oscilloscope measurements triggering. */
#define SCOPE_TRIGGER_PORT		GPIOA
#define SCOPE_TRIGGER_PIN		GPIO7

static void do_spi_send8(void)
{ spi_send8(STLINKV3_MINI_SPI, sf_pop()); sf_push(spi_read8(STLINKV3_MINI_SPI)); }

static void do_spi_send(void)
{ sf_push(spi_xfer(STLINKV3_MINI_SPI, sf_pop())); }

static void do_spi_cpol0(void) { spi_set_clock_polarity_0(STLINKV3_MINI_SPI); }
static void do_spi_cpol1(void) { spi_set_clock_polarity_1(STLINKV3_MINI_SPI); }

static void do_spi_cphase0(void) { spi_set_clock_phase_0(STLINKV3_MINI_SPI); }
static void do_spi_cphase1(void) { spi_set_clock_phase_1(STLINKV3_MINI_SPI); }

static void do_spi_set_baud_prescaler(void) { spi_set_baudrate_prescaler(STLINKV3_MINI_SPI, sf_pop() & 7); }
static void do_spi_set_data_bitsize(void)
{
	static const uint16_t data_sizes[16] =
	{
		SPI_CR2_DS_4BIT,
		SPI_CR2_DS_4BIT,
		SPI_CR2_DS_4BIT,
		SPI_CR2_DS_4BIT,
		SPI_CR2_DS_5BIT,
		SPI_CR2_DS_6BIT,
		SPI_CR2_DS_7BIT,
		SPI_CR2_DS_8BIT,
		SPI_CR2_DS_9BIT,
		SPI_CR2_DS_10BIT,
		SPI_CR2_DS_11BIT,
		SPI_CR2_DS_12BIT,
		SPI_CR2_DS_13BIT,
		SPI_CR2_DS_14BIT,
		SPI_CR2_DS_15BIT,
		SPI_CR2_DS_16BIT,
	};
	unsigned t = sf_pop() & 15;
	spi_set_data_size(STLINKV3_MINI_SPI, data_sizes[t]);
}

static void do_swdio_float(void)
{
	gpio_mode_setup(STLINKV3_MINI_SPI_MOSI_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_MOSI_PIN);
	gpio_set_af(STLINKV3_MINI_SPI_MOSI_PORT, STLINKV3_MINI_SPI_AF_NUMBER, STLINKV3_MINI_SPI_MOSI_PIN);
}

static void do_swdio_drive(void)
{
	gpio_mode_setup(STLINKV3_MINI_SPI_MOSI_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_MOSI_PIN);
	gpio_set_output_options(STLINKV3_MINI_SPI_MOSI_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, STLINKV3_MINI_SPI_MOSI_PIN);
	gpio_set_af(STLINKV3_MINI_SPI_MOSI_PORT, STLINKV3_MINI_SPI_AF_NUMBER, STLINKV3_MINI_SPI_MOSI_PIN);
}

static void do_spi_to_gpio(void)
{
	spi_disable(STLINKV3_MINI_SPI);

	do_swdio_float();

	gpio_mode_setup(STLINKV3_MINI_SPI_SCK_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_SCK_PIN);
	gpio_set_output_options(STLINKV3_MINI_SPI_SCK_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, STLINKV3_MINI_SPI_SCK_PIN);
	gpio_set_af(STLINKV3_MINI_SPI_SCK_PORT, STLINKV3_MINI_SPI_AF_NUMBER, STLINKV3_MINI_SPI_SCK_PIN);
}

static void do_gpio_to_spi(void)
{
	gpio_mode_setup(STLINKV3_MINI_SPI_MOSI_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_MOSI_PIN);
	gpio_set_output_options(STLINKV3_MINI_SPI_MOSI_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, STLINKV3_MINI_SPI_MOSI_PIN);
	gpio_set_af(STLINKV3_MINI_SPI_MOSI_PORT, STLINKV3_MINI_SPI_AF_NUMBER, STLINKV3_MINI_SPI_MOSI_PIN);

	gpio_mode_setup(STLINKV3_MINI_SPI_MISO_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_MISO_PIN);
	gpio_set_af(STLINKV3_MINI_SPI_MISO_PORT, STLINKV3_MINI_SPI_AF_NUMBER, STLINKV3_MINI_SPI_MISO_PIN);

	gpio_mode_setup(STLINKV3_MINI_SPI_SCK_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_SCK_PIN);
	gpio_set_output_options(STLINKV3_MINI_SPI_SCK_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, STLINKV3_MINI_SPI_SCK_PIN);
	gpio_set_af(STLINKV3_MINI_SPI_SCK_PORT, STLINKV3_MINI_SPI_AF_NUMBER, STLINKV3_MINI_SPI_SCK_PIN);

	spi_enable(STLINKV3_MINI_SPI);
}

static void do_swdio_read(void) { sf_push(gpio_get(STLINKV3_MINI_SPI_MOSI_PORT, STLINKV3_MINI_SPI_MOSI_PIN) ? 1 : 0); }

static void do_swdio_hi(void) { gpio_set(STLINKV3_MINI_SPI_MOSI_PORT, STLINKV3_MINI_SPI_MOSI_PIN); }
static void do_swdio_low(void) { gpio_clear(STLINKV3_MINI_SPI_MOSI_PORT, STLINKV3_MINI_SPI_MOSI_PIN); }

static void do_swck_hi(void) { gpio_set(STLINKV3_MINI_SPI_SCK_PORT, STLINKV3_MINI_SPI_SCK_PIN); }
static void do_swck_low(void) { gpio_clear(STLINKV3_MINI_SPI_SCK_PORT, STLINKV3_MINI_SPI_SCK_PIN); }

static void do_scope_hi(void) { gpio_set(SCOPE_TRIGGER_PORT, SCOPE_TRIGGER_PIN); }
static void do_scope_low(void) { gpio_clear(SCOPE_TRIGGER_PORT, SCOPE_TRIGGER_PIN); }

static void do_cs_hi(void) { gpio_set(CS_PORT, CS_PIN); }
static void do_cs_low(void) { gpio_clear(CS_PORT, CS_PIN); }

static void do_reset_hi(void) { gpio_set(RESET_PORT, RESET_PIN); }
static void do_reset_low(void) { gpio_clear(RESET_PORT, RESET_PIN); }

static void do_tdi_hi(void) { gpio_set(GPIOF, GPIO8); }
static void do_tdi_low(void) { gpio_clear(GPIOF, GPIO8); }


void hw_init(void)
{
	gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO1);

	/* Configure scope trigger signal. */
	gpio_mode_setup(SCOPE_TRIGGER_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, SCOPE_TRIGGER_PIN);
	gpio_set_output_options(SCOPE_TRIGGER_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, SCOPE_TRIGGER_PIN);

	gpio_mode_setup(CS_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, CS_PIN);
	gpio_set_output_options(CS_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, CS_PIN);
	do_cs_hi();

	gpio_mode_setup(RESET_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, RESET_PIN);
	gpio_set_output_options(RESET_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, RESET_PIN);
	do_reset_low();

	rcc_periph_clock_enable(STLINKV3_MINI_SPI_RCC);
	/* Configure spi pins - used for swd bus driving. */
	gpio_mode_setup(STLINKV3_MINI_SPI_MOSI_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_MOSI_PIN);
	gpio_set_output_options(STLINKV3_MINI_SPI_MOSI_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, STLINKV3_MINI_SPI_MOSI_PIN);
	gpio_set_af(STLINKV3_MINI_SPI_MOSI_PORT, STLINKV3_MINI_SPI_AF_NUMBER, STLINKV3_MINI_SPI_MOSI_PIN);

	gpio_mode_setup(STLINKV3_MINI_SPI_MISO_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_MISO_PIN);
	gpio_set_af(STLINKV3_MINI_SPI_MISO_PORT, STLINKV3_MINI_SPI_AF_NUMBER, STLINKV3_MINI_SPI_MISO_PIN);

	gpio_mode_setup(STLINKV3_MINI_SPI_SCK_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, STLINKV3_MINI_SPI_SCK_PIN);
	gpio_set_output_options(STLINKV3_MINI_SPI_SCK_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, STLINKV3_MINI_SPI_SCK_PIN);
	gpio_set_af(STLINKV3_MINI_SPI_SCK_PORT, STLINKV3_MINI_SPI_AF_NUMBER, STLINKV3_MINI_SPI_SCK_PIN);

	/* Set spi port. */
	spi_init_master(STLINKV3_MINI_SPI, SPI_CR1_BAUDRATE_FPCLK_DIV_8, SPI_CR1_CPOL_CLK_TO_0_WHEN_IDLE,
			SPI_CR1_CPHA_CLK_TRANSITION_1, SPI_CR1_MSBFIRST);
	SPI_CR2(STLINKV3_MINI_SPI) |= /* FRXTH */ 1 << 12;

	/* By default, drive the swd bus by gpio bitbanging. */
	//do_spi_to_gpio();
	//do_swck_low();

	/*
	gpio_mode_setup(GPIOF, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO8);
	gpio_set_output_options(GPIOF, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, GPIO8);
	gpio_set_af(GPIOF, STLINKV3_MINI_SPI_AF_NUMBER, GPIO8);
	// */
#if 0
void spi_enable(uint32_t spi);
void spi_disable(uint32_t spi);
void spi_write(uint32_t spi, uint16_t data);
void spi_send(uint32_t spi, uint16_t data);
uint16_t spi_read(uint32_t spi);
uint16_t spi_xfer(uint32_t spi, uint16_t data);
void spi_set_bidirectional_mode(uint32_t spi);
void spi_disable_crc(uint32_t spi);
void spi_set_full_duplex_mode(uint32_t spi);
void spi_enable_software_slave_management(uint32_t spi);
void spi_send_lsb_first(uint32_t spi);
void spi_send_msb_first(uint32_t spi);
void spi_set_baudrate_prescaler(uint32_t spi, uint8_t baudrate);
void spi_set_master_mode(uint32_t spi);
void spi_set_clock_polarity_1(uint32_t spi);
void spi_set_clock_polarity_0(uint32_t spi);
void spi_set_clock_phase_1(uint32_t spi);
void spi_set_clock_phase_0(uint32_t spi);
void spi_enable_ss_output(uint32_t spi);
void spi_disable_ss_output(uint32_t spi);
void spi_enable_tx_dma(uint32_t spi);
void spi_disable_tx_dma(uint32_t spi);
void spi_enable_rx_dma(uint32_t spi);
void spi_disable_rx_dma(uint32_t spi);
void spi_set_standard_mode(uint32_t spi, uint8_t mode);

int spi_init_master(uint32_t spi, uint32_t br, uint32_t cpol, uint32_t cpha, 
		uint32_t lsbfirst);
void spi_set_crcl_8bit(uint32_t spi);
void spi_set_crcl_16bit(uint32_t spi);
void spi_set_data_size(uint32_t spi, uint16_t data_s);
void spi_fifo_reception_threshold_8bit(uint32_t spi);
void spi_fifo_reception_threshold_16bit(uint32_t spi);
void spi_i2s_mode_spi_mode(uint32_t spi);
void spi_send8(uint32_t spi, uint8_t data);
uint8_t spi_read8(uint32_t spi);
#endif
}


static struct word dict_base_dummy_word[1] = { MKWORD(0, 0, "", 0), };
static const struct word custom_dict[] = {
	MKWORD(dict_base_dummy_word,	0,		"spi-send8",	do_spi_send8),
	MKWORD(custom_dict,		__COUNTER__,	"spi-send",	do_spi_send),
	MKWORD(custom_dict,		__COUNTER__,	"spi-cpol0",	do_spi_cpol0),
	MKWORD(custom_dict,		__COUNTER__,	"spi-cpol1",	do_spi_cpol1),
	MKWORD(custom_dict,		__COUNTER__,	"spi-cphase0",	do_spi_cphase0),
	MKWORD(custom_dict,		__COUNTER__,	"spi-cphase1",	do_spi_cphase1),

	MKWORD(custom_dict,		__COUNTER__,	"spi-set-baud-prescaler",	do_spi_set_baud_prescaler),
	MKWORD(custom_dict,		__COUNTER__,	"spi-set-data-bitsize",		do_spi_set_data_bitsize),

	MKWORD(custom_dict,		__COUNTER__,	"spi>gpio",	do_spi_to_gpio),
	MKWORD(custom_dict,		__COUNTER__,	"gpio>spi",	do_gpio_to_spi),

	MKWORD(custom_dict,		__COUNTER__,	"swdio-float",	do_swdio_float),
	MKWORD(custom_dict,		__COUNTER__,	"swdio-drive",	do_swdio_drive),

	MKWORD(custom_dict,		__COUNTER__,	"swdio-hi",	do_swdio_hi),
	MKWORD(custom_dict,		__COUNTER__,	"swdio-low",	do_swdio_low),

	MKWORD(custom_dict,		__COUNTER__,	"swdio-read",	do_swdio_read),

	MKWORD(custom_dict,		__COUNTER__,	"swck-hi",	do_swck_hi),
	MKWORD(custom_dict,		__COUNTER__,	"swck-low",	do_swck_low),

	MKWORD(custom_dict,		__COUNTER__,	"scope-hi",	do_scope_hi),
	MKWORD(custom_dict,		__COUNTER__,	"scope-low",	do_scope_low),

	MKWORD(custom_dict,		__COUNTER__,	"cs-hi",	do_cs_hi),
	MKWORD(custom_dict,		__COUNTER__,	"cs-low",	do_cs_low),

	MKWORD(custom_dict,		__COUNTER__,	"reset-hi",	do_reset_hi),
	MKWORD(custom_dict,		__COUNTER__,	"reset-low",	do_reset_low),

	MKWORD(custom_dict,		__COUNTER__,	"tdi-hi",	do_tdi_hi),
	MKWORD(custom_dict,		__COUNTER__,	"tdi-low",	do_tdi_low),

}, * custom_dict_start = custom_dict + __COUNTER__;


static void sf_stlinkv3_mini_dictionary(void) __attribute__((constructor));
static void sf_stlinkv3_mini_dictionary(void)
{
	sf_merge_custom_dictionary(dict_base_dummy_word, custom_dict_start);
}

