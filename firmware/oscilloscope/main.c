/*
 * Плата-осциллограф. STM32 + ADC + DMA (ping-pong) + USB CDC/UART.
 */

#include "stm32fxxx_hal.h"   // замените на конкретный заголовок
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// Настройки буферов
#define OSC_DMA_POINTS    2048             // размер половины DMA буфера
#define OSC_FRAME_POINTS  8192             // сколько точек отправляем в одном кадре
#define OSC_RING_FRAMES   4                // количество кадров в кольце

// Коды команд (см. docs/protocol.md)
#define CMD_STREAM_ON 0x24
#define CMD_SET_FS    0x20
#define CMD_SET_TRIG  0x22
#define CMD_OSC_DATA  0x40

// Простая структура кадра для очереди
typedef struct {
    uint32_t fs_hz;
    uint16_t nsamples;
    uint16_t pretrig;
    uint16_t data[OSC_FRAME_POINTS];
} osc_frame_t;

// Кольцевой буфер кадров
static osc_frame_t ring[OSC_RING_FRAMES];
static volatile uint8_t ring_wr = 0;
static volatile uint8_t ring_rd = 0;
static volatile bool stream_on = false;

// DMA буфер (ping-pong)
static uint16_t dma_buf[OSC_DMA_POINTS * 2];

// Прототипы
static void SystemClock_Config(void);
static void MX_ADC_Init(void);
static void MX_USB_UART_Init(void);
static void MX_TIM_Sample_Init(uint32_t fs_hz);
static void start_adc_dma(void);
static void handle_command(uint8_t *pkt, uint16_t len);
static void push_block(uint16_t *src, uint16_t count);
static void send_frame(osc_frame_t *f);
static uint16_t crc16_ibm(const uint8_t *data, uint16_t len);

// Можно улучшить: вынести протокол в отдельный модуль и разделить обработку команд/потока.

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_ADC_Init();
    MX_USB_UART_Init();
    MX_TIM_Sample_Init(100000); // 100 кГц по умолчанию
    start_adc_dma();

    // Главный цикл: принимаем команды, отправляем готовые кадры
    while (1) {
        // TODO: читать из USB CDC/UART входящие кадры и вызывать handle_command()
        // Здесь можно вставить неблокирующий опрос очереди USB CDC.

        if (stream_on && ring_rd != ring_wr) {
            send_frame(&ring[ring_rd]);
            ring_rd = (ring_rd + 1) % OSC_RING_FRAMES;
        }
    }
}

// Колбэк DMA: половина буфера
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    push_block(&dma_buf[0], OSC_DMA_POINTS);
}

// Колбэк DMA: весь буфер
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    push_block(&dma_buf[OSC_DMA_POINTS], OSC_DMA_POINTS);
}

// Складываем блок выборок в кольцо кадров
static void push_block(uint16_t *src, uint16_t count)
{
    static uint16_t collected = 0;
    osc_frame_t *f = &ring[ring_wr];

    if (!stream_on) return;

    uint16_t to_copy = (collected + count > OSC_FRAME_POINTS) ? (OSC_FRAME_POINTS - collected) : count;
    memcpy(&f->data[collected], src, to_copy * sizeof(uint16_t));
    collected += to_copy;

    if (collected >= OSC_FRAME_POINTS) {
        f->fs_hz = 100000; // TODO: хранить текущую частоту дискретизации
        f->nsamples = OSC_FRAME_POINTS;
        f->pretrig = 0;    // TODO: считать долю предтриггера
        ring_wr = (ring_wr + 1) % OSC_RING_FRAMES;
        if (ring_wr == ring_rd) {
            // Можно улучшить: счётчик пропусков кадров, если очередь переполнена
            ring_rd = (ring_rd + 1) % OSC_RING_FRAMES;
        }
        collected = 0;
    }
}

// Простая обработка команд (упрощена)
static void handle_command(uint8_t *pkt, uint16_t len)
{
    if (len < 6) return; // минимальный размер без CRC
    uint8_t cmd = pkt[5]; // sync(2)+ver(1)+seq(2)+cmd(1)

    switch (cmd) {
    case CMD_STREAM_ON:
        if (len >= 7) {
            stream_on = pkt[6];
            if (stream_on) {
                ring_rd = ring_wr = 0;
            }
        }
        break;
    case CMD_SET_FS:
        // TODO: распарсить частоту и перенастроить таймер
        break;
    case CMD_SET_TRIG:
        // TODO: настроить триггер (пока триггер выключен)
        break;
    default:
        // Можно улучшить: отправлять ошибку "неизвестная команда"
        break;
    }
}

// Отправка кадра данных (голый пример, без очереди USB)
static void send_frame(osc_frame_t *f)
{
    uint8_t hdr[9];
    uint16_t payload_len = sizeof(uint32_t) + 1 + 2 + 2 + f->nsamples * 2;
    uint16_t idx = 0;
    hdr[idx++] = 0x55; // sync low
    hdr[idx++] = 0xAA; // sync high
    hdr[idx++] = 0x01; // ver
    hdr[idx++] = 0x00; // seq low (пока 0)
    hdr[idx++] = 0x00; // seq high
    hdr[idx++] = CMD_OSC_DATA;
    hdr[idx++] = payload_len & 0xFF;
    hdr[idx++] = payload_len >> 8;

    // Можно улучшить: использовать DMA для передачи
    // TODO: собрать буфер: hdr + meta + данные + crc и отправить по USB CDC/UART
}

// Заглушки инициализаций — заполните под конкретную плату
static void SystemClock_Config(void) { /* TODO */ }
static void MX_ADC_Init(void) { /* TODO */ }
static void MX_USB_UART_Init(void) { /* TODO */ }
static void MX_TIM_Sample_Init(uint32_t fs_hz) { /* TODO */ }
static void start_adc_dma(void) { /* TODO */ }

// Простой CRC16/IBM
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
