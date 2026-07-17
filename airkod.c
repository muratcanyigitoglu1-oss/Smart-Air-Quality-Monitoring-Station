#include <msp430.h>
#include <stdint.h>
#include "lcd_i2c.h"

/* =========================================================
   PIN TANIMLARI
   ========================================================= */

// MQ-135 -> P1.5 / A5
#define MQ135_CHANNEL   INCH_5
#define MQ135_PIN       BIT5

// MQ-7 -> P1.3 / A3
#define MQ7_CHANNEL     INCH_3
#define MQ7_PIN         BIT3

// DHT22 -> P2.1
#define DHT22_PIN       BIT1

// FAN / MOSFET GATE -> P2.2
#define FAN_PIN         BIT2

// HC-05 TEK YONLU YAZILIMSAL UART TX -> P2.0
#define BT_TX_PIN       BIT0

// DHT22 timing
#define DHT_TIMEOUT_US       10000
#define DHT22_THRESHOLD_US   64

// AQI eşik
#define AQI_FAN_THRESHOLD    30

// Software UART 9600 baud -> yaklaşık 104 us
#define SOFT_UART_BIT_US     104

// Ana döngü gecikmesi ve DHT periyodu
#define MAIN_LOOP_DELAY_MS   500
#define DHT_LOOP_INTERVAL    6   // 6 * 500ms = 3000ms

// DHT filtre sınırları
#define DHT_MAX_TEMP_X10_JUMP   30    // 3.0C
#define DHT_MAX_HUM_X10_JUMP    100   // 10.0%

/* =========================================================
   GLOBAL DEGISKENLER
   ========================================================= */

unsigned int mq135_raw = 0;
unsigned int mq7_raw = 0;

unsigned int temperature_x10 = 0;
unsigned int humidity_x10 = 0;

unsigned int last_valid_temperature_x10 = 0;
unsigned int last_valid_humidity_x10 = 0;

unsigned char temperature = 0;
unsigned char humidity = 0;
unsigned char dht_ok = 0;
unsigned char have_valid_dht = 0;

unsigned char mq135_score = 0;
unsigned char mq7_score = 0;
unsigned char temp_score = 0;
unsigned char hum_score = 0;

unsigned char air_index = 0;
unsigned char air_status = 0;   // 0 = IYI, 1 = TEHLIKE
unsigned char fan_state = 0;    // 0 = KAPALI, 1 = ACIK

unsigned char dht_data[5] = {0, 0, 0, 0, 0};
unsigned char dht_shift_right_data[5] = {0, 0, 0, 0, 0};
unsigned char dht_shift_left_data[5]  = {0, 0, 0, 0, 0};

unsigned char dht_error = 0;
unsigned char fail_bit = 0;
unsigned int last_width = 0;
unsigned char expected_checksum = 0;
unsigned char shift_mode = 0;

unsigned char dht_loop_counter = 0;

/* =========================================================
   CLOCK INIT - MSP430 8 MHz
   ========================================================= */

void clock_init(void)
{
    WDTCTL = WDTPW | WDTHOLD;

    if (CALBC1_8MHZ != 0xFF)
    {
        DCOCTL = 0;
        BCSCTL1 = CALBC1_8MHZ;
        DCOCTL = CALDCO_8MHZ;
    }
}

/* =========================================================
   TIMER1_A INIT
   SMCLK = 8 MHz
   ID_3 = /8
   Timer clock = 1 MHz
   1 count yaklaşık 1 us
   ========================================================= */

void timer1_init(void)
{
    TA1CTL = TASSEL_2 | ID_3 | MC_2 | TACLR;
}

/* =========================================================
   TIMER FONKSIYONLARI
   ========================================================= */

unsigned int timer_now(void)
{
    return TA1R;
}

unsigned int timer_elapsed(unsigned int start)
{
    return (unsigned int)(TA1R - start);
}

void delay_us_timer(unsigned int us)
{
    unsigned int start = timer_now();
    while (timer_elapsed(start) < us);
}

void delay_ms_timer(unsigned int ms)
{
    while (ms--)
    {
        delay_us_timer(1000);
    }
}

/* =========================================================
   SOFTWARE UART INIT - HC-05 BLUETOOTH
   HC-05 RXD -> MSP430 P2.0
   HC-05 TXD -> BOS
   ========================================================= */

void uart_init(void)
{
    P2SEL &= ~BT_TX_PIN;
    P2SEL2 &= ~BT_TX_PIN;

    P2DIR |= BT_TX_PIN;
    P2OUT |= BT_TX_PIN;   // idle HIGH
}

void uart_send_char(char c)
{
    unsigned char i;

    // Start bit
    P2OUT &= ~BT_TX_PIN;
    delay_us_timer(SOFT_UART_BIT_US);

    // 8 data bit, LSB first
    for (i = 0; i < 8; i++)
    {
        if (c & 0x01)
        {
            P2OUT |= BT_TX_PIN;
        }
        else
        {
            P2OUT &= ~BT_TX_PIN;
        }

        delay_us_timer(SOFT_UART_BIT_US);
        c >>= 1;
    }

    // Stop bit
    P2OUT |= BT_TX_PIN;
    delay_us_timer(SOFT_UART_BIT_US);
}

void uart_send_string(char *str)
{
    while (*str)
    {
        uart_send_char(*str++);
    }
}

void uart_send_number(unsigned int num)
{
    char buffer[6];
    int i = 0;

    if (num == 0)
    {
        uart_send_char('0');
        return;
    }

    while (num > 0 && i < 5)
    {
        buffer[i++] = '0' + (num % 10);
        num /= 10;
    }

    while (i > 0)
    {
        uart_send_char(buffer[--i]);
    }
}

void uart_send_x10(unsigned int value_x10)
{
    uart_send_number(value_x10 / 10);
    uart_send_char('.');
    uart_send_number(value_x10 % 10);
}

/* =========================================================
   ADC INIT
   MQ-135 -> P1.5 / A5
   MQ-7   -> P1.3 / A3
   ========================================================= */

void adc_init(void)
{
    ADC10CTL0 &= ~ENC;

    ADC10CTL0 = ADC10SHT_2 | ADC10ON;
    ADC10CTL1 = ADC10SSEL_3 | ADC10DIV_3;

    ADC10AE0 |= MQ135_PIN | MQ7_PIN;
}

unsigned int adc_read(unsigned int channel)
{
    ADC10CTL0 &= ~ENC;

    ADC10CTL1 = channel | ADC10SSEL_3 | ADC10DIV_3;

    ADC10CTL0 |= ENC | ADC10SC;

    while (ADC10CTL1 & ADC10BUSY);

    return ADC10MEM;
}

unsigned int adc_read_average(unsigned int channel)
{
    unsigned long sum = 0;
    unsigned char i;

    for (i = 0; i < 8; i++)
    {
        sum += adc_read(channel);
        delay_ms_timer(2);
    }

    return (unsigned int)(sum / 8);
}

/* =========================================================
   DHT22 GPIO INIT
   ========================================================= */

void dht22_gpio_init(void)
{
    P2SEL &= ~DHT22_PIN;
    P2SEL2 &= ~DHT22_PIN;

    P2DIR &= ~DHT22_PIN;

    // Modüllü DHT22 için dahili pull-up kapalı
    P2REN &= ~DHT22_PIN;
    P2OUT |= DHT22_PIN;
}

/* =========================================================
   DHT22 WAIT FUNCTIONS
   ========================================================= */

unsigned char dht22_wait_low(unsigned int timeout_us)
{
    unsigned int start = timer_now();

    while (P2IN & DHT22_PIN)
    {
        if (timer_elapsed(start) > timeout_us)
        {
            return 0;
        }
    }

    return 1;
}

unsigned char dht22_wait_high(unsigned int timeout_us)
{
    unsigned int start = timer_now();

    while (!(P2IN & DHT22_PIN))
    {
        if (timer_elapsed(start) > timeout_us)
        {
            return 0;
        }
    }

    return 1;
}

unsigned int dht22_measure_high_width_by_bit(void)
{
    unsigned int start;

    if (!dht22_wait_low(DHT_TIMEOUT_US))
    {
        return 0;
    }

    if (!dht22_wait_high(DHT_TIMEOUT_US))
    {
        return 0;
    }

    start = timer_now();

    while (P2IN & DHT22_PIN)
    {
        if (timer_elapsed(start) > DHT_TIMEOUT_US)
        {
            return 0;
        }
    }

    return timer_elapsed(start);
}

/* =========================================================
   DHT22 CHECKSUM / SHIFT FUNCTIONS
   ========================================================= */

unsigned char dht_checksum(unsigned char *data)
{
    return (unsigned char)(data[0] + data[1] + data[2] + data[3]);
}

unsigned char dht_checksum_ok(unsigned char *data)
{
    return (dht_checksum(data) == data[4]);
}

void copy_5_bytes(unsigned char *dst, unsigned char *src)
{
    unsigned char i;

    for (i = 0; i < 5; i++)
    {
        dst[i] = src[i];
    }
}

void shift_right_1bit(unsigned char *out, unsigned char *in)
{
    unsigned char i;
    unsigned char carry = 0;
    unsigned char new_carry;

    for (i = 0; i < 5; i++)
    {
        new_carry = in[i] & 0x01;
        out[i] = (in[i] >> 1) | (carry << 7);
        carry = new_carry;
    }
}

void shift_left_1bit(unsigned char *out, unsigned char *in)
{
    int i;
    unsigned char carry = 0;
    unsigned char new_carry;

    for (i = 4; i >= 0; i--)
    {
        new_carry = (in[i] & 0x80) ? 1 : 0;
        out[i] = (in[i] << 1) | carry;
        carry = new_carry;
    }
}

/* =========================================================
   DHT22 DATA PARSE
   ========================================================= */

unsigned char dht22_parse_data(unsigned char *data)
{
    unsigned int raw_humidity;
    unsigned int raw_temperature;

    raw_humidity = ((unsigned int)data[0] << 8) | data[1];
    raw_temperature = ((unsigned int)data[2] << 8) | data[3];

    if (raw_temperature & 0x8000)
    {
        raw_temperature &= 0x7FFF;
        temperature_x10 = 0;
        temperature = 0;
    }
    else
    {
        temperature_x10 = raw_temperature;
        temperature = (unsigned char)(temperature_x10 / 10);
    }

    humidity_x10 = raw_humidity;
    humidity = (unsigned char)(humidity_x10 / 10);

    if (humidity_x10 > 1000)
    {
        return 0;
    }

    if (temperature_x10 > 800)
    {
        return 0;
    }

    return 1;
}

/* =========================================================
   DHT22 READ ONCE
   ========================================================= */

unsigned char dht22_read(void)
{
    unsigned char bit_index;
    unsigned char byte_index;
    unsigned char bit_pos;
    unsigned int high_width;

    dht_error = 0;
    fail_bit = 0;
    last_width = 0;
    expected_checksum = 0;
    shift_mode = 0;

    dht_data[0] = 0;
    dht_data[1] = 0;
    dht_data[2] = 0;
    dht_data[3] = 0;
    dht_data[4] = 0;

    dht_shift_right_data[0] = 0;
    dht_shift_right_data[1] = 0;
    dht_shift_right_data[2] = 0;
    dht_shift_right_data[3] = 0;
    dht_shift_right_data[4] = 0;

    dht_shift_left_data[0] = 0;
    dht_shift_left_data[1] = 0;
    dht_shift_left_data[2] = 0;
    dht_shift_left_data[3] = 0;
    dht_shift_left_data[4] = 0;

    P2SEL &= ~DHT22_PIN;
    P2SEL2 &= ~DHT22_PIN;

    P2DIR |= DHT22_PIN;

    P2OUT |= DHT22_PIN;
    delay_ms_timer(5);

    P2OUT &= ~DHT22_PIN;
    delay_ms_timer(2);

    P2OUT |= DHT22_PIN;
    delay_us_timer(30);

    P2DIR &= ~DHT22_PIN;

    if (!dht22_wait_low(500))
    {
        dht_error = 1;
        return 0;
    }

    if (!dht22_wait_high(500))
    {
        dht_error = 2;
        return 0;
    }

    if (!dht22_wait_low(500))
    {
        dht_error = 3;
        return 0;
    }

    for (bit_index = 0; bit_index < 40; bit_index++)
    {
        fail_bit = bit_index;

        high_width = dht22_measure_high_width_by_bit();

        if (high_width == 0)
        {
            if (bit_index == 39)
            {
                high_width = 0;
            }
            else
            {
                dht_error = 4;
                return 0;
            }
        }

        last_width = high_width;

        byte_index = bit_index / 8;
        bit_pos = 7 - (bit_index % 8);

        if (high_width > DHT22_THRESHOLD_US)
        {
            dht_data[byte_index] |= (1 << bit_pos);
        }
    }

    expected_checksum = dht_checksum(dht_data);

    if (dht_checksum_ok(dht_data))
    {
        if (dht22_parse_data(dht_data))
        {
            shift_mode = 0;
            dht_error = 0;
            return 1;
        }
    }

    shift_right_1bit(dht_shift_right_data, dht_data);

    if (dht_checksum_ok(dht_shift_right_data))
    {
        if (dht22_parse_data(dht_shift_right_data))
        {
            copy_5_bytes(dht_data, dht_shift_right_data);
            expected_checksum = dht_checksum(dht_data);
            shift_mode = 1;
            dht_error = 0;
            return 1;
        }
    }

    shift_left_1bit(dht_shift_left_data, dht_data);

    if (dht_checksum_ok(dht_shift_left_data))
    {
        if (dht22_parse_data(dht_shift_left_data))
        {
            copy_5_bytes(dht_data, dht_shift_left_data);
            expected_checksum = dht_checksum(dht_data);
            shift_mode = 2;
            dht_error = 0;
            return 1;
        }
    }

    dht_error = 6;
    expected_checksum = dht_checksum(dht_data);
    return 0;
}

/* =========================================================
   DHT FILTRE VE RETRY
   ========================================================= */

unsigned int abs_diff_u16(unsigned int a, unsigned int b)
{
    if (a > b)
    {
        return a - b;
    }
    else
    {
        return b - a;
    }
}

unsigned char dht_value_reasonable(unsigned int t_x10, unsigned int h_x10)
{
    if (t_x10 > 600)   // 60.0C üstü reddet
    {
        return 0;
    }

    if (h_x10 > 1000)  // %100 üstü reddet
    {
        return 0;
    }

    return 1;
}

unsigned char dht_value_stable(unsigned int t_x10, unsigned int h_x10)
{
    if (!have_valid_dht)
    {
        return 1;
    }

    if (abs_diff_u16(t_x10, last_valid_temperature_x10) > DHT_MAX_TEMP_X10_JUMP)
    {
        return 0;
    }

    if (abs_diff_u16(h_x10, last_valid_humidity_x10) > DHT_MAX_HUM_X10_JUMP)
    {
        return 0;
    }

    return 1;
}

unsigned char dht22_read_filtered(void)
{
    unsigned char attempt;
    unsigned int backup_temp_x10 = temperature_x10;
    unsigned int backup_hum_x10 = humidity_x10;
    unsigned char backup_temp = temperature;
    unsigned char backup_hum = humidity;

    for (attempt = 0; attempt < 3; attempt++)
    {
        if (dht22_read())
        {
            if (dht_value_reasonable(temperature_x10, humidity_x10) &&
                dht_value_stable(temperature_x10, humidity_x10))
            {
                last_valid_temperature_x10 = temperature_x10;
                last_valid_humidity_x10 = humidity_x10;
                have_valid_dht = 1;
                dht_ok = 1;
                return 1;
            }
            else
            {
                dht_error = 8;   // filtre reddetti
            }
        }

        // Başarısız/filtre reddi sonrası eski değeri geri koy
        if (have_valid_dht)
        {
            temperature_x10 = last_valid_temperature_x10;
            humidity_x10 = last_valid_humidity_x10;
            temperature = (unsigned char)(temperature_x10 / 10);
            humidity = (unsigned char)(humidity_x10 / 10);
        }
        else
        {
            temperature_x10 = backup_temp_x10;
            humidity_x10 = backup_hum_x10;
            temperature = backup_temp;
            humidity = backup_hum;
        }

        dht22_gpio_init();
        delay_ms_timer(80);
    }

    dht_ok = 0;
    return 0;
}

/* =========================================================
   FAN INIT / CONTROL
   ========================================================= */

void fan_init(void)
{
    P2SEL &= ~FAN_PIN;
    P2SEL2 &= ~FAN_PIN;

    P2DIR |= FAN_PIN;

    P2OUT &= ~FAN_PIN;
    fan_state = 0;
}

void fan_on(void)
{
    P2OUT |= FAN_PIN;
    fan_state = 1;
}

void fan_off(void)
{
    P2OUT &= ~FAN_PIN;
    fan_state = 0;
}

/* =========================================================
   SICAKLIK SKORU
   ========================================================= */

unsigned char calculate_temp_score(unsigned char t)
{
    if (t >= 22 && t <= 27)
    {
        return 0;
    }
    else if ((t >= 18 && t <= 21) || (t >= 28 && t <= 30))
    {
        return 25;
    }
    else if ((t >= 15 && t <= 17) || (t >= 31 && t <= 35))
    {
        return 50;
    }
    else if ((t >= 10 && t <= 14) || (t >= 36 && t <= 40))
    {
        return 75;
    }
    else
    {
        return 100;
    }
}

/* =========================================================
   NEM SKORU
   ========================================================= */

unsigned char calculate_hum_score(unsigned char h)
{
    if (h >= 40 && h <= 60)
    {
        return 0;
    }
    else if ((h >= 30 && h <= 39) || (h >= 61 && h <= 70))
    {
        return 25;
    }
    else if ((h >= 20 && h <= 29) || (h >= 71 && h <= 80))
    {
        return 50;
    }
    else if ((h >= 10 && h <= 19) || (h >= 81 && h <= 90))
    {
        return 75;
    }
    else
    {
        return 100;
    }
}

/* =========================================================
   AQI HESABI
   ========================================================= */

unsigned char calculate_air_index(void)
{
    unsigned long index;

    mq135_score = (unsigned char)((mq135_raw * 100UL) / 1023UL);
    mq7_score   = (unsigned char)((mq7_raw   * 100UL) / 1023UL);

    temp_score = calculate_temp_score(temperature);
    hum_score  = calculate_hum_score(humidity);

    index =
        (unsigned long)mq135_score * 35UL +
        (unsigned long)mq7_score   * 35UL +
        (unsigned long)temp_score  * 15UL +
        (unsigned long)hum_score   * 15UL;

    index = index / 100UL;

    if (index > 100UL)
    {
        index = 100UL;
    }

    return (unsigned char)index;
}

/* =========================================================
   DURUM SINIFLANDIRMA
   ========================================================= */

void classify_air_status(void)
{
    if (air_index > AQI_FAN_THRESHOLD)
    {
        air_status = 1;
    }
    else
    {
        air_status = 0;
    }
}

/* =========================================================
   FAN KONTROL
   ========================================================= */

void control_fan(void)
{
    if (air_index > AQI_FAN_THRESHOLD)
    {
        fan_on();
    }
    else
    {
        fan_off();
    }
}

/* =========================================================
   LCD GÖSTERİM
   ========================================================= */

void lcd_show_status_only(void)
{
    lcd_clear();
    lcd_set_cursor(0, 0);

    if (air_status == 0)
    {
        lcd_print("YESIL: IYI :)");
    }
    else
    {
        lcd_print("KIRMIZI:TEHLIKE:(");
    }
}

/* =========================================================
   BLUETOOTH GONDERME
   ========================================================= */

void bluetooth_send_values(void)
{
    uart_send_string("MQ135=");
    uart_send_number(mq135_raw);

    uart_send_string(" MQ7=");
    uart_send_number(mq7_raw);

    uart_send_string(" TEMP=");
    uart_send_x10(temperature_x10);
    uart_send_string("C");

    uart_send_string(" HUM=");
    uart_send_x10(humidity_x10);
    uart_send_string("%");

    uart_send_string(" AQI=");
    uart_send_number(air_index);

    uart_send_string(" DURUM=");

    if (air_status == 0)
    {
        uart_send_string("IYI");
    }
    else
    {
        uart_send_string("TEHLIKE");
    }

    uart_send_string(" FAN=");

    if (fan_state)
    {
        uart_send_string("ACIK");
    }
    else
    {
        uart_send_string("KAPALI");
    }

    if (dht_ok)
    {
        uart_send_string(" DHT=OK");
    }
    else
    {
        uart_send_string(" DHT=ERR");
        uart_send_string(" E=");
        uart_send_number(dht_error);
    }

    uart_send_string("\r\n");
}

void bluetooth_send_alert_if_needed(void)
{
    if (air_status == 1)
    {
        uart_send_string("UYARI: AQI ESIGI ASILDI! FAN ACILDI!\r\n");
    }
}

/* =========================================================
   MAIN
   ========================================================= */

int main(void)
{
    clock_init();
    timer1_init();

    adc_init();
    uart_init();
    dht22_gpio_init();
    fan_init();

    lcd_init();

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("Akilli Hava");
    lcd_set_cursor(1, 0);
    lcd_print("DHT22 Sistem");

    uart_send_string("Akilli Hava Kalitesi Sistemi Basladi\r\n");
    uart_send_string("HC05 -> P2.0 TX only\r\n");
    uart_send_string("MQ average aktif, DHT filtre+retry aktif\r\n");

    delay_ms_timer(3000);

    // İlk DHT denemesi
    dht22_read_filtered();
    dht22_gpio_init();

    dht_loop_counter = 0;

    while (1)
    {
        mq135_raw = adc_read_average(MQ135_CHANNEL);
        mq7_raw   = adc_read_average(MQ7_CHANNEL);

        dht_loop_counter++;

        if (dht_loop_counter >= DHT_LOOP_INTERVAL)
        {
            dht22_read_filtered();
            dht22_gpio_init();
            dht_loop_counter = 0;
        }

        air_index = calculate_air_index();
        classify_air_status();
        control_fan();
        lcd_show_status_only();
        bluetooth_send_values();
        bluetooth_send_alert_if_needed();

        delay_ms_timer(MAIN_LOOP_DELAY_MS);
    }
}