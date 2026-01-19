/*
 * Плата-генератор. STM32 + DAC (или PWM) + DMA circular.
 */

#include "stm32fxxx_hal.h"   // замените на конкретный заголовок
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

// Настройки таблиц
#define WAVE_TABLE_POINTS 256
#define MAX_USER_POINTS   1024

// Типы форм
enum {
    WAVE_SINE = 0,
    WAVE_RECT_FULL,
    WAVE_RECT_HALF,
    WAVE_SAW,
    WAVE_TRI,
    WAVE_SQUARE,
    WAVE_USER
};

// Текущие параметры
static uint8_t wave_type = WAVE_SINE;
static uint32_t freq_mHz = 1000000;  // 1 кГц по умолчанию
static uint16_t ampl_mVpp = 1000;    // 1 В пик-пик
static int16_t offset_mV = 0;
static uint16_t duty_permille = 500;

static uint16_t wave_table[MAX_USER_POINTS];
static uint16_t table_len = WAVE_TABLE_POINTS;

static void SystemClock_Config(void);
static void MX_DAC_PWM_Init(void);
static void MX_USB_UART_Init(void);
static void MX_TIM_Wave_Init(uint32_t freq_mHz, uint16_t points);
static void apply_waveform(void);
static void fill_wave_table(uint8_t type);
static void handle_command(uint8_t *pkt, uint16_t len);
static uint16_t crc16_ibm(const uint8_t *data, uint16_t len);

// Можно улучшить: добавить калибровку амплитуды с учётом опорного напряжения.

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_DAC_PWM_Init();
    MX_USB_UART_Init();

    fill_wave_table(wave_type);
    MX_TIM_Wave_Init(freq_mHz, table_len);
    apply_waveform();

    while (1) {
        // TODO: читать входящие кадры по USB CDC/UART и вызывать handle_command()
        // Можно добавить светодиод статуса.
    }
}

// Обработка команд
static void handle_command(uint8_t *pkt, uint16_t len)
{
    if (len < 6) return;
    uint8_t cmd = pkt[5];

    switch (cmd) {
    case 0x10: // set_wave
        if (len >= 7) {
            wave_type = pkt[6];
            if (wave_type != WAVE_USER) {
                fill_wave_table(wave_type);
            }
            apply_waveform();
        }
        break;
    case 0x11: // set_freq
        if (len >= 10) {
            memcpy(&freq_mHz, &pkt[6], 4);
            MX_TIM_Wave_Init(freq_mHz, table_len);
            apply_waveform();
        }
        break;
    case 0x12: // set_ampl
        if (len >= 8) {
            memcpy(&ampl_mVpp, &pkt[6], 2);
            fill_wave_table(wave_type);
            apply_waveform();
        }
        break;
    case 0x13: // set_offset
        if (len >= 8) {
            memcpy(&offset_mV, &pkt[6], 2);
            fill_wave_table(wave_type);
            apply_waveform();
        }
        break;
    case 0x14: // set_duty (для меандра)
        if (len >= 8) {
            memcpy(&duty_permille, &pkt[6], 2);
            if (wave_type == WAVE_SQUARE) {
                fill_wave_table(wave_type);
                apply_waveform();
            }
        }
        break;
    case 0x15: // upload_wave
        if (len >= 8) {
            uint16_t n = 0;
            memcpy(&n, &pkt[6], 2);
            if (n > 0 && n <= MAX_USER_POINTS && len >= 8 + n * 2) {
                memcpy(wave_table, &pkt[8], n * 2);
                table_len = n;
                wave_type = WAVE_USER;
                MX_TIM_Wave_Init(freq_mHz, table_len);
                apply_waveform();
            }
        }
        break;
    case 0x1F: // get_gen_status
        // TODO: отправить ответ с текущими параметрами
        break;
    default:
        // Можно улучшить: вернуть ошибку "неизвестная команда"
        break;
    }
}

// Заполнение таблиц форм
static void fill_wave_table(uint8_t type)
{
    table_len = WAVE_TABLE_POINTS;
    float ampl = ampl_mVpp / 2.0f;
    float offset = offset_mV;

    switch (type) {
    case WAVE_SINE:
        for (uint16_t i = 0; i < table_len; i++) {
            float x = (2.0f * 3.1415926f * i) / table_len;
            float v = offset + ampl * sinf(x);
            if (v < 0) v = 0; // простая защита
            if (v > 3300) v = 3300;
            wave_table[i] = (uint16_t)(v * 4095.0f / 3300.0f);
        }
        break;
    case WAVE_RECT_FULL:
        for (uint16_t i = 0; i < table_len; i++) {
            float v = offset + ampl * fabsf(sinf((2.0f * 3.1415926f * i) / table_len));
            if (v > 3300) v = 3300;
            wave_table[i] = (uint16_t)(v * 4095.0f / 3300.0f);
        }
        break;
    case WAVE_RECT_HALF:
        for (uint16_t i = 0; i < table_len; i++) {
            float s = sinf((2.0f * 3.1415926f * i) / table_len);
            float v = offset + (s > 0 ? ampl * s : 0);
            if (v < 0) v = 0;
            if (v > 3300) v = 3300;
            wave_table[i] = (uint16_t)(v * 4095.0f / 3300.0f);
        }
        break;
    case WAVE_SAW:
        for (uint16_t i = 0; i < table_len; i++) {
            float v = offset + ampl * ((float)i / table_len * 2.0f - 1.0f);
            if (v < 0) v = 0;
            if (v > 3300) v = 3300;
            wave_table[i] = (uint16_t)(v * 4095.0f / 3300.0f);
        }
        break;
    case WAVE_TRI:
        for (uint16_t i = 0; i < table_len; i++) {
            float phase = (float)i / table_len;
            float v = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
            v = offset + ampl * (v * 2.0f - 1.0f);
            if (v < 0) v = 0;
            if (v > 3300) v = 3300;
            wave_table[i] = (uint16_t)(v * 4095.0f / 3300.0f);
        }
        break;
    case WAVE_SQUARE:
        for (uint16_t i = 0; i < table_len; i++) {
            float duty = duty_permille / 1000.0f;
            float v = ( (float)i / table_len < duty ) ? (offset + ampl) : (offset - ampl);
            if (v < 0) v = 0;
            if (v > 3300) v = 3300;
            wave_table[i] = (uint16_t)(v * 4095.0f / 3300.0f);
        }
        break;
    case WAVE_USER:
        // Пользовательская таблица уже загружена, просто не трогаем
        break;
    default:
        break;
    }
}

// Применяем таблицу: останавливаем DMA, обновляем буфер, запускаем снова
static void apply_waveform(void)
{
    // TODO: остановить DMA, скопировать wave_table в буфер DAC/PWM, запустить DMA circular
}

// Настройки железа — заполните под конкретный МК
static void SystemClock_Config(void) { /* TODO */ }
static void MX_DAC_PWM_Init(void) { /* TODO */ }
static void MX_USB_UART_Init(void) { /* TODO */ }
static void MX_TIM_Wave_Init(uint32_t freq_mHz, uint16_t points) { /* TODO */ }

static uint16_t crc16_ibm(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}
