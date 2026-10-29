/* LPC2148 SPI EEPROM motor-profile controller
 * 12 MHz crystal, PLL off, PCLK 3 MHz
 * 25LC256: SCK P0.4, MISO P0.5, MOSI P0.6, CS P0.7
 * L293D: EN1 P0.8, IN1 P0.12, IN2 P0.13; LED P0.1
 * 16x2 LCD (4-bit): RS P1.16, E P1.17, D4-D7 P1.18-P1.21, RW to GND
 * Optional NEXT button: P0.15 to GND, 10k pull-up to 3.3 V
 * Six profiles, one per 64-byte EEPROM page, run in sequence.
 * All waits run service(); nothing blocks after start-up.
 */

#define FOSC_HZ         12000000U
#define EE_PAGE_SIZE    64U
#define DEAD_TIME_MS    200U
#define PROFILE_VERSION 3
#define SKIP_RAMP_MS    300U
#define DEBOUNCE_MS     20U

#define R(a) (*(volatile unsigned int *)(a))
#define PINSEL0 R(0xE002C000)
#define PINSEL2 R(0xE002C014)
#define IO0PIN  R(0xE0028000)
#define IO0SET  R(0xE0028004)
#define IO0DIR  R(0xE0028008)
#define IO0CLR  R(0xE002800C)
#define IO1SET  R(0xE0028014)
#define IO1DIR  R(0xE0028018)
#define IO1CLR  R(0xE002801C)
#define S0SPCR  R(0xE0020000)
#define S0SPSR  R(0xE0020004)
#define S0SPDR  R(0xE0020008)
#define S0SPCCR R(0xE002000C)
#define T1TCR   R(0xE0008004)
#define T1TC    R(0xE0008008)
#define T1PR    R(0xE000800C)
#define T1MCR   R(0xE0008014)
#define PLLCON  R(0xE01FC080)
#define PLLFEED R(0xE01FC08C)
#define PCONP   R(0xE01FC0C4)
#define VPBDIV  R(0xE01FC100)
#define SCS     R(0xE01FC1A0)

#define PCLK_HZ      (FOSC_HZ / 4U)
#define TICK_HZ      100000U             /* Timer1 tick = 10 us */
#define TICKS_PER_MS (TICK_HZ / 1000U)
#define PWM_PERIOD   1000U               /* 10 ms = 100 Hz */
#define TICKS_PER_PC (PWM_PERIOD / 100U)

#define LED (1U << 1)
#define CS  (1U << 7)
#define EN1 (1U << 8)
#define IN1 (1U << 12)
#define IN2 (1U << 13)
#define BTN (1U << 15)

#define LCD_RS    (1U << 16)
#define LCD_E     (1U << 17)
#define LCD_D4    18
#define LCD_PINS  (0x3FU << 16)
#define LCD_FRAME 34U                    /* 2 address commands + 32 characters */
#define LCD_GAP   5U                     /* 50 us between bytes (needs 37 us) */

#define PINSEL0_MASK 0xCF03FF0CU
#define PINSEL0_SPI  0x00001500U         /* P0.4-P0.6 = SPI0, P0.7 = GPIO */

#define EE_WRITE 0x02
#define EE_READ  0x03
#define EE_RDSR  0x05
#define EE_WREN  0x06
#define SR_WIP   0x01
#define SR_WEL   0x02
#define SR_BP    0x0C

/* LED patterns: 1 s frame, bit n = LED on during 100 ms slot n */
#define LED_FWD     0x01FU               /* slow blink */
#define LED_REV     0x005U               /* double flash */
#define LED_STOP    0x001U               /* single short flash */
#define LED_RAMP    0x155U               /* fast blink */
#define LED_SLOTS   10U
#define LED_SLOT_MS 100U

#define MAX_STEPS 8
#define NAME_LEN  6
#define HDR       (6 + NAME_LEN)         /* 'M' 'P' ver id n reps name */
#define NO_ADDR   (-1)

enum { STOP, FWD, REV };
enum { OK, E_SPI_TIMEOUT, E_SPI_FAULT, E_NO_EEPROM, E_PROTECTED, E_WEL, E_WRITE_TIMEOUT, E_PROFILE = 9 };
enum { PH_IDLE, PH_DECEL, PH_DEAD, PH_ACCEL, PH_HOLD };

typedef struct {
    unsigned char dir, speed;            /* speed in % */
    unsigned short ramp, run;            /* ms */
} step_t;

typedef struct {
    char name[NAME_LEN + 1];
    unsigned char reps, n;               /* repeats, steps */
    step_t s[MAX_STEPS];
} profile_t;

/* Defaults written to blank/invalid EEPROM pages */
static const profile_t defaults[] = {
    { "DEMO", 1, 4, {                    /* original test profile */
        { FWD, 75, 500, 3000 }, { STOP, 0, 300, 1000 },
        { REV, 40, 500, 3000 }, { STOP, 0, 300, 1000 } } },
    { "WASH", 3, 4, {                    /* washing machine agitation */
        { FWD, 60, 300, 2000 }, { STOP, 0, 300, 500 },
        { REV, 60, 300, 2000 }, { STOP, 0, 300, 500 } } },
    { "SPIN", 1, 3, {                    /* spin dry: slow start, full speed, long coast */
        { FWD, 40, 1000, 2000 }, { FWD, 100, 2000, 5000 },
        { STOP, 0, 3000, 2000 } } },
    { "GATE", 1, 4, {                    /* gate / garage door: open, wait, close */
        { FWD, 80, 800, 4000 }, { STOP, 0, 800, 3000 },
        { REV, 80, 800, 4000 }, { STOP, 0, 800, 2000 } } },
    { "FAN", 2, 4, {                     /* natural-breeze fan */
        { FWD, 30, 1000, 2000 }, { FWD, 60, 1000, 2000 },
        { FWD, 90, 1000, 2000 }, { FWD, 50, 1000, 2000 } } },
    { "MIXER", 3, 2, {                   /* blender pulse */
        { FWD, 100, 200, 700 }, { STOP, 0, 200, 500 } } }
};
#define NPROFILES (sizeof(defaults) / sizeof(defaults[0]))

static profile_t prof[NPROFILES];
static unsigned char page[EE_PAGE_SIZE];
static unsigned int duty, sched, pwm_start, ms_mark, last_driven, off_since, en1;
static unsigned int skip, btn_stable, btn_cnt;
static unsigned int led_auto, led_lit, led_ms, led_slot;
static unsigned int lcd_left, lcd_t;
static char lcd_buf[32];

volatile unsigned int g_fail_code, g_profile_source, g_profile, g_rep, g_step;
volatile unsigned int g_direction, g_speed, g_phase, g_cycles, g_time_ms, g_led_pattern;

/* ARM7 has no divide instruction */
static unsigned int udiv(unsigned int n, unsigned int d)
{
    unsigned int q = 0, bit = 1;
    if (d == 0) return 0;
    while (d < n && !(d & 0x80000000U)) { d <<= 1; bit <<= 1; }
    for (; bit; d >>= 1, bit >>= 1)
        if (n >= d) { n -= d; q |= bit; }
    return q;
}

/* ---------------- LCD ---------------- */

static void lcd_nibble(unsigned int rs, unsigned int v)
{
    volatile int i;
    IO1CLR = LCD_PINS;
    IO1SET = (rs ? LCD_RS : 0U) | ((v & 0xFU) << LCD_D4);
    IO1SET = LCD_E;
    for (i = 0; i < 3; i++) { }
    IO1CLR = LCD_E;
}

static void lcd_byte(unsigned int rs, unsigned int v)
{
    lcd_nibble(rs, v >> 4);
    lcd_nibble(rs, v);
}

/* Sends one byte of the pending frame; called from service() */
static void lcd_next(void)
{
    unsigned int p = LCD_FRAME - lcd_left--;
    if (p == 0) lcd_byte(0, 0x80);
    else if (p == 17) lcd_byte(0, 0xC0);
    else lcd_byte(1, (unsigned char)lcd_buf[p < 17 ? p - 1 : p - 2]);
    lcd_t = T1TC;
}

/* Queue a new screen; restarts any frame in progress */
static void lcd_show(const char *l1, const char *l2)
{
    unsigned int i;
    for (i = 0; i < 16; i++) { lcd_buf[i] = l1[i]; lcd_buf[i + 16] = l2[i]; }
    lcd_left = LCD_FRAME;
}

/* ---------------- Time base ---------------- */

static void led_update(void)
{
    unsigned int p, on;
    if (g_phase == PH_ACCEL || g_phase == PH_DECEL) p = LED_RAMP;
    else if (g_direction == FWD) p = LED_FWD;
    else if (g_direction == REV) p = LED_REV;
    else p = LED_STOP;
    if (p != g_led_pattern) { g_led_pattern = p; led_ms = 0; led_slot = 0; }
    on = (p >> led_slot) & 1U;
    if (on != led_lit) {
        if (on) IO0SET = LED; else IO0CLR = LED;
        led_lit = on;
    }
}

/* NEXT button: debounced press sets skip */
static void button(void)
{
    unsigned int b = (IO0PIN & BTN) ? 1U : 0U;
    if (b == btn_stable) btn_cnt = 0;
    else if (++btn_cnt >= DEBOUNCE_MS) {
        btn_stable = b;
        btn_cnt = 0;
        if (!b) skip = 1;
    }
}

/* Runs in every wait loop: ms counter, software PWM, LED, button, LCD */
static void service(void)
{
    unsigned int t = T1TC, on;
    while (t - ms_mark >= TICKS_PER_MS) {
        ms_mark += TICKS_PER_MS;
        g_time_ms++;
        if (++led_ms == LED_SLOT_MS) { led_ms = 0; if (++led_slot == LED_SLOTS) led_slot = 0; }
        if (led_auto) button();
    }
    while (t - pwm_start >= PWM_PERIOD) pwm_start += PWM_PERIOD;
    on = t - pwm_start < duty * TICKS_PER_PC;
    if (on != en1) {
        if (on) IO0SET = EN1; else IO0CLR = EN1;
        en1 = on;
    }
    if (led_auto) led_update();
    if (lcd_left && t - lcd_t >= LCD_GAP) lcd_next();
}

/* Returns 0 if a button press cut the wait short */
static unsigned int wait_until(unsigned int deadline)
{
    while ((int)(T1TC - deadline) < 0) {
        if (skip) return 0;
        service();
    }
    return 1;
}

/* Absolute scheduling: no drift */
static unsigned int wait_ms(unsigned int ms)
{
    sched += ms * TICKS_PER_MS;
    return wait_until(sched);
}

/* Start-up only: relative wait */
static void pause_ms(unsigned int ms)
{
    unsigned int t = T1TC + ms * TICKS_PER_MS;
    while ((int)(T1TC - t) < 0) service();
}

static void lcd_init(void)
{
    PINSEL2 &= ~(1U << 3);               /* P1.16-P1.25 as GPIO */
    IO1CLR = LCD_PINS;
    IO1DIR |= LCD_PINS;
    pause_ms(40);
    lcd_nibble(0, 3); pause_ms(5);
    lcd_nibble(0, 3); pause_ms(1);
    lcd_nibble(0, 3); pause_ms(1);
    lcd_nibble(0, 2); pause_ms(1);       /* 4-bit mode */
    lcd_byte(0, 0x28); pause_ms(1);      /* 2 lines, 5x8 */
    lcd_byte(0, 0x0C); pause_ms(1);      /* display on, no cursor */
    lcd_byte(0, 0x06); pause_ms(1);
    lcd_byte(0, 0x01); pause_ms(2);      /* clear */
}

/* Right-aligned decimal in w characters */
static void put_num(char *p, unsigned int w, unsigned int v)
{
    do {
        unsigned int q = udiv(v, 10);
        p[--w] = (char)('0' + v - q * 10);
        v = q;
    } while (v && w);
}

static void copy(char *d, const char *s)
{
    while (*s) *d++ = *s++;
}

/* Line 1: profile, direction, target %. Line 2: phase, step, repeat */
static void lcd_status(unsigned int target)
{
    static const char *const dir_txt[] = { "STOP", "FWD", "REV" };
    static const char *const ph_txt[] = { "IDLE", "DECEL", "DEAD", "ACCEL", "HOLD" };
    const profile_t *p = &prof[g_profile];
    char a[17] = "                ", b[17] = "      S /   R / ";
    copy(a, p->name);
    copy(a + 7, dir_txt[g_direction]);
    put_num(a + 11, 3, target);
    a[14] = '%';
    copy(b, g_direction == STOP && g_phase == PH_HOLD ? "OFF" : ph_txt[g_phase]);
    put_num(b + 7, 1, g_step + 1);
    put_num(b + 9, 1, p->n);
    put_num(b + 13, 1, g_rep + 1);
    put_num(b + 15, 1, p->reps);
    lcd_show(a, b);
}

static void blink(void)
{
    IO0SET = LED; wait_ms(100);
    IO0CLR = LED; wait_ms(100);
}

/* ---------------- SPI0 and 25LC256 ---------------- */

static void cs(int high)
{
    volatile int i;
    for (i = 0; i < 10; i++) { }
    if (high) IO0SET = CS; else IO0CLR = CS;
    for (i = 0; i < 10; i++) { }
}

/* SPIF cleared by reading S0SPSR then S0SPDR */
static unsigned int spi(unsigned char tx, unsigned char *rx)
{
    unsigned int n = 100000, s;
    S0SPDR = tx;
    while (!((s = S0SPSR) & 0x80)) {
        if (s & 0x18) return E_SPI_FAULT;
        if (--n == 0) return E_SPI_TIMEOUT;
    }
    s = S0SPDR;
    if (rx) *rx = (unsigned char)s;
    return OK;
}

static unsigned int ee(unsigned char cmd, int addr, const unsigned char *tx, unsigned char *rx, unsigned int n)
{
    unsigned int e, i;
    cs(0);
    e = spi(cmd, 0);
    if (addr != NO_ADDR) {
        if (!e) e = spi((unsigned char)((addr >> 8) & 0xFF), 0);
        if (!e) e = spi((unsigned char)(addr & 0xFF), 0);
    }
    for (i = 0; i < n && !e; i++) e = spi(tx ? tx[i] : 0, rx ? &rx[i] : 0);
    cs(1);
    return e;
}

/* Page-aware write: WREN, check WEL, WRITE, poll WIP */
static unsigned int ee_write(unsigned int addr, const unsigned char *buf, unsigned int len)
{
    unsigned int e, n, polls;
    unsigned char st = 0;
    for (; len; addr += n, buf += n, len -= n) {
        n = EE_PAGE_SIZE - (addr % EE_PAGE_SIZE);
        if (n > len) n = len;
        if ((e = ee(EE_WREN, NO_ADDR, 0, 0, 0)) || (e = ee(EE_RDSR, NO_ADDR, 0, &st, 1))) return e;
        if (!(st & SR_WEL)) return E_WEL;
        if ((e = ee(EE_WRITE, (int)addr, buf, 0, n))) return e;
        for (polls = 0; ; polls++) {
            if (polls == 20000) return E_WRITE_TIMEOUT;
            if ((e = ee(EE_RDSR, NO_ADDR, 0, &st, 1))) return e;
            if (!(st & SR_WIP)) break;
        }
    }
    return OK;
}

/* ---------------- Profile ---------------- */

static unsigned char sum(const unsigned char *b, unsigned int n)
{
    unsigned char s = 0;
    while (n--) s += *b++;
    return s;
}

/* Page k: 'M' 'P' ver k n reps name[6], 6 bytes per step, checksum */
static unsigned int encode(unsigned int k, unsigned char *b)
{
    const profile_t *p = &defaults[k];
    unsigned int i, n = HDR;
    b[0] = 'M'; b[1] = 'P'; b[2] = PROFILE_VERSION; b[3] = (unsigned char)k;
    b[4] = p->n; b[5] = p->reps;
    for (i = 0; i < NAME_LEN; i++) b[6 + i] = (unsigned char)p->name[i];
    for (i = 0; i < p->n; i++) {
        const step_t *s = &p->s[i];
        b[n++] = s->dir;
        b[n++] = s->speed;
        b[n++] = (unsigned char)(s->ramp >> 8); b[n++] = (unsigned char)s->ramp;
        b[n++] = (unsigned char)(s->run >> 8);  b[n++] = (unsigned char)s->run;
    }
    b[n] = (unsigned char)(0 - sum(b, n));
    return n + 1;
}

static unsigned int decode(unsigned int k, const unsigned char *b)
{
    profile_t *p = &prof[k];
    unsigned int i, n = b[4];
    if (b[0] != 'M' || b[1] != 'P' || b[2] != PROFILE_VERSION || b[3] != k) return 0;
    if (n == 0 || n > MAX_STEPS || b[5] == 0 || b[5] > 9 || sum(b, HDR + n * 6 + 1) != 0) return 0;
    for (i = 0; i < n; i++) {
        const unsigned char *q = b + HDR + i * 6;
        if (q[0] > REV || q[1] > 100) return 0;
        p->s[i].dir = q[0];
        p->s[i].speed = q[1];
        p->s[i].ramp = (unsigned short)(q[2] << 8 | q[3]);
        p->s[i].run = (unsigned short)(q[4] << 8 | q[5]);
    }
    for (i = 0; i < NAME_LEN; i++) p->name[i] = (char)b[6 + i];
    p->name[NAME_LEN] = 0;
    p->n = (unsigned char)n;
    p->reps = b[5];
    return 1;
}

/* Each blank or invalid page gets its default and is read back */
static unsigned int load(void)
{
    unsigned char st = 0;
    unsigned int e, k, addr;
    if ((e = ee(EE_RDSR, NO_ADDR, 0, &st, 1))) return e;
    if (st == 0xFF) return E_NO_EEPROM;
    if ((st & SR_BP) == SR_BP) return E_PROTECTED;
    g_profile_source = 1;
    for (k = 0; k < NPROFILES; k++) {
        addr = k * EE_PAGE_SIZE;
        if ((e = ee(EE_READ, (int)addr, 0, page, EE_PAGE_SIZE))) return e;
        if (decode(k, page)) continue;
        if ((e = ee_write(addr, page, encode(k, page))) || (e = ee(EE_READ, (int)addr, 0, page, EE_PAGE_SIZE)))
            return e;
        if (!decode(k, page)) return E_PROFILE;
        g_profile_source = 2;
    }
    return OK;
}

/* ---------------- Motor ---------------- */

static void set_duty(unsigned int p)
{
    duty = p > 100 ? 100 : p;
    g_speed = duty;
    service();
}

/* IN1 and IN2 never high together */
static void set_dir(unsigned int d)
{
    IO0CLR = IN1 | IN2;
    if (d == FWD) IO0SET = IN1;
    else if (d == REV) IO0SET = IN2;
    if (d == STOP && g_direction != STOP) { last_driven = g_direction; off_since = sched; }
    g_direction = d;
}

/* Linear ramp, exactly ms long; returns 0 if skipped */
static unsigned int ramp_to(unsigned int target, unsigned int ms)
{
    unsigned int from = duty, n, k, base = sched, total = ms * TICKS_PER_MS;
    n = from > target ? from - target : target - from;
    if (n == 0) return 1;
    for (k = 1; k <= n; k++) {
        if (!wait_until(base + udiv(total * k, n))) return 0;
        set_duty(from < target ? from + k : from - k);
    }
    sched = base + total;
    return 1;
}

static void run_step(const step_t *s)
{
    unsigned int ready;

    if (g_direction != STOP && s->dir != g_direction) {
        g_phase = PH_DECEL;
        lcd_status(0);
        if (!ramp_to(0, s->ramp)) return;
        set_dir(STOP);
    }
    if (s->dir != STOP) {
        if (g_direction != s->dir) {
            ready = off_since + DEAD_TIME_MS * TICKS_PER_MS;
            if (last_driven != STOP && last_driven != s->dir && (int)(ready - sched) > 0) {
                g_phase = PH_DEAD;
                lcd_status(0);
                sched = ready;
                if (!wait_until(sched)) return;
            }
            set_dir(s->dir);
        }
        g_phase = PH_ACCEL;
        lcd_status(s->speed);
        if (!ramp_to(s->speed, s->ramp)) return;
    }
    g_phase = PH_HOLD;
    lcd_status(duty);
    wait_ms(s->run);
}

static void run_profile(const profile_t *p)
{
    for (g_rep = 0; g_rep < p->reps && !skip; g_rep++)
        for (g_step = 0; g_step < p->n; g_step++) {
            run_step(&p->s[g_step]);
            if (skip) return;
        }
}

/* Button pressed: ramp down, stop, then the next profile starts */
static void skip_profile(void)
{
    skip = 0;
    sched = T1TC;
    g_phase = PH_DECEL;
    lcd_status(0);
    ramp_to(0, SKIP_RAMP_MS);
    set_duty(0);
    sched = T1TC;
    set_dir(STOP);
}

int main(void)
{
    unsigned int i;

    /* PLL off in case startup code enabled it */
    PLLCON = 0;
    PLLFEED = 0xAA;
    PLLFEED = 0x55;
    VPBDIV = 0;
    SCS = 0;

    PINSEL0 = (PINSEL0 & ~PINSEL0_MASK) | PINSEL0_SPI;
    IO0SET = CS;
    IO0CLR = LED | EN1 | IN1 | IN2;
    IO0DIR |= LED | CS | EN1 | IN1 | IN2;
    PCONP |= (1U << 2) | (1U << 8);      /* Timer1, SPI0 */

    T1TCR = 2;
    T1PR = PCLK_HZ / TICK_HZ - 1;        /* 29 -> 10 us */
    T1MCR = 0;
    T1TCR = 1;

    pwm_start = ms_mark = T1TC;
    last_driven = STOP;
    set_dir(STOP);
    lcd_init();
    lcd_show("MOTOR PROFILES  ", "Loading EEPROM  ");

    sched = T1TC;
    for (i = 0; i < 3; i++) blink();

    S0SPCCR = 8;                         /* 375 kHz */
    S0SPCR = 0x20;                       /* master, mode 0 */
    g_fail_code = load();

    if (g_fail_code) {                   /* LED solid on, motor off */
        char e[17] = "Fail code       ";
        put_num(e + 10, 1, g_fail_code);
        lcd_show("EEPROM ERROR    ", e);
        IO0SET = LED;
        for (;;) service();
    }

    btn_stable = (IO0PIN & BTN) ? 1U : 0U;
    led_auto = 1;
    sched = T1TC;
    for (;;) {
        run_profile(&prof[g_profile]);
        if (skip) skip_profile();
        if (++g_profile == NPROFILES) { g_profile = 0; g_cycles++; }
    }
}
