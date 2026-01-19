#include <gtk/gtk.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>

// Коммуникация простая: посылаем кадры протокола (см. docs/protocol.md) по USB CDC/UART.
// Здесь добавлен поток чтения осциллографа и минимальный рендер данных.

// Можно улучшить: вынести протокол в отдельный модуль и добавить поток получения данных осциллографа.

typedef struct {
    int fd_osc;
    int fd_gen;
    GtkDrawingArea *scope_area;
    GtkLabel *status_label;
    GtkEntry *osc_entry;
    GtkEntry *gen_entry;
    GThread *osc_thread;
    GMutex osc_lock;
    bool osc_thread_run;
    float osc_samples[4096];
    uint16_t osc_count;
    uint16_t seq;
} AppState;

static bool send_cmd(int fd, uint8_t cmd, const uint8_t *payload, uint16_t len, uint16_t *seq);

// Отправка пакета настройки генератора (несколько команд подряд)
static void on_apply_generator(GtkButton *btn, gpointer user_data)
{
    AppState *st = user_data;
    GtkWidget *box = gtk_widget_get_parent(GTK_WIDGET(btn));
    GtkComboBox *wave_combo = GTK_COMBO_BOX(g_object_get_data(G_OBJECT(box), "wave_combo"));
    GtkSpinButton *freq_spin = GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(box), "freq_spin"));
    GtkSpinButton *ampl_spin = GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(box), "ampl_spin"));
    GtkSpinButton *offset_spin = GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(box), "offset_spin"));
    GtkSpinButton *duty_spin = GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(box), "duty_spin"));

    uint8_t wave = gtk_combo_box_get_active(wave_combo);
    uint32_t freq = (uint32_t)gtk_spin_button_get_value(freq_spin) * 1000; // Гц -> мГц
    uint16_t ampl = (uint16_t)gtk_spin_button_get_value(ampl_spin);
    int16_t offset = (int16_t)gtk_spin_button_get_value(offset_spin);
    uint16_t duty = (uint16_t)gtk_spin_button_get_value(duty_spin);

    uint8_t p_wave[1] = {wave};
    uint8_t p_freq[4]; memcpy(p_freq, &freq, 4);
    uint8_t p_ampl[2]; memcpy(p_ampl, &ampl, 2);
    uint8_t p_offs[2]; memcpy(p_offs, &offset, 2);
    uint8_t p_duty[2]; memcpy(p_duty, &duty, 2);

    bool ok = true;
    ok &= send_cmd(st->fd_gen, 0x10, p_wave, 1, &st->seq);
    ok &= send_cmd(st->fd_gen, 0x11, p_freq, 4, &st->seq);
    ok &= send_cmd(st->fd_gen, 0x12, p_ampl, 2, &st->seq);
    ok &= send_cmd(st->fd_gen, 0x13, p_offs, 2, &st->seq);
    ok &= send_cmd(st->fd_gen, 0x14, p_duty, 2, &st->seq);

    gtk_label_set_text(st->status_label, ok ? "Генератор настроен" : "Ошибка отправки команд генератору");
}

// Простейший расчёт CRC16/IBM
static uint16_t crc16_ibm(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

// Отправка кадра: sync, ver=1, seq++, cmd, len, payload, crc16
static bool send_cmd(int fd, uint8_t cmd, const uint8_t *payload, uint16_t len, uint16_t *seq)
{
    if (fd < 0) return false;
    uint8_t buf[256];
    if (len > sizeof(buf) - 9) return false;
    uint16_t s = ++(*seq);
    size_t idx = 0;
    buf[idx++] = 0x55; // sync low
    buf[idx++] = 0xAA; // sync high
    buf[idx++] = 0x01; // ver
    buf[idx++] = s & 0xFF;
    buf[idx++] = s >> 8;
    buf[idx++] = cmd;
    buf[idx++] = len & 0xFF;
    buf[idx++] = len >> 8;
    if (payload && len) {
        memcpy(&buf[idx], payload, len);
    }
    size_t hdr_len = idx + len;
    uint16_t crc = crc16_ibm(&buf[2], hdr_len - 2); // считаем от ver
    buf[hdr_len] = crc & 0xFF;
    buf[hdr_len + 1] = crc >> 8;

    ssize_t w = write(fd, buf, hdr_len + 2);
    return w == (ssize_t)(hdr_len + 2);
}

// Заглушка открытия порта
static int open_port(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tio = {0};
    cfmakeraw(&tio);
    cfsetspeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Чтение осциллографа в отдельном потоке (очень упрощено)
static gpointer osc_reader_thread(gpointer data)
{
    AppState *st = data;
    uint8_t buf[8192];
    size_t have = 0;

    while (st->osc_thread_run) {
        ssize_t r = read(st->fd_osc, buf + have, sizeof(buf) - have);
        if (r <= 0) {
            g_usleep(1000);
            continue;
        }
        have += (size_t)r;

        // Поиск sync 0x55 0xAA
        size_t pos = 0;
        while (have >= 8) {
            if (!(buf[pos] == 0x55 && buf[pos + 1] == 0xAA)) { pos++; have--; continue; }
            if (pos > 0) { memmove(buf, buf + pos, have); pos = 0; }
            if (have < 8) break; // ждём минимум заголовок без CRC
            uint8_t ver = buf[2];
            uint16_t seq = buf[3] | (buf[4] << 8);
            uint8_t cmd = buf[5];
            uint16_t len = buf[6] | (buf[7] << 8);
            if (ver != 1) { memmove(buf, buf + 2, have - 2); have -= 2; continue; }
            size_t frame_len = 8 + len + 2; // hdr+payload+crc
            if (have < frame_len) break; // ждём весь кадр
            uint16_t crc_calc = crc16_ibm(&buf[2], 6 + len);
            uint16_t crc_rx = buf[8 + len] | (buf[9 + len] << 8);
            if (crc_calc != crc_rx) {
                // Можно улучшить: счётчик ошибок
                memmove(buf, buf + 2, have - 2);
                have -= 2;
                continue;
            }
            if (cmd == 0x40) { // OSC_DATA
                if (len >= 9) {
                    uint32_t fs = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24);
                    uint8_t ch = buf[12];
                    uint16_t ns = buf[13] | (buf[14] << 8);
                    uint16_t pre = buf[15] | (buf[16] << 8);
                    (void)fs; (void)ch; (void)pre; // пока не используем
                    if (ns > 0 && ns <= 2048 && 17 + ns * 2 <= 8 + len) {
                        GMutex *m = &st->osc_lock;
                        g_mutex_lock(m);
                        st->osc_count = ns;
                        for (uint16_t i = 0; i < ns; i++) {
                            uint16_t raw = buf[17 + i * 2] | (buf[18 + i * 2] << 8);
                            st->osc_samples[i] = (float)raw;
                        }
                        g_mutex_unlock(m);
                        gtk_widget_queue_draw(GTK_WIDGET(st->scope_area));
                    }
                }
            }
            memmove(buf, buf + frame_len, have - frame_len);
            have -= frame_len;
            (void)seq; // пока игнорируем
        }
    }
    return NULL;
}

static void on_connect_clicked(GtkButton *btn, gpointer user_data)
{
    AppState *st = user_data;
    (void)btn;
    const char *osc_path = gtk_editable_get_text(GTK_EDITABLE(st->osc_entry));
    const char *gen_path = gtk_editable_get_text(GTK_EDITABLE(st->gen_entry));

    if (st->osc_thread) {
        st->osc_thread_run = false;
        g_thread_join(st->osc_thread);
        st->osc_thread = NULL;
    }
    if (st->fd_osc > 0) close(st->fd_osc);
    if (st->fd_gen > 0) close(st->fd_gen);

    st->fd_osc = open_port(osc_path);
    st->fd_gen = open_port(gen_path);

    if (st->fd_osc < 0 || st->fd_gen < 0) {
        gtk_label_set_text(st->status_label, "Ошибка открытия портов");
    } else {
        st->osc_thread_run = true;
        st->osc_thread = g_thread_new("osc_rx", osc_reader_thread, st);
        gtk_label_set_text(st->status_label, "Порты открыты");
    }
}

static void on_start_stream(GtkButton *btn, gpointer user_data)
{
    AppState *st = user_data;
    (void)btn;
    uint8_t payload[1] = {1};
    if (send_cmd(st->fd_osc, 0x24, payload, 1, &st->seq)) {
        gtk_label_set_text(st->status_label, "Стрим осциллографа запущен");
    } else {
        gtk_label_set_text(st->status_label, "Не удалось запустить стрим");
    }
}

static void on_stop_stream(GtkButton *btn, gpointer user_data)
{
    AppState *st = user_data;
    (void)btn;
    uint8_t payload[1] = {0};
    if (send_cmd(st->fd_osc, 0x24, payload, 1, &st->seq)) {
        gtk_label_set_text(st->status_label, "Стрим остановлен");
    } else {
        gtk_label_set_text(st->status_label, "Не удалось остановить стрим");
    }
}

static void draw_scope(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
    (void)area;
    AppState *st = user_data;
    g_mutex_lock(&st->osc_lock);
    uint16_t n = st->osc_count;
    if (n == 0) {
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_paint(cr);
        g_mutex_unlock(&st->osc_lock);
        return;
    }
    float *data = st->osc_samples;
    float maxv = 4095.0f;
    cairo_set_source_rgb(cr, 0.05, 0.05, 0.08);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.2, 0.7, 0.2);
    cairo_set_line_width(cr, 1.2);
    for (uint16_t i = 0; i < n; i++) {
        double x = (double)i / (n - 1) * width;
        double y = height - (data[i] / maxv) * height;
        if (i == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
    g_mutex_unlock(&st->osc_lock);
}

static GtkWidget *build_scope_tab(AppState *st)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *connect_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    st->osc_entry = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(st->osc_entry), "/dev/ttyACM0");
    st->gen_entry = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(st->gen_entry), "/dev/ttyACM1");
    GtkWidget *connect_btn = gtk_button_new_with_label("Подключить");
    g_signal_connect(connect_btn, "clicked", G_CALLBACK(on_connect_clicked), st);
    gtk_box_append(GTK_BOX(connect_row), gtk_label_new("Осциллограф"));
    gtk_box_append(GTK_BOX(connect_row), GTK_WIDGET(st->osc_entry));
    gtk_box_append(GTK_BOX(connect_row), gtk_label_new("Генератор"));
    gtk_box_append(GTK_BOX(connect_row), GTK_WIDGET(st->gen_entry));
    gtk_box_append(GTK_BOX(connect_row), connect_btn);
    gtk_box_append(GTK_BOX(box), connect_row);

    // Кнопки управления потоком
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *start_btn = gtk_button_new_with_label("Старт");
    GtkWidget *stop_btn = gtk_button_new_with_label("Стоп");
    g_signal_connect(start_btn, "clicked", G_CALLBACK(on_start_stream), st);
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(on_stop_stream), st);
    gtk_box_append(GTK_BOX(btn_row), start_btn);
    gtk_box_append(GTK_BOX(btn_row), stop_btn);
    gtk_box_append(GTK_BOX(box), btn_row);

    // Поле отрисовки осциллограммы
    st->scope_area = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_content_width(st->scope_area, 640);
    gtk_drawing_area_set_content_height(st->scope_area, 240);
    gtk_drawing_area_set_draw_func(st->scope_area, draw_scope, st, NULL);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(st->scope_area));

    // Статус
    st->status_label = GTK_LABEL(gtk_label_new("Нет подключений"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(st->status_label));

    return box;
}

static GtkWidget *build_generator_tab(AppState *st)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *wave_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wave_combo), "Синус");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wave_combo), "Двухполупериодный");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wave_combo), "Однополупериодный");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wave_combo), "Пила");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wave_combo), "Треугольник");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wave_combo), "Меандр");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wave_combo), "Пользовательская");
    gtk_combo_box_set_active(GTK_COMBO_BOX(wave_combo), 0);

    GtkWidget *freq_spin = gtk_spin_button_new_with_range(1, 200000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(freq_spin), 1000);

    GtkWidget *ampl_spin = gtk_spin_button_new_with_range(0, 3000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ampl_spin), 1000);

    GtkWidget *offset_spin = gtk_spin_button_new_with_range(-1500, 1500, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(offset_spin), 0);

    GtkWidget *duty_spin = gtk_spin_button_new_with_range(1, 999, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(duty_spin), 500);

    gtk_box_append(GTK_BOX(box), gtk_label_new("Форма сигнала"));
    gtk_box_append(GTK_BOX(box), wave_combo);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Частота, Гц"));
    gtk_box_append(GTK_BOX(box), freq_spin);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Амплитуда, мВpp"));
    gtk_box_append(GTK_BOX(box), ampl_spin);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Смещение, мВ"));
    gtk_box_append(GTK_BOX(box), offset_spin);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Скважность, промилле"));
    gtk_box_append(GTK_BOX(box), duty_spin);

    GtkWidget *apply_btn = gtk_button_new_with_label("Применить");
    gtk_box_append(GTK_BOX(box), apply_btn);
    // Подключаем обработчик применения настроек генератора
    g_object_set_data(G_OBJECT(box), "wave_combo", wave_combo);
    g_object_set_data(G_OBJECT(box), "freq_spin", freq_spin);
    g_object_set_data(G_OBJECT(box), "ampl_spin", ampl_spin);
    g_object_set_data(G_OBJECT(box), "offset_spin", offset_spin);
    g_object_set_data(G_OBJECT(box), "duty_spin", duty_spin);
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply_generator), st);

    return box;
}

static void app_activate(GtkApplication *app, gpointer user_data)
{
    AppState *st = user_data;
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Осциллограф + Генератор");
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 600);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_scope_tab(st), gtk_label_new("Осциллограф"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_generator_tab(st), gtk_label_new("Генератор"));

    gtk_window_set_child(GTK_WINDOW(win), notebook);
    gtk_widget_set_visible(win, TRUE);
}

int main(int argc, char **argv)
{
    AppState st = {0};
    g_mutex_init(&st.osc_lock);
    GtkApplication *app = gtk_application_new("student.oscgen", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(app_activate), &st);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    if (st.osc_thread) {
        st.osc_thread_run = false;
        g_thread_join(st.osc_thread);
    }
    if (st.fd_osc > 0) close(st.fd_osc);
    if (st.fd_gen > 0) close(st.fd_gen);
    return status;
}
