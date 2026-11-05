/*
 * SPI EEPROM Motor-Profile Controller (LPC2138 / LPC2148)
 *
 * EEPROM 25LC256 : SCK P0.4, MISO P0.5, MOSI P0.6, CS P0.7
 * L293D          : EN1 P0.8, IN1 P0.12, IN2 P0.13
 * LED P0.1, NEXT button P0.15
 * LCD            : RS P1.16, E P1.17, D4-D7 P1.18-P1.21
 */

#define PINSEL0 (*(volatile unsigned int *)0xE002C000)
#define PINSEL2 (*(volatile unsigned int *)0xE002C014)
#define IO0PIN  (*(volatile unsigned int *)0xE0028000)
#define IO0SET  (*(volatile unsigned int *)0xE0028004)
#define IO0DIR  (*(volatile unsigned int *)0xE0028008)
#define IO0CLR  (*(volatile unsigned int *)0xE002800C)
#define IO1SET  (*(volatile unsigned int *)0xE0028014)
#define IO1DIR  (*(volatile unsigned int *)0xE0028018)
#define IO1CLR  (*(volatile unsigned int *)0xE002801C)
#define S0SPCR  (*(volatile unsigned int *)0xE0020000)
#define S0SPSR  (*(volatile unsigned int *)0xE0020004)
#define S0SPDR  (*(volatile unsigned int *)0xE0020008)
#define S0SPCCR (*(volatile unsigned int *)0xE002000C)
#define T1TCR   (*(volatile unsigned int *)0xE0008004)
#define T1TC    (*(volatile unsigned int *)0xE0008008)
#define T1PR    (*(volatile unsigned int *)0xE000800C)
#define PLLCON  (*(volatile unsigned int *)0xE01FC080)
#define PLLFEED (*(volatile unsigned int *)0xE01FC08C)
#define PCONP   (*(volatile unsigned int *)0xE01FC0C4)
#define VPBDIV  (*(volatile unsigned int *)0xE01FC100)
#define SCS     (*(volatile unsigned int *)0xE01FC1A0)

#define FOSC  12000000
#define MS    100           /* timer ticks every 10 us, so 100 ticks = 1 ms */

#define LED   (1 << 1)
#define CS    (1 << 7)
#define EN1   (1 << 8)
#define IN1   (1 << 12)
#define IN2   (1 << 13)

#define STOP  0
#define FWD   1
#define REV   2

#define DECEL 1
#define DEAD  2
#define ACCEL 3
#define HOLD  4

/* Default profiles: each step has direction, speed %, ramp ms, run ms */
const char def_name[6][7] = { "DEMO", "WASH", "SPIN", "GATE", "FAN", "MIXER" };
const unsigned int def_reps[6]  = { 1, 3, 1, 1, 2, 3 };
const unsigned int def_steps[6] = { 4, 4, 3, 4, 4, 2 };

const unsigned int def_dir[6][4] = {
    {FWD, STOP, REV,  STOP},
    {FWD, STOP, REV,  STOP},
    {FWD, FWD,  STOP, 0},
    {FWD, STOP, REV,  STOP},
    {FWD, FWD,  FWD,  FWD},
    {FWD, STOP, 0,    0}
};

const unsigned int def_speed[6][4] = {
    {75,  0,   40, 0},
    {60,  0,   60, 0},
    {40,  100, 0,  0},
    {80,  0,   80, 0},
    {30,  60,  90, 50},
    {100, 0,   0,  0}
};

const unsigned int def_ramp[6][4] = {
    {500,  300,  500,  300},
    {300,  300,  300,  300},
    {1000, 2000, 3000, 0},
    {800,  800,  800,  800},
    {1000, 1000, 1000, 1000},
    {200,  200,  0,    0}
};

const unsigned int def_run[6][4] = {
    {3000, 1000, 3000, 1000},
    {2000, 500,  2000, 500},
    {2000, 5000, 2000, 0},
    {4000, 3000, 4000, 2000},
    {2000, 2000, 2000, 2000},
    {700,  500,  0,    0}
};

const char dir_text[3][5]   = { "STOP", "FWD", "REV" };
const char phase_text[5][6] = { "IDLE", "DECEL", "DEAD", "ACCEL", "HOLD" };
const unsigned int pattern[4] = { 0x001, 0x01F, 0x005, 0x155 };   /* LED: stop, fwd, rev, ramp */

/* Profiles loaded from the EEPROM */
char name[6][7];
unsigned int reps[6], nsteps[6];
unsigned int dir[6][8], speed[6][8], ramp[6][8], run[6][8];

unsigned char buf[64];      /* one EEPROM page */
char lcd[32];               /* LCD text, 2 lines x 16 */

unsigned int now, duty, pwm_start, ms_mark;
unsigned int last_dir, off_since;
unsigned int running, skip, btn, btn_count;
unsigned int led_ms, led_slot, led_pat;
unsigned int lcd_left, lcd_time;
unsigned int spi_err, fail;
unsigned int profile, rep, step, cur_dir, phase, cycles;

void pin(unsigned int mask, int on) {
    if (on)
        IO0SET = mask;
    else
        IO0CLR = mask;
}

int reached(unsigned int t) {
    return (int)(T1TC - t) >= 0;    /* safe across timer overflow */
}

/* ---------------- LCD (4-bit) ---------------- */

void lcd_nibble(int rs, int v) {
    unsigned int t;

    IO1CLR = 0x3F << 16;
    IO1SET = (rs << 16) | ((v & 15) << 18) | (1 << 17);    /* data out, E high */
    t = T1TC;
    while (T1TC - t < 2);
    IO1CLR = 1 << 17;                                      /* E low, LCD latches */
}

void lcd_byte(int rs, int v) {
    lcd_nibble(rs, v >> 4);
    lcd_nibble(rs, v);
}

void lcd_clear(void) {
    int i;

    for (i = 0; i < 32; i++)
        lcd[i] = ' ';
}

void lcd_text(int pos, const char s[]) {
    int i;

    for (i = 0; s[i] != 0; i++)
        lcd[pos + i] = s[i];
}

void lcd_num(int pos, int w, unsigned int v) {   /* right-aligned number */
    do {
        w--;
        lcd[pos + w] = '0' + v % 10;
        v = v / 10;
    } while (v > 0 && w > 0);
}

/* ---------------- Background jobs (called inside every wait) ---------------- */

void service(void) {
    unsigned int t = T1TC;
    unsigned int p;

    while (t - ms_mark >= MS) {             /* 1 ms tick */
        ms_mark += MS;

        if (++led_ms == 100) {
            led_ms = 0;
            if (++led_slot == 10)
                led_slot = 0;
        }

        p = (IO0PIN >> 15) & 1;             /* button is active low, 20 ms debounce */
        if (running == 0 || p == btn) {
            btn_count = 0;
        } else if (++btn_count >= 20) {
            btn = p;
            btn_count = 0;
            if (p == 0)
                skip = 1;
        }
    }

    while (t - pwm_start >= 10 * MS)
        pwm_start += 10 * MS;
    pin(EN1, t - pwm_start < duty * 10);    /* software PWM, 10 ms period */

    if (running) {
        if (phase == ACCEL || phase == DECEL)
            p = 3;
        else
            p = cur_dir;

        if (p + 1 != led_pat) {
            led_pat = p + 1;
            led_ms = 0;
            led_slot = 0;
        }
        pin(LED, (pattern[p] >> led_slot) & 1);
    }

    if (lcd_left > 0 && t - lcd_time >= 5) {   /* one LCD byte every 50 us */
        p = 34 - lcd_left;
        lcd_left--;

        if (p == 0)
            lcd_byte(0, 0x80);
        else if (p == 17)
            lcd_byte(0, 0xC0);
        else if (p < 17)
            lcd_byte(1, lcd[p - 1]);
        else
            lcd_byte(1, lcd[p - 2]);

        lcd_time = T1TC;
    }
}

int wait_until(unsigned int t) {            /* returns 0 if NEXT was pressed */
    while (!reached(t)) {
        if (skip)
            return 0;
        service();
    }
    return 1;
}

void delay_ms(unsigned int ms) {
    unsigned int t = T1TC + ms * MS;

    while (!reached(t))
        service();
}

void show_status(unsigned int target) {     /* e.g. "WASH FWD 60%" */
    lcd_clear();
    lcd_text(0, name[profile]);
    lcd_text(7, dir_text[cur_dir]);
    lcd_num(11, 3, target);
    lcd_text(14, "%");

    if (cur_dir == STOP && phase == HOLD)
        lcd_text(16, "OFF");
    else
        lcd_text(16, phase_text[phase]);

    lcd_text(22, "S /   R /");
    lcd_num(23, 1, step + 1);
    lcd_num(25, 1, nsteps[profile]);
    lcd_num(29, 1, rep + 1);
    lcd_num(31, 1, reps[profile]);
    lcd_left = 34;
}

/* ---------------- SPI and 25LC256 EEPROM ---------------- */

unsigned char spi(unsigned char tx) {
    unsigned int timeout = 100000;
    unsigned int s;

    if (spi_err)
        return 0;

    S0SPDR = tx;
    do {
        s = S0SPSR;
        if (s & 0x18)
            spi_err = 2;                    /* mode fault or abort */
        if (--timeout == 0)
            spi_err = 1;                    /* no response */
    } while ((s & 0x80) == 0 && spi_err == 0);

    return S0SPDR;                          /* reading it clears the done flag */
}

unsigned char ee_status(void) {
    unsigned char s;

    IO0CLR = CS;
    spi(0x05);                              /* RDSR */
    s = spi(0);
    IO0SET = CS;
    return s;
}

void ee_read_page(unsigned int addr) {      /* 64 bytes into buf[] */
    int i;

    IO0CLR = CS;
    spi(0x03);                              /* READ */
    spi(addr >> 8);
    spi(addr & 0xFF);
    for (i = 0; i < 64; i++)
        buf[i] = spi(0);
    IO0SET = CS;
}

unsigned int ee_write_page(unsigned int addr, int n) {
    int i;
    unsigned char s;

    IO0CLR = CS;
    spi(0x06);                              /* WREN */
    IO0SET = CS;

    s = ee_status();
    if (spi_err)
        return spi_err;
    if ((s & 0x02) == 0)
        return 5;                           /* write enable not set */

    IO0CLR = CS;
    spi(0x02);                              /* WRITE */
    spi(addr >> 8);
    spi(addr & 0xFF);
    for (i = 0; i < n; i++)
        spi(buf[i]);
    IO0SET = CS;

    for (i = 0; i < 20000; i++) {           /* wait while write is in progress */
        s = ee_status();
        if (spi_err)
            return spi_err;
        if ((s & 0x01) == 0)
            return 0;
    }
    return 6;
}

/* ---------------- Profile page ----------------
   'M' 'P' 3 id steps reps name[6], then 6 bytes per step, then checksum */

unsigned char checksum(int n) {
    unsigned char s = 0;
    int i;

    for (i = 0; i < n; i++)
        s += buf[i];
    return s;
}

int encode(int k) {                         /* default profile k into buf[] */
    int i;
    int n = 12;

    buf[0] = 'M';
    buf[1] = 'P';
    buf[2] = 3;
    buf[3] = k;
    buf[4] = def_steps[k];
    buf[5] = def_reps[k];

    for (i = 0; i < 6; i++)
        buf[6 + i] = def_name[k][i];

    for (i = 0; i < (int)def_steps[k]; i++) {
        buf[n]     = def_dir[k][i];
        buf[n + 1] = def_speed[k][i];
        buf[n + 2] = def_ramp[k][i] >> 8;
        buf[n + 3] = def_ramp[k][i] & 0xFF;
        buf[n + 4] = def_run[k][i] >> 8;
        buf[n + 5] = def_run[k][i] & 0xFF;
        n += 6;
    }

    buf[n] = 0 - checksum(n);               /* whole record now sums to 0 */
    return n + 1;
}

int decode(int k) {                         /* buf[] into profile k, returns 1 if valid */
    int i;
    int n = buf[4];

    if (buf[0] != 'M' || buf[1] != 'P' || buf[2] != 3 || buf[3] != k)
        return 0;
    if (n < 1 || n > 8 || buf[5] < 1 || buf[5] > 9)
        return 0;
    if (checksum(13 + 6 * n) != 0)
        return 0;

    for (i = 0; i < n; i++) {
        if (buf[12 + 6 * i] > REV || buf[13 + 6 * i] > 100)
            return 0;
    }

    for (i = 0; i < n; i++) {
        dir[k][i]   = buf[12 + 6 * i];
        speed[k][i] = buf[13 + 6 * i];
        ramp[k][i]  = buf[14 + 6 * i] * 256 + buf[15 + 6 * i];
        run[k][i]   = buf[16 + 6 * i] * 256 + buf[17 + 6 * i];
    }

    for (i = 0; i < 6; i++)
        name[k][i] = buf[6 + i];
    name[k][6] = 0;
    nsteps[k] = n;
    reps[k] = buf[5];
    return 1;
}

unsigned int load_profiles(void) {
    int k;
    unsigned int e;
    unsigned char s = ee_status();

    if (spi_err)
        return spi_err;
    if (s == 0xFF)
        return 3;                           /* no EEPROM */
    if ((s & 0x0C) == 0x0C)
        return 4;                           /* write-protected */

    for (k = 0; k < 6; k++) {
        ee_read_page(k * 64);
        if (spi_err)
            return spi_err;
        if (decode(k))
            continue;

        /* bad page: write the default and read it back */
        e = ee_write_page(k * 64, encode(k));
        if (e != 0)
            return e;

        ee_read_page(k * 64);
        if (spi_err)
            return spi_err;
        if (!decode(k))
            return 9;
    }
    return 0;
}

/* ---------------- Motor ---------------- */

void set_speed(unsigned int p) {
    duty = p;
    service();
}

void set_dir(unsigned int d) {
    IO0CLR = IN1 | IN2;                     /* IN1 and IN2 never high together */
    if (d == FWD)
        IO0SET = IN1;
    if (d == REV)
        IO0SET = IN2;

    if (d == STOP && cur_dir != STOP) {
        last_dir = cur_dir;
        off_since = now;
    }
    cur_dir = d;
}

int ramp_to(unsigned int target, unsigned int ms) {   /* 1% steps, exactly ms long */
    unsigned int from = duty;
    unsigned int start = now;
    unsigned int n, k;

    if (target > from)
        n = target - from;
    else
        n = from - target;

    if (n == 0)
        return 1;

    for (k = 1; k <= n; k++) {
        if (!wait_until(start + ms * MS * k / n))
            return 0;

        if (target > from)
            set_speed(from + k);
        else
            set_speed(from - k);
    }

    now = start + ms * MS;
    return 1;
}

void set_phase(unsigned int ph, unsigned int target) {
    phase = ph;
    show_status(target);
}

void run_step(void) {
    unsigned int d = dir[profile][step];

    if (cur_dir != STOP && d != cur_dir) {  /* slow down before changing direction */
        set_phase(DECEL, 0);
        if (!ramp_to(0, ramp[profile][step]))
            return;
        set_dir(STOP);
    }

    if (d != STOP && d != cur_dir) {
        if (last_dir != STOP && last_dir != d && (int)(off_since + 200 * MS - now) > 0) {
            set_phase(DEAD, 0);             /* 200 ms off before reversing */
            now = off_since + 200 * MS;
            if (!wait_until(now))
                return;
        }
        set_dir(d);
    }

    if (d != STOP) {
        set_phase(ACCEL, speed[profile][step]);
        if (!ramp_to(speed[profile][step], ramp[profile][step]))
            return;
    }

    set_phase(HOLD, duty);
    now += run[profile][step] * MS;
    wait_until(now);
}

void run_profile(void) {
    for (rep = 0; rep < reps[profile]; rep++) {
        for (step = 0; step < nsteps[profile]; step++) {
            run_step();
            if (skip)
                return;
        }
    }
}

/* ---------------- Main ---------------- */

int main(void) {
    int i;

    PLLCON = 0;                             /* PLL off, CPU at 12 MHz */
    PLLFEED = 0xAA;
    PLLFEED = 0x55;
    VPBDIV = 0;                             /* peripherals at 3 MHz */
    SCS = 0;

    PINSEL0 = (PINSEL0 & 0x30FC00F3) | 0x1500;   /* P0.4-P0.6 = SPI, rest GPIO */
    PINSEL2 = PINSEL2 & ~8;                      /* P1.16-P1.25 = GPIO */

    IO0SET = CS;
    IO0CLR = LED | EN1 | IN1 | IN2;
    IO0DIR |= LED | CS | EN1 | IN1 | IN2;
    IO1CLR = 0x3F << 16;
    IO1DIR |= 0x3F << 16;

    PCONP |= (1 << 2) | (1 << 8);           /* Timer1 and SPI0 on */

    T1TCR = 2;
    T1PR = FOSC / 4 / 100000 - 1;           /* Timer1 tick = 10 us */
    T1TCR = 1;
    ms_mark = T1TC;
    pwm_start = ms_mark;

    S0SPCCR = 8;
    S0SPCR = 0x20;                          /* SPI master, mode 0, 375 kHz */

    /* LCD init: 3, 3, 3, 2, then settings */
    delay_ms(40);
    for (i = 0; i < 4; i++) {
        if (i < 3)
            lcd_nibble(0, 3);
        else
            lcd_nibble(0, 2);

        if (i == 0)
            delay_ms(5);
        else
            delay_ms(1);
    }
    lcd_byte(0, 0x28);
    delay_ms(1);
    lcd_byte(0, 0x0C);
    delay_ms(1);
    lcd_byte(0, 0x06);
    delay_ms(1);
    lcd_byte(0, 0x01);
    delay_ms(2);

    lcd_clear();
    lcd_text(0, "MOTOR PROFILES");
    lcd_text(16, "Loading EEPROM");
    lcd_left = 34;

    for (i = 0; i < 3; i++) {
        pin(LED, 1);
        delay_ms(100);
        pin(LED, 0);
        delay_ms(100);
    }

    fail = load_profiles();
    if (fail) {                             /* EEPROM fault: LED on, motor off */
        lcd_clear();
        lcd_text(0, "EEPROM ERROR");
        lcd_text(16, "Fail code");
        lcd_num(26, 1, fail);
        lcd_left = 34;
        pin(LED, 1);
        while (1)
            service();
    }

    btn = (IO0PIN >> 15) & 1;
    running = 1;
    now = T1TC;

    while (1) {
        if (!skip)
            run_profile();

        if (skip) {                         /* NEXT: ramp down, stop, go to next profile */
            skip = 0;
            now = T1TC;
            set_phase(DECEL, 0);
            ramp_to(0, 300);
            set_speed(0);
            now = T1TC;
            set_dir(STOP);
        }

        if (++profile == 6) {
            profile = 0;
            cycles++;
        }
    }
}
