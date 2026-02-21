 

Telegram System Monitor RPi-4 

Легковесный мониторинг системных ресурсов для Raspberry Pi 4 (и других Linux-серверов) на C++, работающий через Telegram Bot API.

/*
High-performance system monitoring bot for RPi4/Linux, written in C++17.
Легковесный монитор системных ресурсов и статуса Systemd-сервисов через Telegram Bot API.
*/

 Основные функции
- Телеметрия в реальном времени: Температура CPU, Load Average, RAM/Swap, частота CPU и локальный IP.
- Просмотр статуса (OK/FAIL) всех активных Systemd-юнитов.
- Мгновенное обновление данных через Inline-кнопки.
- Безопасность: Полная изоляция учетных данных через переменные окружения.

Стек
- C++17
- Lib: `libcurl` (сетевые запросы), `nlohmann/json` (обработка данных).
- Система: POSIX / Linux API
  


Запуск.

1.Установка зависимостей:

sudo apt-get update
sudo apt-get install libcurl4-openssl-dev nlohmann-json3-dev g++


2.Компиляция:
используем флаг -std=c++17 и линковку curl и pthread:
g++ -std=c++17 SMB.cpp -o system_monitor_bot -lcurl -lpthread

3.Настройка окружения (.env):
Проект не хранит токены в коде. Для разового запуска используйте export:

export BOT_TOKEN="ваш_токен"
export CHAT_ID="ваш_id"
./system_monitor_bot


4.Автозапуск (Systemd):

1. Создаем и редактируем на сервере RPi system_monitor.service
 (sudo nano /etc/systemd/system/system_monitor_bot.service)

2. [Unit]
Description=Telegram System Monitor Bot
After=network.target

[Service]
Type=simple
User=root

// Укажите путь к папке с бинарником
WorkingDirectory=/path/to/your/folder 

// Переменные окружения прямо в сервисе
Environment="BOT_TOKEN=44444:AAFh..."
Environment="CHAT_ID= 4444444444"
ExecStart=/path/to/your/folder/system_monitor_bot
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target

3.Активация сервиса:
sudo systemctl daemon-reload
sudo systemctl enable system_monitor_bot.service
sudo systemctl start system_monitor_bot.service




 
