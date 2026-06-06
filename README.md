# Telegram System Monitor Bot

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Telegram Bot API](https://img.shields.io/badge/Telegram-Bot%20API-26A5E4.svg)](https://core.telegram.org/bots/api)

Легковесный Telegram-бот для мониторинга Raspberry Pi и Linux-сервера. Бот показывает системную телеметрию, диски, сеть, состояние `systemd`-сервисов с именами вида `bot-*.service`, умеет перезапускать эти сервисы кнопками и постоянно обновляет live-экран через редактирование одного Telegram-сообщения.

## Возможности

- Отчет по серверу: температура CPU в `°C`, load average, RAM, swap, частота CPU, локальный IP, throttling и uptime.
- Информативный статус с уровнями `OK/WARN/CRIT`: температура, нагрузка, RAM, диск и состояние bot-сервисов.
- Гистерезис уровней (`LEVEL_HYSTERESIS`) для защиты от "дребезга" на порогах.
- Alarm-сообщения по переходам уровней `OK/WARN/CRIT` для температуры, нагрузки, RAM, диска и сервисов с автоудалением через 30 секунд.
- Health score `0..100` в основном экране.
- История замеров с кнопкой `HISTORY` и командой `/history`.
- Отдельный красивый отчет по дискам: общий размер, занято, свободно и текстовый progress bar.
- Отдельный сетевой отчет: hostname, IP, ping до `1.1.1.1`, входящий и исходящий трафик по интерфейсам.
- Отображение отчетов в Telegram как `cpp` code block.
- Текстовые прогресс-бары для RAM, swap и дисков.
- Inline-кнопки `Обновить`, `SERVICES`, `DISK`, `NET`, `HISTORY` и опционально `POWER`.
- Команды `/start`, `/status`, `/services`, `/disk`, `/net`, `/history`, `/power`.
- Отслеживание температуры, load average, RAM, дисков и упавших `bot-*` сервисов с сохранением предыдущего уровня в state-файле.
- Опциональный restart `bot-*.service` через команду `/restart` и кнопки в экране `SERVICES`.
- Пагинация кнопок restart, чтобы список сервисов оставался удобным при большом количестве ботов.
- Опциональное управление питанием Raspberry Pi: перезагрузка и выключение с экраном подтверждения.
- Хранение `BOT_TOKEN` и `CHAT_ID` только в переменных окружения.
- Проверка `CHAT_ID`: бот отвечает только разрешенному чату.
- Корректное кодирование параметров Telegram API и логирование ошибок API в stderr.
- Аккуратное завершение по `SIGINT` и `SIGTERM`.
- Мини-самотесты критичной логики при старте (уровни и расчет памяти сервисов).

## Как это работает

При запуске бот отправляет в указанный Telegram-чат сообщение с телеметрией и кнопками:

- `Обновить` заново собирает данные и обновляет текущий отчет.
- `SERVICES` показывает статус сервисов `bot-*.service`.
- `DISK` показывает использование уникальных файловых систем для `/`, `/home` и `/var/log`.
- `NET` показывает hostname, IP, ping до `1.1.1.1` и накопленный RX/TX по сетевым интерфейсам.
- `HISTORY` показывает последние сохраненные замеры: время, health, temp/load/ram/disk.
- `POWER` открывает меню перезагрузки и выключения, если включен `BOT_ALLOW_POWER_CONTROL=1`.

Дальше процесс постоянно опрашивает Telegram Bot API через `getUpdates`. При нажатии кнопки и при автообновлении бот редактирует уже существующее сообщение через `editMessageText`, поэтому чат не засоряется повторяющимися отчетами. При смене уровня состояния бот отправляет короткое `ALARM`-сообщение и удаляет его через `ALARM_TTL_SEC` секунд.

Текстовые команды отправляют новый отчет в чат. Это удобно, если нужно заново создать панель командой `/start`.

Пример системного статуса:

```text
Status: Temp OK, Load OK, RAM OK, Disk 3% OK, Services OK
```

Если есть проблема, статус становится конкретным:

```text
Status: Temp WARN, Load OK, RAM CRIT, Disk 92% WARN, Services failed: 1
```

## Требования

- Linux или Raspberry Pi OS.
- C++17-совместимый компилятор.
- `libcurl`.
- `nlohmann/json`.
- `systemd`, если нужен отчет по сервисам.
- `ping`, если нужен сетевой health-check.
- `vcgencmd` на Raspberry Pi для частоты CPU. На обычном Linux-сервере это поле может быть `N/A`.

## Установка зависимостей

Debian, Ubuntu, Raspberry Pi OS:

```bash
sudo apt-get update
sudo apt-get install -y g++ libcurl4-openssl-dev nlohmann-json3-dev iputils-ping
```

## Создание Telegram-бота

1. Откройте `@BotFather` в Telegram.
2. Отправьте команду `/newbot`.
3. Создайте имя и username бота.
4. Сохраните выданный токен. Он понадобится как `BOT_TOKEN`.
5. Узнайте свой chat id, например через `@userinfobot`. Он понадобится как `CHAT_ID`.

## Сборка

```bash
g++ -std=c++17 SMB.cpp -o system_monitor_bot -lcurl -lpthread
```

## Запуск

```bash
export BOT_TOKEN="токен_от_BotFather"
export CHAT_ID="ваш_chat_id"

./system_monitor_bot
```

После запуска бот сразу отправит первое сообщение с отчетом.

## Команды

| Команда | Описание |
|---------|----------|
| `/start` | Отправить новую панель мониторинга |
| `/status` | Показать системную телеметрию |
| `/services` | Показать `bot-*.service` |
| `/disk` | Показать использование дисков |
| `/net` | Показать сетевой отчет |
| `/history` | Показать последние замеры (история телеметрии) |
| `/power` | Открыть меню перезагрузки и выключения, если включен `BOT_ALLOW_POWER_CONTROL=1` |
| `/restart bot-name.service` | Перезапустить сервис, если включен `BOT_ALLOW_SERVICE_CONTROL=1` |

## Отчет по дискам

Кнопка `DISK` показывает понятный блок по каждой уникальной файловой системе:

```text
Disk usage

/
[■·········] 3% used
Total     687.0G
Used      16.0G
Free      639.0G
```

Бот проверяет mountpoint'ы `/`, `/home` и `/var/log`, но если они находятся на одном физическом разделе, показывает его один раз. Это убирает дубли вроде трех одинаковых строк для одного диска.

## Отчет по сети

Кнопка `NET` показывает:

```text
Network report

HOST        > raspberrypi
IP          > 192.168.1.10
PING 1.1.1.1> OK

Traffic since interface start

IFACE     RX IN         TX OUT
wlan0     1.5GB         320.4MB
```

`RX IN` и `TX OUT` — это накопленный трафик с момента старта интерфейса, а не текущая скорость.

## Отчет по сервисам

Кнопка `SERVICES` показывает только сервисы, имена которых начинаются с `bot-`:

```text
bot-example.service
bot-worker.service
bot-notifier.service
```

Для активных сервисов бот пытается получить потребление памяти через `systemctl show ... MemoryCurrent`. Если systemd не возвращает значение, в отчете показывается `N/A`, чтобы экран `SERVICES` не тормозил из-за дополнительных fallback-запросов.

Если включен `BOT_ALLOW_SERVICE_CONTROL=1`, под отчетом по сервисам появляются кнопки restart. Кнопки разбиваются на страницы по 5 сервисов:

```text
Restart bot-example.service
Restart bot-worker.service
Restart bot-notifier.service

<   Page 1/2   >
Обновить   STATUS
DISK       NET
HISTORY
```

Restart сервисов по умолчанию отключен. Чтобы включить его, задайте:

```bash
export BOT_ALLOW_SERVICE_CONTROL=1
```

Даже в этом режиме бот принимает только имена вида `bot-*.service`, проверяет что unit существует в списке systemd и отвечает только разрешенному `CHAT_ID`.

Если монитор-бот запущен не от root, пользователю сервиса нужно дать право на restart без интерактивного пароля. Например, если unit запускается как `User=entropia`, создайте sudoers-файл:

```bash
sudo visudo -f /etc/sudoers.d/system_monitor_bot
```

И добавьте:

```text
entropia ALL=(root) NOPASSWD: /usr/bin/systemctl restart bot-*.service
```

Проверьте путь к `systemctl` командой `command -v systemctl`; на большинстве Debian/Ubuntu/Raspberry Pi OS это `/usr/bin/systemctl`.

## Управление питанием

Кнопка `POWER` по умолчанию скрыта. Чтобы включить управление перезагрузкой и выключением Raspberry Pi, задайте:

```bash
export BOT_ALLOW_POWER_CONTROL=1
```

После этого в основной клавиатуре появится кнопка `POWER`. Она открывает меню:

```text
Перезагрузка   Выключение
STATUS         SERVICES
DISK           NET
HISTORY
```

Нажатие `Перезагрузка` или `Выключение` сначала показывает экран подтверждения. Только кнопка `Да, выполнить` запускает команду:

```bash
sudo -n systemctl reboot
sudo -n systemctl poweroff
```

Если монитор-бот запущен не от root, пользователю сервиса нужно дать права без интерактивного пароля:

```bash
sudo visudo -f /etc/sudoers.d/system_monitor_bot
```

Пример для пользователя `entropia`:

```text
entropia ALL=(root) NOPASSWD: /usr/bin/systemctl reboot
entropia ALL=(root) NOPASSWD: /usr/bin/systemctl poweroff
```

Не включайте `BOT_ALLOW_POWER_CONTROL` для публичных или общих чатов. Бот проверяет `CHAT_ID`, но команды питания все равно лучше держать доступными только владельцу сервера.

## Переменные окружения

| Переменная | Описание |
|------------|----------|
| `BOT_TOKEN` | Токен Telegram-бота от `@BotFather` |
| `CHAT_ID` | ID чата, куда бот отправляет отчет |
| `TEMP_WARN` | Порог температуры, по умолчанию `75` |
| `TEMP_CRIT` | Критический порог температуры, по умолчанию `85` |
| `LOAD_WARN` | Порог load average за 1 минуту, по умолчанию `3.5` |
| `LOAD_CRIT` | Критический порог load average за 1 минуту, по умолчанию `5.0` |
| `RAM_WARN` | Порог RAM в процентах, по умолчанию `80` |
| `RAM_CRIT` | Критический порог RAM в процентах, по умолчанию `90` |
| `DISK_WARN` | Порог заполнения диска в процентах, по умолчанию `90` |
| `DISK_CRIT` | Критический порог заполнения диска в процентах, по умолчанию `95` |
| `LEVEL_HYSTERESIS` | Гистерезис для уровней `WARN/CRIT`, по умолчанию `2.0` |
| `HISTORY_SIZE` | Размер истории замеров, по умолчанию `120` (ограничено `10..500`) |
| `POLL_TIMEOUT_SEC` | Таймаут long polling Telegram, по умолчанию `20` |
| `AUTO_REFRESH_SEC` | Автообновление dashboard-сообщения, по умолчанию `30` |
| `ALARM_TTL_SEC` | Через сколько секунд удалять alarm-сообщения, по умолчанию `30` |
| `BOT_ALLOW_SERVICE_CONTROL` | Разрешить restart `bot-*.service` командами и кнопками, по умолчанию выключено |
| `BOT_ALLOW_POWER_CONTROL` | Показать `POWER` и разрешить reboot/poweroff через подтверждение, по умолчанию выключено |
| `BOT_SELF_UNIT` | Имя собственного unit-файла бота для блокировки self-restart (опционально) |

## Пример systemd unit

```ini
[Unit]
Description=Telegram System Monitor Bot
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/SystemMonitorBot
ExecStart=/opt/SystemMonitorBot/system_monitor_bot
Environment=BOT_TOKEN=your_bot_token
Environment=CHAT_ID=your_chat_id
Environment=TEMP_WARN=75
Environment=TEMP_CRIT=85
Environment=RAM_WARN=80
Environment=RAM_CRIT=90
Environment=DISK_WARN=90
Environment=DISK_CRIT=95
Environment=LEVEL_HYSTERESIS=2
Environment=HISTORY_SIZE=120
Environment=AUTO_REFRESH_SEC=30
Environment=ALARM_TTL_SEC=30
# Environment=BOT_ALLOW_SERVICE_CONTROL=1
# Environment=BOT_ALLOW_POWER_CONTROL=1
# Environment=BOT_SELF_UNIT=bot-monitor.service
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

## Примечания

- Температура читается из `/sys/class/thermal/thermal_zone0/temp`.
- Частота CPU читается через `vcgencmd measure_clock arm`.
- Raspberry Pi throttling читается через `vcgencmd get_throttled`.
- Использование дисков читается через `statvfs`.
- Сетевой трафик читается из `/proc/net/dev`.
- Список сервисов читается через `systemctl list-units --type=service --all`.
- Перезапуск сервисов выполняется командой `sudo -n systemctl restart bot-name.service`.
- Перезагрузка и выключение выполняются командами `sudo -n systemctl reboot` и `sudo -n systemctl poweroff`.
- История и состояние уровней сохраняются в `/tmp/systemmonitorbot-*.state`.
- Команды оболочки выполняются с таймаутом и retry, чтобы не блокировать основной цикл бота.
- Бот логирует ошибки Telegram API в stderr.
