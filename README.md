# Двухплатный осциллограф/генератор на STM32

Проект: две STM32 — одна как осциллограф (ADC+DMA+USB CDC/UART), вторая как генератор сигналов (DAC/PWM+DMA). Управление и просмотр с ПК (GTK4, C).

## Состав
- firmware/oscilloscope — прошивка платы-осциллографа.
- firmware/generator — прошивка платы-генератора.
- pc-app — GTK4 приложение для управления и отображения.
- docs — описание протокола и заметки.

## Аппаратные опоры
- Осциллограф: ADC + таймер запуска + двойной буфер DMA (ping-pong), USB CDC или UART.
- Генератор: DAC (или PWM + RC/активный фильтр) + таймер + DMA circular. Таблицы форм сигнала в ОЗУ.

## Минимальные границы (можно менять)
- Генератор: 1 Гц – 100 кГц (DAC) или до ~200 кГц (PWM), амплитуда 0–3000 мВpp, смещение ±1500 мВ.
- Осциллограф: частота дискретизации 1 кГц – 500 кГц, кадр 4096–16384 отсчётов.

## Сборка прошивок
Используйте STM32CubeMX/CubeIDE или cmake/Make с STM32 HAL. В коде оставлены пометки, где подставить конкретные MCU/пины/таймеры.

## Сборка ПК-приложения
Требования: GTK4, glib-2.0, gio-2.0, cairo, pkg-config.

### Зависимости по дистрибутивам
- Ubuntu/Debian: `sudo apt install build-essential pkg-config libgtk-4-dev libglib2.0-dev libgio2.0-dev libcairo2-dev`
- Fedora: `sudo dnf install gcc make pkg-config gtk4-devel glib2-devel gio-devel cairo-devel`
- openSUSE: `sudo zypper install gcc make pkg-config gtk4-devel glib2-devel gio-devel libcairo-devel`
- Arch Linux: `sudo pacman -S base-devel pkgconf gtk4 glib2 cairo`
- ALT Linux: `sudo apt-get install gcc make pkg-config gtk4-devel glib2-devel gio-devel cairo-devel`

### Сборка и запуск
```bash
cd pc-app
make
./osc_gen_ui
```

### Сборка AppImage (минимальный пример)
Понадобятся `appimagetool` и `linuxdeploy`.

Установка инструментов:
- Ubuntu/Debian: `sudo apt install wget fuse && wget -O appimagetool.AppImage -L https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage && chmod +x appimagetool.AppImage && sudo mv appimagetool.AppImage /usr/local/bin/appimagetool && wget -O linuxdeploy.AppImage -L https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage && chmod +x linuxdeploy.AppImage && sudo mv linuxdeploy.AppImage /usr/local/bin/linuxdeploy`
- Fedora: `sudo dnf install fuse fuse-libs wget && wget -O appimagetool.AppImage -L https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage && chmod +x appimagetool.AppImage && sudo mv appimagetool.AppImage /usr/local/bin/appimagetool && wget -O linuxdeploy.AppImage -L https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage && chmod +x linuxdeploy.AppImage && sudo mv linuxdeploy.AppImage /usr/local/bin/linuxdeploy`
- openSUSE: `sudo zypper install fuse wget && wget -O appimagetool.AppImage -L https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage && chmod +x appimagetool.AppImage && sudo mv appimagetool.AppImage /usr/local/bin/appimagetool && wget -O linuxdeploy.AppImage -L https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage && chmod +x linuxdeploy.AppImage && sudo mv linuxdeploy.AppImage /usr/local/bin/linuxdeploy`
- Arch Linux: `sudo pacman -S fuse2 wget && wget -O appimagetool.AppImage -L https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage && chmod +x appimagetool.AppImage && sudo mv appimagetool.AppImage /usr/local/bin/appimagetool && wget -O linuxdeploy.AppImage -L https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage && chmod +x linuxdeploy.AppImage && sudo mv linuxdeploy.AppImage /usr/local/bin/linuxdeploy`
- ALT Linux: `sudo apt-get install wget fuse && wget -O appimagetool.AppImage -L https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage && chmod +x appimagetool.AppImage && sudo mv appimagetool.AppImage /usr/local/bin/appimagetool && wget -O linuxdeploy.AppImage -L https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage && chmod +x linuxdeploy.AppImage && sudo mv linuxdeploy.AppImage /usr/local/bin/linuxdeploy`
```bash
# 1) Собрать бинарник
cd pc-app && make

# 2) Подготовить AppDir
mkdir -p AppDir/usr/bin AppDir/usr/share/applications AppDir/usr/share/icons/hicolor/256x256/apps
cp osc_gen_ui AppDir/usr/bin/
cat > AppDir/usr/share/applications/osc_gen_ui.desktop <<'EOF'
[Desktop Entry]
Name=OscGen UI
Exec=osc_gen_ui
Icon=osc_gen_ui
Type=Application
Categories=Development;Utility;
EOF

# 3) Добавить иконку (если есть):
# cp path/to/icon.png AppDir/usr/share/icons/hicolor/256x256/apps/osc_gen_ui.png

# 4) Упаковать
linuxdeploy --appdir AppDir --executable AppDir/usr/bin/osc_gen_ui --desktop-file AppDir/usr/share/applications/osc_gen_ui.desktop --icon-file AppDir/usr/share/icons/hicolor/256x256/apps/osc_gen_ui.png
appimagetool AppDir
# На выходе появится OscGen_UI-x86_64.AppImage
```

## Протокол
Смотрите docs/protocol.md. Кадры: sync 0xAA55, версия 1, seq, cmd, len, payload, crc16. Поток осциллографа — отдельные кадры OSC_DATA.

## Быстрый старт
1. Собрать прошивки, прошить обе платы.
2. Подключить две платы (разные USB CDC или UART). Узнать их /dev/ttyACM*.
3. Запустить GTK4-приложение, выбрать порты, включить поток осциллографа, настроить генератор.

## Идеи на будущее
- Калибровка амплитуды и АЦП по опорному напряжению.
- Аппаратный триггер на таймере через аналоговый компаратор.
- Режим загрузки пользовательских форм сигнала в генератор.
- Сохранение осциллограмм в CSV/PNG.
