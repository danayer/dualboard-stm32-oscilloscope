# Протокол обмена между ПК и платами

Кадр (little-endian):
```
sync   : 0xAA55 (u16)
ver    : 0x01   (u8)
seq    : счетчик запроса (u16)
cmd    : код команды (u8)
len    : длина полезных данных (u16)
payload: len байт
crc16  : CRC-16/IBM над полями ver..payload
```
Ответы возвращают тот же `seq`, `cmd|0x80` и полезные данные.

## Команды генератора (плата 2)
- 0x10 set_wave {u8 type}
  - 0 sine, 1 rect_full, 2 rect_half, 3 saw, 4 tri, 5 square, 6 user
- 0x11 set_freq {u32 mHz} — миллигерцы, шаг 1 Гц
- 0x12 set_ampl {u16 mVpp}
- 0x13 set_offset {i16 mV}
- 0x14 set_duty {u16 permille} — для меандра
- 0x15 upload_wave {u16 n, n×u12 в u16}
- 0x1F get_gen_status → {u8 wave; u32 freq_mHz; u16 ampl_mVpp; i16 offset_mV; u16 duty_permille}

## Команды осциллографа (плата 1)
- 0x20 set_fs {u32 Hz} — частота дискретизации
- 0x21 set_gain {u8 step} — шаги предусилителя/делителя
- 0x22 set_trigger {u8 mode; i16 level_mV; u8 edge}
  - mode: 0 off, 1 norm, 2 auto; edge: 0 rising, 1 falling
- 0x23 capture_once {u16 pre_pct; u16 samples} — единичный захват
- 0x24 stream_on {u8 on}
- 0x2F get_osc_status → {u32 fs; u8 gain; u8 mode; i16 level_mV; u8 edge}

## Кадр данных осциллографа (OSC_DATA, cmd=0x40)
Payload:
```
struct osc_meta {
    u32 fs_hz;
    u8  ch;        // номер канала
    u16 nsamples;  // количество точек в кадре
    u16 pretrig;   // сколько точек до триггера
};
далее nsamples значений (u12 в u16)
```

## Ноты по реализации CRC
- Полином 0xA001 (CRC-16/IBM), init 0xFFFF.
- Считаем от поля `ver` до конца payload включительно.

## Потоки и состояние
- Поток осциллографа не требует подтверждений, кадры идут подряд.
- Команды — запрос/ответ. При ошибке возвращаем код ошибки в первом байте payload (0 — нет ошибки).

## Идеи на будущее
- Добавить команду get_info с идентификатором платы и версией прошивки.
- Добавить пакет keepalive для контроля обрыва связи.
- Добавить склейку нескольких каналов в одном кадре.
