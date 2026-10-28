/*
 * L293D and motor wiring test (no EEPROM, timers or PWM)
 * EN1 held high; the motor runs forward, stops, reverses and stops.
 */

#define REG32(addr) (*(volatile unsigned int *)(addr))

#define PINSEL0  REG32(0xE002C000)
#define IO0SET   REG32(0xE0028004)
#define IO0DIR   REG32(0xE0028008)
#define IO0CLR   REG32(0xE002800C)

#define LED_PIN  (1U << 1)
#define CS_PIN   (1U << 7)
#define EN1_PIN  (1U << 8)
#define IN1_PIN  (1U << 12)
#define IN2_PIN  (1U << 13)

#define PINSEL0_MASK  ((3U << 2) | (3U << 14) | (3U << 16) | (0xFU << 24))

static void delay(unsigned int count)
{
    volatile unsigned int i;

    for (i = 0; i < count; i++) {
    }
}

int main(void)
{
    PINSEL0 &= ~PINSEL0_MASK;

    IO0SET = CS_PIN;
    IO0CLR = LED_PIN | EN1_PIN | IN1_PIN | IN2_PIN;
    IO0DIR |= LED_PIN | CS_PIN | EN1_PIN | IN1_PIN | IN2_PIN;

    IO0SET = EN1_PIN;

    while (1) {
        IO0CLR = IN2_PIN;
        IO0SET = IN1_PIN;
        IO0SET = LED_PIN;
        delay(2000000U);

        IO0CLR = IN1_PIN | IN2_PIN;
        IO0CLR = LED_PIN;
        delay(1000000U);

        IO0SET = IN2_PIN;
        delay(2000000U);

        IO0CLR = IN1_PIN | IN2_PIN;
        delay(1000000U);
    }
}
