/* LPC2138/LPC2148 SPI EEPROM motor-profile controller
 * 25LC256: SCK P0.4, MISO P0.5, MOSI P0.6, CS P0.7
 * L293D  : EN1 P0.8, IN1 P0.12, IN2 P0.13
 * LED P0.1 | NEXT button P0.15 (optional) | LCD: RS P1.16, E P1.17, D4-D7 P1.18-P1.21
 */
#define FOSC_HZ 12000000U
#define REG(a)  (*(volatile unsigned int *)(a))
#define PINSEL0 REG(0xE002C000)
#define PINSEL2 REG(0xE002C014)
#define IO0PIN  REG(0xE0028000)
#define IO0SET  REG(0xE0028004)
#define IO0DIR  REG(0xE0028008)
#define IO0CLR  REG(0xE002800C)
#define IO1SET  REG(0xE0028014)
#define IO1DIR  REG(0xE0028018)
#define IO1CLR  REG(0xE002801C)
#define S0SPCR  REG(0xE0020000)
#define S0SPSR  REG(0xE0020004)
#define S0SPDR  REG(0xE0020008)
#define S0SPCCR REG(0xE002000C)
#define T1TCR   REG(0xE0008004)
#define T1TC    REG(0xE0008008)
#define T1PR    REG(0xE000800C)
#define PLLCON  REG(0xE01FC080)
#define PLLFEED REG(0xE01FC08C)
#define PCONP   REG(0xE01FC0C4)
#define VPBDIV  REG(0xE01FC100)
#define SCS     REG(0xE01FC1A0)

#define LED (1U << 1)
#define CS  (1U << 7)
#define EN1 (1U << 8)
#define IN1 (1U << 12)
#define IN2 (1U << 13)
#define BUTTON   (1U << 15)
#define LCD_PINS (0x3FU << 16)           /* RS P1.16, E P1.17, D4-D7 P1.18-21 */
#define TICKS_PER_MS 100U                /* Timer1 tick = 10 us */
#define PWM_PERIOD   (10 * TICKS_PER_MS)
#define DEAD_TIME    (200 * TICKS_PER_MS)
#define HEADER 12                        /* 'M' 'P' version id steps repeats name[6] */

enum { STOP, FWD, REV };
enum { IDLE, DECEL, DEAD, ACCEL, HOLD };

typedef struct { unsigned char dir, speed; unsigned short ramp_ms, run_ms; } step_t;
typedef struct { char name[7]; unsigned char repeats, steps; step_t step[8]; } profile_t;

static const profile_t defaults[] = {
    { "DEMO",  1, 4, { {FWD, 75, 500, 3000}, {STOP, 0, 300, 1000},
                       {REV, 40, 500, 3000}, {STOP, 0, 300, 1000} } },
    { "WASH",  3, 4, { {FWD, 60, 300, 2000}, {STOP, 0, 300, 500},
                       {REV, 60, 300, 2000}, {STOP, 0, 300, 500} } },
    { "SPIN",  1, 3, { {FWD, 40, 1000, 2000}, {FWD, 100, 2000, 5000},
                       {STOP, 0, 3000, 2000} } },
    { "GATE",  1, 4, { {FWD, 80, 800, 4000}, {STOP, 0, 800, 3000},
                       {REV, 80, 800, 4000}, {STOP, 0, 800, 2000} } },
    { "FAN",   2, 4, { {FWD, 30, 1000, 2000}, {FWD, 60, 1000, 2000},
                       {FWD, 90, 1000, 2000}, {FWD, 50, 1000, 2000} } },
    { "MIXER", 3, 2, { {FWD, 100, 200, 700}, {STOP, 0, 200, 500} } }
};
#define NUM_PROFILES (sizeof(defaults) / sizeof(defaults[0]))

static profile_t profiles[NUM_PROFILES];
static unsigned char page[64];
static char screen[32];
static unsigned int now, duty, pwm_start, ms_mark, last_dir, off_since, running, skip;
static unsigned int button_state, button_count, led_ms, led_slot, led_pattern;
static unsigned int lcd_left, lcd_time;
volatile unsigned int g_fail_code, g_profile, g_repeat, g_step;
volatile unsigned int g_direction, g_speed, g_phase, g_cycles;

static unsigned int divide(unsigned int n, unsigned int d)   /* ARM7 has no divide */
{
    unsigned int q = 0, bit = 1;
    while (d < n && !(d & 0x80000000U)) { d <<= 1; bit <<= 1; }
    for (; bit; d >>= 1, bit >>= 1) if (n >= d) { n -= d; q |= bit; }
    return q;
}

static void set_pin(unsigned int mask, int on) { if (on) IO0SET = mask; else IO0CLR = mask; }

/* ---------------- LCD ---------------- */
static void lcd_nibble(unsigned int rs, unsigned int value)
{
    volatile int i;
    IO1CLR = LCD_PINS;
    IO1SET = (rs << 16) | ((value & 0xFU) << 18) | (1U << 17);   /* data + E high */
    for (i = 0; i < 3; i++) { }
    IO1CLR = 1U << 17;                                           /* E low latches */
}
static void lcd_byte(unsigned int rs, unsigned int value)
{
    lcd_nibble(rs, value >> 4);
    lcd_nibble(rs, value);
}

static void lcd_show(const char *line1, const char *line2)
{
    unsigned int i;
    for (i = 0; i < 16; i++) { screen[i] = line1[i]; screen[16 + i] = line2[i]; }
    lcd_left = 34;                       /* 2 address commands + 32 characters */
}

/* ---------------- Background service (runs in every wait) ---------------- */
static void service(void)
{
    static const unsigned short pattern[] = { 0x001, 0x01F, 0x005, 0x155 }; /* stop fwd rev ramp */
    unsigned int t = T1TC, p;

    while (t - ms_mark >= TICKS_PER_MS) {                        /* every 1 ms */
        ms_mark += TICKS_PER_MS;
        if (++led_ms == 100) { led_ms = 0; if (++led_slot == 10) led_slot = 0; }
        p = (IO0PIN & BUTTON) ? 1U : 0U;
        if (!running || p == button_state) button_count = 0;
        else if (++button_count >= 20) {                         /* 20 ms debounce */
            button_state = p;
            button_count = 0;
            if (!p) skip = 1;
        }
    }
    while (t - pwm_start >= PWM_PERIOD) pwm_start += PWM_PERIOD;
    set_pin(EN1, t - pwm_start < duty * (PWM_PERIOD / 100));    /* software PWM */

    if (running) {                                               /* LED pattern */
        p = (g_phase == ACCEL || g_phase == DECEL) ? 3 : g_direction;
        if (p + 1 != led_pattern) { led_pattern = p + 1; led_ms = led_slot = 0; }
        set_pin(LED, (pattern[p] >> led_slot) & 1);
    }
    if (lcd_left && t - lcd_time >= 5) {                         /* one LCD byte per 50 us */
        p = 34 - lcd_left--;
        if (p == 0 || p == 17) lcd_byte(0, p ? 0xC0 : 0x80);
        else lcd_byte(1, (unsigned char)screen[p < 17 ? p - 1 : p - 2]);
        lcd_time = T1TC;
    }
}

static int wait_until(unsigned int time)                         /* 0 if NEXT pressed */
{
    while ((int)(T1TC - time) < 0) { if (skip) return 0; service(); }
    return 1;
}
static void pause_ms(unsigned int ms)
{
    unsigned int end = T1TC + ms * TICKS_PER_MS;
    while ((int)(T1TC - end) < 0) service();
}

/* ---------------- LCD text ---------------- */
static void put_text(char *dst, const char *src) { while (*src) *dst++ = *src++; }
static void put_number(char *dst, unsigned int width, unsigned int value)
{
    do {
        unsigned int q = divide(value, 10);
        dst[--width] = (char)('0' + value - q * 10);
        value = q;
    } while (value && width);
}

/* Line 1 "WASH   FWD  60%", line 2 "HOLD  S1/4  R2/3" */
static void show_status(unsigned int target)
{
    static const char *const dir_text[] = { "STOP", "FWD", "REV" };
    static const char *const phase_text[] = { "IDLE", "DECEL", "DEAD", "ACCEL", "HOLD" };
    const profile_t *p = &profiles[g_profile];
    char line1[17] = "                ", line2[17] = "      S /   R / ";
    put_text(line1, p->name);
    put_text(line1 + 7, dir_text[g_direction]);
    put_number(line1 + 11, 3, target);
    line1[14] = '%';
    put_text(line2, (g_direction == STOP && g_phase == HOLD) ? "OFF" : phase_text[g_phase]);
    put_number(line2 + 7, 1, g_step + 1);
    put_number(line2 + 9, 1, p->steps);
    put_number(line2 + 13, 1, g_repeat + 1);
    put_number(line2 + 15, 1, p->repeats);
    lcd_show(line1, line2);
}

/* ---------------- SPI and 25LC256 EEPROM ---------------- */
static unsigned int spi_transfer(unsigned int tx, unsigned char *rx)
{
    unsigned int timeout = 100000, status;
    S0SPDR = tx;
    while (!((status = S0SPSR) & 0x80)) {                        /* wait for SPIF */
        if (status & 0x18) return 2;                             /* mode fault / abort */
        if (--timeout == 0) return 1;
    }
    status = S0SPDR;                     /* reading SPDR clears SPIF */
    if (rx) *rx = (unsigned char)status;
    return 0;
}

static unsigned int eeprom(unsigned int cmd, int addr,
                           const unsigned char *tx, unsigned char *rx, unsigned int n)
{
    unsigned int err, i;
    volatile int d;
    for (d = 0; d < 10; d++) { }
    IO0CLR = CS;
    err = spi_transfer(cmd, 0);
    if (addr >= 0 && !err) err = spi_transfer((unsigned int)addr >> 8, 0);
    if (addr >= 0 && !err) err = spi_transfer((unsigned int)addr & 0xFF, 0);
    for (i = 0; i < n && !err; i++) err = spi_transfer(tx ? tx[i] : 0, rx ? &rx[i] : 0);
    IO0SET = CS;
    return err;
}

static unsigned int eeprom_write_page(unsigned int addr, unsigned int n)
{
    unsigned int err, polls;
    unsigned char status = 0;
    if ((err = eeprom(0x06, -1, 0, 0, 0))) return err;           /* WREN */
    if ((err = eeprom(0x05, -1, 0, &status, 1))) return err;     /* RDSR */
    if (!(status & 0x02)) return 5;                              /* WEL not set */
    if ((err = eeprom(0x02, (int)addr, page, 0, n))) return err; /* WRITE */
    for (polls = 0; polls < 20000; polls++) {                    /* wait while WIP */
        if ((err = eeprom(0x05, -1, 0, &status, 1))) return err;
        if (!(status & 0x01)) return 0;
    }
    return 6;
}

/* ---------------- Profile storage (one profile per 64-byte page) ---------------- */
static unsigned char checksum(const unsigned char *b, unsigned int n)
{
    unsigned char s = 0;
    while (n--) s += *b++;
    return s;
}

static unsigned int encode(unsigned int k)
{
    const profile_t *p = &defaults[k];
    unsigned int i, n = HEADER;
    page[0] = 'M'; page[1] = 'P'; page[2] = 3; page[3] = (unsigned char)k;
    page[4] = p->steps; page[5] = p->repeats;
    for (i = 0; i < 6; i++) page[6 + i] = (unsigned char)p->name[i];
    for (i = 0; i < p->steps; i++, n += 6) {
        const step_t *s = &p->step[i];
        page[n] = s->dir; page[n + 1] = s->speed;
        page[n + 2] = (unsigned char)(s->ramp_ms >> 8); page[n + 3] = (unsigned char)s->ramp_ms;
        page[n + 4] = (unsigned char)(s->run_ms >> 8);  page[n + 5] = (unsigned char)s->run_ms;
    }
    page[n] = (unsigned char)(0 - checksum(page, n));            /* page sums to 0 */
    return n + 1;
}

static int decode(unsigned int k)
{
    profile_t *p = &profiles[k];
    unsigned int i, steps = page[4], repeats = page[5];
    if (page[0] != 'M' || page[1] != 'P' || page[2] != 3 || page[3] != k) return 0;
    if (steps < 1 || steps > 8 || repeats < 1 || repeats > 9) return 0;
    if (checksum(page, HEADER + 6 * steps + 1) != 0) return 0;
    for (i = 0; i < steps; i++) {
        const unsigned char *b = page + HEADER + 6 * i;
        if (b[0] > REV || b[1] > 100) return 0;
        p->step[i].dir = b[0];
        p->step[i].speed = b[1];
        p->step[i].ramp_ms = (unsigned short)(b[2] << 8 | b[3]);
        p->step[i].run_ms = (unsigned short)(b[4] << 8 | b[5]);
    }
    for (i = 0; i < 6; i++) p->name[i] = (char)page[6 + i];
    p->name[6] = 0; p->steps = (unsigned char)steps; p->repeats = (unsigned char)repeats;
    return 1;
}

/* Bad or blank page: write the default, read back, check again */
static unsigned int load_profiles(void)
{
    unsigned int err, k;
    unsigned char status = 0;
    if ((err = eeprom(0x05, -1, 0, &status, 1))) return err;
    if (status == 0xFF) return 3;                                /* no EEPROM */
    if ((status & 0x0C) == 0x0C) return 4;                       /* write-protected */
    for (k = 0; k < NUM_PROFILES; k++) {
        if ((err = eeprom(0x03, (int)(k * 64), 0, page, 64))) return err;
        if (decode(k)) continue;
        if ((err = eeprom_write_page(k * 64, encode(k)))) return err;
        if ((err = eeprom(0x03, (int)(k * 64), 0, page, 64))) return err;
        if (!decode(k)) return 9;
    }
    return 0;
}

/* ---------------- Motor control ---------------- */
static void set_speed(unsigned int percent) { duty = g_speed = percent; service(); }

/* IN1 and IN2 are never high together */
static void set_direction(unsigned int dir)
{
    IO0CLR = IN1 | IN2;
    if (dir == FWD) IO0SET = IN1;
    if (dir == REV) IO0SET = IN2;
    if (dir == STOP && g_direction != STOP) { last_dir = g_direction; off_since = now; }
    g_direction = dir;
}

static int ramp_to(unsigned int target, unsigned int ms)        /* linear, exactly ms long */
{
    unsigned int from = duty, start = now, k;
    unsigned int steps = from > target ? from - target : target - from;
    for (k = 1; k <= steps; k++) {
        if (!wait_until(start + divide(ms * TICKS_PER_MS * k, steps))) return 0;
        set_speed(from < target ? from + k : from - k);
    }
    if (steps) now = start + ms * TICKS_PER_MS;
    return 1;
}

static void set_phase(unsigned int phase, unsigned int target)
{
    g_phase = phase;
    show_status(target);
}

static void run_step(const step_t *s)
{
    if (g_direction != STOP && s->dir != g_direction) {          /* slow down first */
        set_phase(DECEL, 0);
        if (!ramp_to(0, s->ramp_ms)) return;
        set_direction(STOP);
    }
    if (s->dir != STOP && s->dir != g_direction) {
        if (last_dir != STOP && last_dir != s->dir && (int)(off_since + DEAD_TIME - now) > 0) {
            set_phase(DEAD, 0);                                  /* 200 ms off before reversing */
            now = off_since + DEAD_TIME;
            if (!wait_until(now)) return;
        }
        set_direction(s->dir);
    }
    if (s->dir != STOP) {
        set_phase(ACCEL, s->speed);
        if (!ramp_to(s->speed, s->ramp_ms)) return;
    }
    set_phase(HOLD, duty);
    now += s->run_ms * TICKS_PER_MS;
    wait_until(now);
}

static void run_profile(const profile_t *p)
{
    for (g_repeat = 0; g_repeat < p->repeats && !skip; g_repeat++)
        for (g_step = 0; g_step < p->steps; g_step++) {
            run_step(&p->step[g_step]);
            if (skip) return;
        }
}

/* ---------------- Main ---------------- */
int main(void)
{
    unsigned int i;

    PLLCON = 0; PLLFEED = 0xAA; PLLFEED = 0x55;                  /* PLL off: CPU = FOSC_HZ */
    VPBDIV = 0; SCS = 0;                                         /* PCLK = CPU / 4 */
    PINSEL0 = (PINSEL0 & ~0xCF03FF0CU) | 0x1500U;                /* SPI0 on P0.4-P0.6 */
    PINSEL2 &= ~(1U << 3);
    IO0SET = CS; IO0CLR = LED | EN1 | IN1 | IN2; IO0DIR |= LED | CS | EN1 | IN1 | IN2;
    IO1CLR = LCD_PINS; IO1DIR |= LCD_PINS;
    PCONP |= (1U << 2) | (1U << 8);                              /* Timer1, SPI0 */
    T1TCR = 2; T1PR = FOSC_HZ / 4 / (1000 * TICKS_PER_MS) - 1; T1TCR = 1;
    pwm_start = ms_mark = T1TC;
    S0SPCCR = 8; S0SPCR = 0x20;                                  /* 375 kHz, master, mode 0 */

    pause_ms(40);                                                /* LCD 4-bit init */
    for (i = 0; i < 4; i++) { lcd_nibble(0, i < 3 ? 3 : 2); pause_ms(i ? 1 : 5); }
    lcd_byte(0, 0x28); pause_ms(1);                              /* 2 lines */
    lcd_byte(0, 0x0C); pause_ms(1);                              /* display on */
    lcd_byte(0, 0x06); pause_ms(1);
    lcd_byte(0, 0x01); pause_ms(2);                              /* clear */
    lcd_show("MOTOR PROFILES  ", "Loading EEPROM  ");
    for (i = 0; i < 3; i++) { set_pin(LED, 1); pause_ms(100); set_pin(LED, 0); pause_ms(100); }

    if ((g_fail_code = load_profiles()) != 0) {                  /* fault: LED on, motor off */
        char line2[17] = "Fail code       ";
        put_number(line2 + 10, 1, g_fail_code);
        lcd_show("EEPROM ERROR    ", line2);
        set_pin(LED, 1);
        for (;;) service();
    }

    button_state = (IO0PIN & BUTTON) ? 1U : 0U;
    running = 1;
    now = T1TC;
    for (;;) {
        run_profile(&profiles[g_profile]);
        if (skip) {                                              /* NEXT: ramp down, stop */
            skip = 0; now = T1TC; set_phase(DECEL, 0);
            ramp_to(0, 300); set_speed(0); now = T1TC; set_direction(STOP);
        }
        if (++g_profile == NUM_PROFILES) { g_profile = 0; g_cycles++; }
    }
}
