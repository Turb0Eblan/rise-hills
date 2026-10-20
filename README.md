# Rise Hills

Прошивки и утилиты для интерактивного макета жилого комплекса **Rise Hills**: светодиодная подсветка зданий, подъёмный механизм «Building Layout» и выдвижная аллея. Все узлы построены на **ESP32** и общаются между собой по **ESP-NOW**; управление идёт через Master по USB Serial или Bluetooth.

## Архитектура

```
              USB Serial / Bluetooth
                       │
               ┌───────▼───────┐
               │    Master     │  ESP32
               └───────┬───────┘
                       │ ESP-NOW
     ┌─────────────┬───┴─────────┬──────────────┐
     ▼             ▼             ▼              ▼
 LED Slave ×4   Building      Alley          (LED TEST —
 blue / red /   Layout        Stepper         десктоп-приложение
 yellow/orange  (3 шаговых    (шаговый +      для отправки команд
 RGBW-лента     двигателя)    LED-лента)      через Serial)
```

## Структура репозитория

| Папка | Описание |
|---|---|
| `Rise Hills Msater/` | Master-контроллер: принимает команды по Serial/Bluetooth и рассылает их слейвам через ESP-NOW (`src/Master.cpp`) |
| `Rise Hills slave/` | LED-слейв: RGBW-лента NeoPixel (до 3700 пикселей), неблокирующие эффекты (`src/Slave.cpp`) |
| `Rise Hills Building Layout Slave/` | Подъём/опускание макета здания: 3 шаговых двигателя (AccelStepper), концевики, настройки в Preferences (`src/Building.cpp`) |
| `Rise Hills Aleya Slave/` | Выдвижная аллея: шаговый двигатель с концевиком + LED-подсветка (`src/Alley.cpp`) |
| `LED TEST/` | Десктоп-приложение на PyQt6 для управления системой через Serial (`esp_tester.py`, раскладка `building.json`) |
| `CUT/` | Чертежи для резки (DWG/DXF/CDR): потолок, верхняя LED-подсветка, крепления блоков питания |
| `Комагды.rtf` | Шпаргалка по командам Master |

## Железо

- Плата: ESP32 (`upesy_wroom`)
- LED: адресные RGBW-ленты (NeoPixel, `NEO_GRBW`)
- Механика: шаговые двигатели через драйверы STEP/DIR (TMC), концевики

## Сборка и прошивка

Каждая папка с прошивкой — отдельный проект [PlatformIO](https://platformio.org/). Удобнее всего открыть `Rise hills.code-workspace` в VS Code с расширением PlatformIO.

```bash
cd "Rise Hills Msater"
pio run -t upload
pio device monitor   # 115200 бод
```

Перед прошивкой:

1. **LED-слейвы** — в `Slave.cpp` задать `MY_DEVICE_ID` (`blue` | `red` | `yellow` | `orange`) для каждого устройства.
2. **Master** — прописать MAC-адреса слейвов в `ledDevices[]`, `buildingMAC` и `alleyMAC`. Каждый слейв печатает свой MAC в Serial при загрузке.

## LED TEST (десктоп-приложение)

```bash
cd "LED TEST"
python3 -m venv .venv && source .venv/bin/activate
pip install PyQt6 pyserial
python esp_tester.py
```

## Команды Master

### LED-устройства

Глобальные (на все 4 устройства):

| Команда | Описание |
|---|---|
| `off` | выключить все |
| `on` / `on white` | включить все (можно указать цвет) |
| `allOn` | включить все белым |
| `random` | случайный эффект |
| `brightness 128` | яркость 0–255 |
| `fill red` | залить цветом |
| `blink white 500 10` | мигание: цвет, период мс, раз |
| `breath blue 50 8` | дыхание: цвет, шагов, пауза мс |
| `wipe cyan 5` | волна: цвет, пауза мс |
| `chase white 30 2` | бегущий огонь: цвет, хвост, пауза мс |
| `sparkle yellow 20 60 15` | искры: цвет, кол-во, пауза мс, итераций |
| `strobe white 50 50 8` | строб: цвет, вкл мс, выкл мс, раз |
| `sections 4` | разделить ленту на N секций |
| `reffect 0 500 chase white 30 2` | эффект на диапазоне пикселей |
| `range 0 500 red` | покрасить диапазон пикселей |

Для конкретного устройства — префикс с его именем:

```
blue on red
red fill cyan
yellow solo                         # yellow вкл, остальные выкл
orange reffect 0 500 chase white 30 2
blue range 0 200 red
```

Цвета: `white red green blue yellow cyan magenta orange purple warm off` или RGB(W): `255,100,0,0`.

### Building Layout (лифт)

| Команда | Описание |
|---|---|
| `building down` | опустить: M1 → пауза → M3 → пауза → M2 |
| `building up` | поднять: M2 → пауза → M3 → пауза → M1 |
| `building stop` | аварийная остановка |

### Alley (аллея)

| Команда | Описание |
|---|---|
| `alley 1` | POS1: убрать (LED выключается) |
| `alley 2` | POS2: выдвинуть (LED + бегущий огонь) |
| `alley help` | статус и конфигурация |

### Системные

| Команда | Описание |
|---|---|
| `status` | список устройств с MAC-адресами |
| `help` | справка |
| `all <команда>` | явно отправить команду всем LED-устройствам |
