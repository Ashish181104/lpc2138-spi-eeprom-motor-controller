/*
 * SPI EEPROM self-test (LPC2138 + 25LC256)
 * Writes 0x5A and 0xA5 to address 0x0010 and reads them back.
 * LED: 3 blinks, then ON = pass, OFF = fail (cause in g_fail_code).
 */

#define REG32(addr) (*(volatile unsigned int *)(addr))

#define PINSEL0  REG32(0xE002C000)

#define IO0PIN   REG32(0xE0028000)
#define IO0SET   REG32(0xE0028004)
#define IO0DIR   REG32(0xE0028008)
#define IO0CLR   REG32(0xE002800C)

#define S0SPCR   REG32(0xE0020000)
#define S0SPSR   REG32(0xE0020004)
#define S0SPDR   REG32(0xE0020008)
#define S0SPCCR  REG32(0xE002000C)

#define PCONP    REG32(0xE01FC0C4)

#define LED_PIN  (1U << 1)
#define CS_PIN   (1U << 7)

#define PINSEL0_P01_MASK   (3U << 2)
#define PINSEL0_SPI_MASK   (0xFFU << 8)
#define PINSEL0_SPI_VALUE  ((1U << 8) | (1U << 10) | (1U << 12))

#define PCONP_PCSPI0  (1U << 8)

#define SPCR_CPHA  (1U << 3)
#define SPCR_CPOL  (1U << 4)
#define SPCR_MSTR  (1U << 5)
#define SPCR_LSBF  (1U << 6)

#define SPSR_ABRT  (1U << 3)
#define SPSR_MODF  (1U << 4)
#define SPSR_ROVR  (1U << 5)
#define SPSR_WCOL  (1U << 6)
#define SPSR_SPIF  (1U << 7)

#define EE_WRSR   0x01U
#define EE_WRITE  0x02U
#define EE_READ   0x03U
#define EE_WRDI   0x04U
#define EE_RDSR   0x05U
#define EE_WREN   0x06U

#define SR_WIP    0x01U
#define SR_WEL    0x02U
#define SR_BP     0x0CU

#define TEST_ADDR      0x0010U
#define PATTERN_1      0x5AU
#define PATTERN_2      0xA5U

#define SPI_TIMEOUT    100000U
#define WIP_TIMEOUT    20000U

#define FAIL_NONE          0U
#define FAIL_SPI_TIMEOUT   1U
#define FAIL_SPI_FAULT     2U
#define FAIL_NO_RESPONSE   3U
#define FAIL_PROTECTED     4U
#define FAIL_WEL_NOT_SET   5U
#define FAIL_WRITE_TIMEOUT 6U
#define FAIL_VERIFY_1      7U
#define FAIL_VERIFY_2      8U

#define RESULT_PASS        0x600DU
#define RESULT_FAIL        0x0BADU

volatile unsigned int g_stage;
volatile unsigned int g_fail_code;
volatile unsigned int g_result;
volatile unsigned int g_spsr;
volatile unsigned int g_status_initial;
volatile unsigned int g_status_after_wren;
volatile unsigned int g_status_after_write;
volatile unsigned int g_read_1;
volatile unsigned int g_read_2;

static void delay(unsigned int count)
{
    volatile unsigned int i;

    for (i = 0; i < count; i++) {
    }
}

static void cs_low(void)
{
    IO0CLR = CS_PIN;
    delay(10U);
}

static void cs_high(void)
{
    delay(10U);
    IO0SET = CS_PIN;
    delay(10U);
}

/* Send/receive one byte with time-out */
static unsigned int spi_transfer(unsigned char tx, unsigned char *rx)
{
    unsigned int timeout = SPI_TIMEOUT;
    unsigned int status;
    unsigned char data;

    S0SPDR = tx;

    for (;;) {
        status = S0SPSR;

        if ((status & SPSR_SPIF) != 0U) {
            break;
        }
        if ((status & (SPSR_MODF | SPSR_ABRT)) != 0U) {
            g_spsr = status;
            return FAIL_SPI_FAULT;
        }
        if (--timeout == 0U) {
            g_spsr = status;
            return FAIL_SPI_TIMEOUT;
        }
    }

    data = (unsigned char)S0SPDR;

    if (rx != 0) {
        *rx = data;
    }

    return FAIL_NONE;
}

static unsigned int ee_read_status(unsigned char *status)
{
    unsigned int err;

    cs_low();
    err = spi_transfer(EE_RDSR, 0);
    if (err == FAIL_NONE) {
        err = spi_transfer(0x00U, status);
    }
    cs_high();

    return err;
}

static unsigned int ee_write_enable(void)
{
    unsigned int err;

    cs_low();
    err = spi_transfer(EE_WREN, 0);
    cs_high();

    return err;
}

static unsigned int ee_wait_ready(unsigned char *final_status)
{
    unsigned int polls;
    unsigned int err;
    unsigned char status = 0xFFU;

    for (polls = 0; polls < WIP_TIMEOUT; polls++) {
        err = ee_read_status(&status);
        if (err != FAIL_NONE) {
            return err;
        }
        if ((status & SR_WIP) == 0U) {
            *final_status = status;
            return FAIL_NONE;
        }
    }

    *final_status = status;
    return FAIL_WRITE_TIMEOUT;
}

static unsigned int ee_write_byte(unsigned int address, unsigned char data)
{
    unsigned int err;
    unsigned char status = 0U;

    err = ee_write_enable();
    if (err != FAIL_NONE) {
        return err;
    }

    err = ee_read_status(&status);
    g_status_after_wren = status;
    if (err != FAIL_NONE) {
        return err;
    }
    if ((status & SR_WEL) == 0U) {
        return FAIL_WEL_NOT_SET;
    }

    cs_low();
    err = spi_transfer(EE_WRITE, 0);
    if (err == FAIL_NONE) {
        err = spi_transfer((unsigned char)((address >> 8) & 0x7FU), 0);
    }
    if (err == FAIL_NONE) {
        err = spi_transfer((unsigned char)(address & 0xFFU), 0);
    }
    if (err == FAIL_NONE) {
        err = spi_transfer(data, 0);
    }
    cs_high();

    if (err != FAIL_NONE) {
        return err;
    }

    err = ee_wait_ready(&status);
    g_status_after_write = status;

    return err;
}

static unsigned int ee_read_byte(unsigned int address, unsigned char *data)
{
    unsigned int err;

    cs_low();
    err = spi_transfer(EE_READ, 0);
    if (err == FAIL_NONE) {
        err = spi_transfer((unsigned char)((address >> 8) & 0x7FU), 0);
    }
    if (err == FAIL_NONE) {
        err = spi_transfer((unsigned char)(address & 0xFFU), 0);
    }
    if (err == FAIL_NONE) {
        err = spi_transfer(0x00U, data);
    }
    cs_high();

    return err;
}

static unsigned int run_test(void)
{
    unsigned int err;
    unsigned char status = 0U;
    unsigned char data = 0U;

    g_stage = 1U;
    err = ee_read_status(&status);
    g_status_initial = status;
    if (err != FAIL_NONE) {
        return err;
    }
    if (status == 0xFFU) {
        return FAIL_NO_RESPONSE;
    }
    if ((status & SR_BP) == SR_BP) {
        return FAIL_PROTECTED;
    }

    g_stage = 2U;
    err = ee_write_byte(TEST_ADDR, PATTERN_1);
    if (err != FAIL_NONE) {
        return err;
    }
    err = ee_read_byte(TEST_ADDR, &data);
    g_read_1 = data;
    if (err != FAIL_NONE) {
        return err;
    }
    if (data != PATTERN_1) {
        return FAIL_VERIFY_1;
    }

    g_stage = 3U;
    err = ee_write_byte(TEST_ADDR, PATTERN_2);
    if (err != FAIL_NONE) {
        return err;
    }
    err = ee_read_byte(TEST_ADDR, &data);
    g_read_2 = data;
    if (err != FAIL_NONE) {
        return err;
    }
    if (data != PATTERN_2) {
        return FAIL_VERIFY_2;
    }

    g_stage = 4U;
    return FAIL_NONE;
}

int main(void)
{
    unsigned int i;

    PINSEL0 = (PINSEL0 & ~(PINSEL0_P01_MASK | PINSEL0_SPI_MASK))
              | PINSEL0_SPI_VALUE;

    IO0SET = CS_PIN;
    IO0DIR |= LED_PIN | CS_PIN;
    IO0CLR = LED_PIN;

    for (i = 0; i < 3U; i++) {
        IO0SET = LED_PIN;
        delay(50000U);
        IO0CLR = LED_PIN;
        delay(50000U);
    }
    delay(150000U);

    PCONP |= PCONP_PCSPI0;
    S0SPCCR = 8U;
    S0SPCR = SPCR_MSTR;

    g_fail_code = run_test();

    if (g_fail_code == FAIL_NONE) {
        g_result = RESULT_PASS;
        IO0SET = LED_PIN;
    } else {
        g_result = RESULT_FAIL;
        IO0CLR = LED_PIN;
    }

    while (1) {
    }
}
