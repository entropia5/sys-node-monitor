 

Telegram System Monitor RPi-4 

Легковесный мониторинг системных ресурсов для Raspberry Pi 4 (и других Linux-серверов) на C++, работающий через Telegram Bot API.

/*
High-performance system monitoring bot for RPi4/Linux, written in C++17.
Легковесный монитор системных ресурсов и статуса Systemd-сервисов через Telegram Bot API.
*/

 Основные функции
- Телеметрия в реальном времени: Температура CPU, Load Average, RAM/Swap, частота CPU и локальный IP.Используется структура sysinfo для получения данных о RAM, Swap и аптайме. Температура считывается напрямую     из файловой системы Linux (/sys/class/thermal/...),это эффективнее, чем вызов внешних утилит.
- Просмотр статуса (OK/FAIL) всех активных Systemd-юнитов.
  Реализована функция bar, которая рисует графический прогресс-бар в текстовом виде (псевдографика). Это значительно улучшает читаемость отчета в Telegram. Используется std::setw и std::left для выравнивания      текста "колонками", что в моноширинном шрифте (MarkdownV2) выглядит аккуратно.
- Мгновенное обновление данных через Inline-кнопки.Использование inline_keyboard и callback_query позволяет обновлять данные или переключать режимы без засорения чата новыми сообщениями (через editMessageText)
- Безопасность: Полная изоляция учетных данных через переменные окружения std::getenv.

Стек
- C++17
- Lib: `libcurl` (сетевые запросы), `nlohmann/json` (обработка данных).
- Система: POSIX / Linux API
  


Запуск.

1.1 Установка зависимостей:

sudo apt-get update
sudo apt-get install libcurl4-openssl-dev nlohmann-json3-dev g++

1.2 Регистрация бота в @BotFather:
- вводим в поиске телеграм @BotFather
- /newbot
- Name: Вводим имя бота (например, My RPi Monitor).
- Username: Вводим уникальный адрес бота, оканчивающийся на bot (например, sys_node_monitor_bot)
- Token: После этого BotFather отобразит длинную строку (наш токен, который нужен для дальнейшей настройки).
- в поиске также ищем бота @userinfobot(наш id, который также нужен для дальнейшей настройки)
- Находим. Запускаем (Start) и он пришлет id. Это наш CHAT_ID.

2.1 Открываем наш исходник SMB.cpp
2.2 Компиляция:
используем флаг -std=c++17 и линковку curl и pthread:
g++ -std=c++17 SMB.cpp -o system_monitor_bot -lcurl -lpthread

3.Настройка окружения (.env):
Проект не хранит токены в коде. Для разового запуска(проверки) в терминале сервера:
/*

export BOT_TOKEN="ваш_токен"
export CHAT_ID="ваш_id"
./system_monitor_bot

*/


4.Автозапуск (Systemd):

- Создаем и редактируем на сервере RPi system_monitor.service
 (sudo nano /etc/systemd/system/system_monitor_bot.service)
/*

[Unit]
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


*/

 
