# 📊 Telegram System Monitor Bot

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

**Легковесный мониторинг системных ресурсов для Raspberry Pi 4 и Linux-серверов на C++**

Бот предоставляет информацию о состоянии сервера через Telegram Bot API: температуру CPU, нагрузку, память, статус systemd-сервисов и многое другое.

---

## ✨ Возможности

### 📈 **Реальная телеметрия**
- 🌡️ **Температура CPU** — чтение напрямую из `/sys/class/thermal/`
- 📊 **Load Average** — 1, 5, 15 минут
- 🧠 **RAM и Swap** — использование и доступная память
- ⚡ **Частота процессора** — текущая тактовая частота
- 🌐 **Локальный IP адрес** — автоматическое определение

### 🔧 **Управление сервисами**
- Просмотр статуса **(✅ OK / ❌ FAIL)** всех активных systemd-юнитов
- **Графический прогресс-бар** в текстовом виде (псевдографика) для наглядности
- Аккуратное форматирование колонками с помощью `std::setw` и `std::left`

### 🎮 **Интерактив**
- **Inline-кнопки** — обновление данных без засорения чата
- Мгновенное обновление через `editMessageText`

### 🔒 **Безопасность**
- Полная изоляция учетных данных через переменные окружения
- Никаких токенов в коде или системе контроля версий

---

## 🛠️ Стек технологий

| Компонент | Технология |
|-----------|------------|
| Язык | C++17 |
| HTTP-запросы | libcurl |
| JSON | nlohmann/json |
| Системные вызовы | POSIX / Linux API |
| Сборка | g++ / CMake |

---

## 📥 Установка и запуск

### 1. Установка зависимостей

```bash
sudo apt-get update
sudo apt-get install -y libcurl4-openssl-dev nlohmann-json3-dev g++



2. Регистрация бота в Telegram

    Найдите @BotFather в Telegram

    Отправьте команду /newbot

    Введите имя бота (например, My RPi Monitor)

    Введите уникальный username, оканчивающийся на bot (например, sys_node_monitor_bot)

    Сохраните полученный токен — он понадобится для настройки

    Найдите @userinfobot, запустите его и получите свой CHAT_ID




3. Компиляция.

# Сборка напрямую через g++
g++ -std=c++17 SMB.cpp -o system_monitor_bot -lcurl -lpthread

# Или через CMake
mkdir build && cd build
cmake ..
make


4. Настройка окружения.

export BOT_TOKEN="ваш_токен_от_BotFather"
export CHAT_ID="ваш_id_от_userinfobot"
./system_monitor_bot




