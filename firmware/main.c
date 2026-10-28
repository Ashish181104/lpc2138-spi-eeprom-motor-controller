/*
 * SPI EEPROM Motor-Profile Controller (LPC2138)
 * Loads a motor profile from a 25LC256 SPI EEPROM and runs it on a DC motor
 * through an L293D driver.
 */

/* 0 = software PWM on P0.8 (default), 1 = hardware PWM4 */
#define USE_HW_PWM  0

#define REG32(addr) (*(volatile unsigned int *)(addr))

/* LPC2138 registers */
#define PINSEL0  REG32(0xE002C000)

#define IO0PIN   REG32(0xE0028000)
#define IO0SET   REG32(0xE0028004)
#define IO0DIR   REG32(0xE0028008)
#define IO0CLR   REG32(0xE002800C)

#define S0SPCR   REG32(0xE0020000)
#define S0SPSR   REG32(0xE0020004)
#define S0SPDR   REG32(0xE0020008)
#define S0SPCCR  REG32(0xE002000C)

#define T0TCR    REG32(0xE0004004)
#define T0TC     REG32(0xE0004008)
#define T0PR     REG32(0xE000400C)
#define T0MCR    REG32(0xE0004014)
#define T0MR0    REG32(0xE0004018)

#define T1TCR    REG32(0xE0008004)
#define T1TC     REG32(0xE0008008)
#define T1PR     REG32(0xE000800C)
#define T1MCR    REG32(0xE0008014)

#define PWMTCR   REG32(0xE0014004)
#define PWMPR    REG32(0xE001400C)
#define PWMMCR   REG32(0xE0014014)
#define PWMMR0   REG32(0xE0014018)
#define PWMMR4   REG32(0xE0014040)
#define PWMPCR   REG32(0xE001404C)
#define PWMLER   REG32(0xE0014050)

#define PCONP    REG32(0xE01FC0C4)
#define VPBDIV   REG32(0xE01FC100)

/* 12 MHz crystal, no PLL; PCLK = CCLK / 4 */
#define CCLK_HZ  12000000U
#define PCLK_HZ  (CCLK_HZ / 4U)

/* Port 0 pins */
#define LED_PIN  (1U << 1)
#define CS_PIN   (1U << 7)
#define EN1_PIN  (1U << 8)
#define IN1_PIN  (1U << 12)
#define IN2_PIN  (1U << 13)

/* PINSEL0: P0.4-P0.6 = SPI0, all other pins used = GPIO */
#define PINSEL0_MASK   ((3U << 2) | (0xFFU << 8) | (3U << 16) | (0xFU << 24))
#define PINSEL0_SPI    ((1U << 8)  |     \
                        (1U << 10) |     \
                        (1U << 12))
#if USE_HW_PWM
#define PINSEL0_VALUE  (PINSEL0_SPI | (2U << 16))
#else
#define PINSEL0_VALUE  PINSEL0_SPI
#endif

#define PCONP_PCTIM0  (1U << 1)
#define PCONP_PCTIM1  (1U << 2)
#define PCONP_PCPWM0  (1U << 5)
#define PCONP_PCSPI0  (1U << 8)

#define TCR_ENABLE        (1U << 0)
#define TCR_RESET         (1U << 1)
#define MCR_MR0_RESET     (1U << 1)

#define SPCR_MSTR  (1U << 5)
#define SPSR_ABRT  (1U << 3)
#define SPSR_MODF  (1U << 4)
#define SPSR_SPIF  (1U << 7)

#define PWMTCR_COUNTER_ENABLE  (1U << 0)
#define PWMTCR_COUNTER_RESET   (1U << 1)
#define PWMTCR_PWM_ENABLE      (1U << 3)
#define PWMMCR_RESET_ON_MR0    (1U << 1)
#define PWMPCR_ENA4            (1U << 12)
#define PWMLER_EN0             (1U << 0)
#define PWMLER_EN4             (1U << 4)

/* 100 steps x 100 us = 10 ms period (100 Hz) */
#define PWM_STEPS      100U
#define PWM_TICK_HZ    10000U

/* 25LC256 opcodes and status bits */
#define EE_WRITE  0x02U
#define EE_READ   0x03U
#define EE_RDSR   0x05U
#define EE_WREN   0x06U
#define SR_WIP    0x01U
#define SR_WEL    0x02U
#define SR_BP     0x0CU
#define EE_PAGE_SIZE  64U

#define SPI_TIMEOUT    100000U
#define WIP_TIMEOUT    20000U

/* Error codes reported in g_fail_code */
#define FAIL_NONE          0U
#define FAIL_SPI_TIMEOUT   1U
#define FAIL_SPI_FAULT     2U
#define FAIL_NO_RESPONSE   3U
#define FAIL_PROTECTED     4U
#define FAIL_WEL_NOT_SET   5U
#define FAIL_WRITE_TIMEOUT 6U
#define FAIL_PROFILE_BAD   9U

#define DIR_STOP     0U
#define DIR_FORWARD  1U
#define DIR_REVERSE  2U

#define DIR_CHANGE_PAUSE_MS  200U

/* EEPROM profile: 'M' 'P' version count, 6 bytes per step, checksum */
#define PROFILE_ADDR     0x0000U
#define PROFILE_MAGIC_0  'M'
#define PROFILE_MAGIC_1  'P'
#define PROFILE_VERSION  2U
#define MAX_STEPS        8U
#define STEP_BYTES       6U
#define HEADER_BYTES     4U
#define PROFILE_MAX_BYTES (HEADER_BYTES + (MAX_STEPS * STEP_BYTES) + 1U)

#define SOURCE_NONE      0U
#define SOURCE_EEPROM    1U
#define SOURCE_DEFAULTS  2U

typedef struct {
    unsigned char  direction;
    unsigned char  speed;
    unsigned short ramp_ms;
    unsigned short run_ms;
} motor_step_t;

/* Written to the EEPROM when no valid profile is found */
static const motor_step_t default_steps[] = {
    { DIR_FORWARD, 75U, 500U, 3000U },
    { DIR_STOP,     0U, 300U, 1000U },
    { DIR_REVERSE, 40U, 500U, 3000U },
    { DIR_STOP,     0U, 300U, 1000U },
};
#define DEFAULT_STEP_COUNT (sizeof(default_steps) / sizeof(default_steps[0]))

static motor_step_t steps[MAX_STEPS];
static unsigned int step_count;

static unsigned int pwm_duty;
#if !USE_HW_PWM
static unsigned int en1_is_high;
#endif

/* Debug variables for the Proteus Watch Window */
volatile unsigned int g_fail_code;
volatile unsigned int g_profile_source;
volatile unsigned int g_step_count;
volatile unsigned int g_step;
volatile unsigned int g_direction;
volatile unsigned int g_speed;
volatile unsigned int g_cycles;

/* ARM7TDMI has no divide instruction */
static unsigned int udiv(unsigned int n, unsigned int d)
{
    unsigned int q = 0U;
    unsigned int bit = 1U;

    if (d == 0U) {
        return 0U;
    }
    while ((d < n) && ((d & 0x80000000U) == 0U)) {
        d <<= 1;
        bit <<= 1;
    }
    while (bit != 0U) {
        if (n >= d) {
            n -= d;
            q |= bit;
        }
        d >>= 1;
        bit >>= 1;
    }
    return q;
}

static void short_delay(void)
{
    volatile unsigned int i;

    for (i = 0; i < 10U; i++) {
    }
}

static void timers_init(void)
{
    PCONP |= PCONP_PCTIM0 | PCONP_PCTIM1;

    T1TCR = TCR_RESET;
    T1PR = (PCLK_HZ / 1000U) - 1U;
    T1MCR = 0U;
    T1TCR = TCR_ENABLE;

    T0TCR = TCR_RESET;
    T0PR = (PCLK_HZ / PWM_TICK_HZ) - 1U;
    T0MR0 = PWM_STEPS - 1U;
    T0MCR = MCR_MR0_RESET;
    T0TCR = TCR_ENABLE;
}

/* Software PWM: EN1 high while Timer0 count < duty */
static void pwm_service(void)
{
#if !USE_HW_PWM
    unsigned int want_high;

    if (pwm_duty == 0U) {
        want_high = 0U;
    } else if (pwm_duty >= PWM_STEPS) {
        want_high = 1U;
    } else {
        want_high = (T0TC < pwm_duty) ? 1U : 0U;
    }

    if (want_high != en1_is_high) {
        if (want_high != 0U) {
            IO0SET = EN1_PIN;
        } else {
            IO0CLR = EN1_PIN;
        }
        en1_is_high = want_high;
    }
#endif
}

static void delay_ms(unsigned int ms)
{
    unsigned int start = T1TC;

    while ((T1TC - start) < ms) {
        pwm_service();
    }
}

static void cs_low(void)
{
    IO0CLR = CS_PIN;
    short_delay();
}

static void cs_high(void)
{
    short_delay();
    IO0SET = CS_PIN;
    short_delay();
}

/* Send/receive one byte; returns an error on time-out or fault */
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
            return FAIL_SPI_FAULT;
        }
        if (--timeout == 0U) {
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

static unsigned int ee_wait_ready(void)
{
    unsigned int polls;
    unsigned int err;
    unsigned char status;

    for (polls = 0; polls < WIP_TIMEOUT; polls++) {
        err = ee_read_status(&status);
        if (err != FAIL_NONE) {
            return err;
        }
        if ((status & SR_WIP) == 0U) {
            return FAIL_NONE;
        }
    }

    return FAIL_WRITE_TIMEOUT;
}

static unsigned int ee_send_address(unsigned char cmd, unsigned int address)
{
    unsigned int err;

    err = spi_transfer(cmd, 0);
    if (err == FAIL_NONE) {
        err = spi_transfer((unsigned char)((address >> 8) & 0x7FU), 0);
    }
    if (err == FAIL_NONE) {
        err = spi_transfer((unsigned char)(address & 0xFFU), 0);
    }
    return err;
}

/* WREN, confirm WEL, WRITE, then wait for WIP to clear */
static unsigned int ee_write_page(unsigned int address,
                                  const unsigned char *buf, unsigned int len)
{
    unsigned int err;
    unsigned int i;
    unsigned char status = 0U;

    cs_low();
    err = spi_transfer(EE_WREN, 0);
    cs_high();
    if (err != FAIL_NONE) {
        return err;
    }

    err = ee_read_status(&status);
    if (err != FAIL_NONE) {
        return err;
    }
    if ((status & SR_WEL) == 0U) {
        return FAIL_WEL_NOT_SET;
    }

    cs_low();
    err = ee_send_address(EE_WRITE, address);
    for (i = 0; (i < len) && (err == FAIL_NONE); i++) {
        err = spi_transfer(buf[i], 0);
    }
    cs_high();

    if (err != FAIL_NONE) {
        return err;
    }

    return ee_wait_ready();
}

/* Split writes at 64-byte page boundaries */
static unsigned int ee_write_block(unsigned int address,
                                   const unsigned char *buf, unsigned int len)
{
    unsigned int err;
    unsigned int chunk;

    while (len > 0U) {
        chunk = EE_PAGE_SIZE - (address & (EE_PAGE_SIZE - 1U));
        if (chunk > len) {
            chunk = len;
        }

        err = ee_write_page(address, buf, chunk);
        if (err != FAIL_NONE) {
            return err;
        }

        address += chunk;
        buf += chunk;
        len -= chunk;
    }

    return FAIL_NONE;
}

static unsigned int ee_read_block(unsigned int address,
                                  unsigned char *buf, unsigned int len)
{
    unsigned int err;
    unsigned int i;

    cs_low();
    err = ee_send_address(EE_READ, address);
    for (i = 0; (i < len) && (err == FAIL_NONE); i++) {
        err = spi_transfer(0x00U, &buf[i]);
    }
    cs_high();

    return err;
}

static unsigned int ee_check(void)
{
    unsigned int err;
    unsigned char status = 0U;

    err = ee_read_status(&status);
    if (err != FAIL_NONE) {
        return err;
    }
    if (status == 0xFFU) {
        return FAIL_NO_RESPONSE;
    }
    if ((status & SR_BP) == SR_BP) {
        return FAIL_PROTECTED;
    }
    return FAIL_NONE;
}

static unsigned char checksum(const unsigned char *buf, unsigned int len)
{
    unsigned int i;
    unsigned char sum = 0U;

    for (i = 0; i < len; i++) {
        sum = (unsigned char)(sum + buf[i]);
    }
    return (unsigned char)(0U - sum);
}

static unsigned int profile_encode(const motor_step_t *src, unsigned int count,
                                   unsigned char *buf)
{
    unsigned int i;
    unsigned int n = HEADER_BYTES;

    buf[0] = PROFILE_MAGIC_0;
    buf[1] = PROFILE_MAGIC_1;
    buf[2] = PROFILE_VERSION;
    buf[3] = (unsigned char)count;

    for (i = 0; i < count; i++) {
        buf[n++] = src[i].direction;
        buf[n++] = src[i].speed;
        buf[n++] = (unsigned char)(src[i].ramp_ms >> 8);
        buf[n++] = (unsigned char)(src[i].ramp_ms & 0xFFU);
        buf[n++] = (unsigned char)(src[i].run_ms >> 8);
        buf[n++] = (unsigned char)(src[i].run_ms & 0xFFU);
    }

    buf[n] = checksum(buf, n);
    return n + 1U;
}

static unsigned int profile_decode(const unsigned char *buf)
{
    unsigned int count = buf[3];
    unsigned int len;
    unsigned int i;
    unsigned int n = HEADER_BYTES;
    unsigned char sum = 0U;

    if ((buf[0] != PROFILE_MAGIC_0) || (buf[1] != PROFILE_MAGIC_1) ||
        (buf[2] != PROFILE_VERSION) || (count == 0U) || (count > MAX_STEPS)) {
        return 0U;
    }

    len = HEADER_BYTES + (count * STEP_BYTES) + 1U;
    for (i = 0; i < len; i++) {
        sum = (unsigned char)(sum + buf[i]);
    }
    if (sum != 0U) {
        return 0U;
    }

    for (i = 0; i < count; i++) {
        if ((buf[n] > DIR_REVERSE) || (buf[n + 1U] > 100U)) {
            return 0U;
        }
        steps[i].direction = buf[n];
        steps[i].speed     = buf[n + 1U];
        steps[i].ramp_ms   = (unsigned short)((buf[n + 2U] << 8) | buf[n + 3U]);
        steps[i].run_ms    = (unsigned short)((buf[n + 4U] << 8) | buf[n + 5U]);
        n += STEP_BYTES;
    }

    step_count = count;
    return 1U;
}

/* Load the profile; if invalid, store defaults and read them back */
static unsigned int profile_load(void)
{
    unsigned char buf[PROFILE_MAX_BYTES];
    unsigned int len;
    unsigned int err;

    err = ee_check();
    if (err != FAIL_NONE) {
        return err;
    }

    err = ee_read_block(PROFILE_ADDR, buf, PROFILE_MAX_BYTES);
    if (err != FAIL_NONE) {
        return err;
    }
    if (profile_decode(buf) != 0U) {
        g_profile_source = SOURCE_EEPROM;
        return FAIL_NONE;
    }

    len = profile_encode(default_steps, DEFAULT_STEP_COUNT, buf);
    err = ee_write_block(PROFILE_ADDR, buf, len);
    if (err != FAIL_NONE) {
        return err;
    }

    err = ee_read_block(PROFILE_ADDR, buf, PROFILE_MAX_BYTES);
    if (err != FAIL_NONE) {
        return err;
    }
    if (profile_decode(buf) == 0U) {
        return FAIL_PROFILE_BAD;
    }

    g_profile_source = SOURCE_DEFAULTS;
    return FAIL_NONE;
}

static void pwm_init(void)
{
#if USE_HW_PWM
    PCONP |= PCONP_PCPWM0;
    PWMTCR = PWMTCR_COUNTER_RESET;
    PWMPR = (PCLK_HZ / PWM_TICK_HZ) - 1U;
    PWMMR0 = PWM_STEPS;
    PWMMR4 = 0U;
    PWMMCR = PWMMCR_RESET_ON_MR0;
    PWMPCR = PWMPCR_ENA4;
    PWMLER = PWMLER_EN0 | PWMLER_EN4;
    PWMTCR = PWMTCR_COUNTER_ENABLE | PWMTCR_PWM_ENABLE;
#else
    IO0CLR = EN1_PIN;
    en1_is_high = 0U;
#endif
    pwm_duty = 0U;
}

static void motor_set_speed(unsigned int percent)
{
    if (percent > 100U) {
        percent = 100U;
    }

#if USE_HW_PWM
    if (percent >= 100U) {
        PWMMR4 = PWM_STEPS + 1U;
    } else {
        PWMMR4 = percent;
    }
    PWMLER = PWMLER_EN4;
#endif

    pwm_duty = percent;
    g_speed = percent;
    pwm_service();
}

static void motor_set_direction(unsigned int dir)
{
    if (dir == DIR_FORWARD) {
        IO0CLR = IN2_PIN;
        IO0SET = IN1_PIN;
    } else if (dir == DIR_REVERSE) {
        IO0CLR = IN1_PIN;
        IO0SET = IN2_PIN;
    } else {
        IO0CLR = IN1_PIN | IN2_PIN;
    }
    g_direction = dir;
}

static void motor_ramp(unsigned int target, unsigned int ramp_ms)
{
    unsigned int now = g_speed;
    unsigned int diff = (target > now) ? (target - now) : (now - target);
    unsigned int step_ms;

    if (diff == 0U) {
        return;
    }

    step_ms = udiv(ramp_ms, diff);

    while (now != target) {
        if (target > now) {
            now++;
        } else {
            now--;
        }
        motor_set_speed(now);
        delay_ms(step_ms);
    }
}

/* Ramp down and pause before any change of direction */
static void motor_run_step(const motor_step_t *s)
{
    unsigned int target = (s->direction == DIR_STOP) ? 0U : s->speed;

    if ((s->direction != g_direction) && (g_speed > 0U)) {
        motor_ramp(0U, s->ramp_ms);
        motor_set_direction(DIR_STOP);
        delay_ms(DIR_CHANGE_PAUSE_MS);
    }

    if (s->direction != DIR_STOP) {
        motor_set_direction(s->direction);
        motor_ramp(target, s->ramp_ms);
    } else {
        motor_set_direction(DIR_STOP);
        motor_set_speed(0U);
    }

    delay_ms(s->run_ms);
}

int main(void)
{
    unsigned int i;

    PINSEL0 = (PINSEL0 & ~PINSEL0_MASK) | PINSEL0_VALUE;

    IO0SET = CS_PIN;
    IO0CLR = LED_PIN | EN1_PIN | IN1_PIN | IN2_PIN;
    IO0DIR |= LED_PIN | CS_PIN | EN1_PIN | IN1_PIN | IN2_PIN;

    VPBDIV = 0U;
    timers_init();
    pwm_init();
    motor_set_direction(DIR_STOP);

    for (i = 0; i < 3U; i++) {
        IO0SET = LED_PIN;
        delay_ms(100U);
        IO0CLR = LED_PIN;
        delay_ms(100U);
    }

    PCONP |= PCONP_PCSPI0;
    S0SPCCR = 8U;
    S0SPCR = SPCR_MSTR;

    g_fail_code = profile_load();
    g_step_count = step_count;

    /* EEPROM error: keep the motor off and blink the LED */
    if (g_fail_code != FAIL_NONE) {

        while (1) {
            IO0SET = LED_PIN;
            delay_ms(100U);
            IO0CLR = LED_PIN;
            delay_ms(100U);
        }
    }

    IO0SET = LED_PIN;

    while (1) {
        for (i = 0; i < step_count; i++) {
            g_step = i;
            motor_run_step(&steps[i]);
        }
        g_cycles++;
    }
}
