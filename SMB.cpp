#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

using json = nlohmann::json;

std::atomic<bool> running(true);

struct Config {
    std::string token;
    std::string chatId;
    double tempWarn = 75.0;
    double loadWarn = 3.5;
    double ramWarn = 90.0;
    double diskWarn = 90.0;
    int pollTimeoutSec = 20;
    int alertIntervalSec = 300;
    int autoRefreshSec = 0;
    bool allowServiceControl = false;
    bool allowPowerControl = false;
};

struct ApiResponse {
    bool ok = false;
    long httpCode = 0;
    std::string body;
};

struct Telemetry {
    double temp = 0.0;
    double load[3] = {0.0, 0.0, 0.0};
    double ramPercent = 0.0;
    double swapPercent = 0.0;
    long uptime = 0;
    std::string cpuFreq;
    std::string localIp;
    std::string throttled;
    std::string status;
};

struct DiskInfo {
    std::string mount;
    unsigned long fsId = 0;
    double usedPercent = 0.0;
    double totalGb = 0.0;
    double usedGb = 0.0;
    double freeGb = 0.0;
    bool ok = false;
};

size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

void handleSignal(int) {
    running = false;
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string exec(const char* cmd) {
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return trim(result);
}

double envDouble(const char* name, double fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    try {
        return std::stod(value);
    } catch (...) {
        std::cerr << "Invalid " << name << ", using default " << fallback << "\n";
        return fallback;
    }
}

int envInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        std::cerr << "Invalid " << name << ", using default " << fallback << "\n";
        return fallback;
    }
}

bool envBool(const char* name, bool fallback = false) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

Config loadConfig() {
    const char* tokenEnv = std::getenv("BOT_TOKEN");
    const char* chatIdEnv = std::getenv("CHAT_ID");
    if (!tokenEnv || !chatIdEnv) {
        throw std::runtime_error("BOT_TOKEN and CHAT_ID are required");
    }

    Config cfg;
    cfg.token = tokenEnv;
    cfg.chatId = chatIdEnv;
    cfg.tempWarn = envDouble("TEMP_WARN", cfg.tempWarn);
    cfg.loadWarn = envDouble("LOAD_WARN", cfg.loadWarn);
    cfg.ramWarn = envDouble("RAM_WARN", cfg.ramWarn);
    cfg.diskWarn = envDouble("DISK_WARN", cfg.diskWarn);
    cfg.pollTimeoutSec = envInt("POLL_TIMEOUT_SEC", cfg.pollTimeoutSec);
    cfg.alertIntervalSec = envInt("ALERT_INTERVAL_SEC", cfg.alertIntervalSec);
    cfg.autoRefreshSec = envInt("AUTO_REFRESH_SEC", cfg.autoRefreshSec);
    cfg.allowServiceControl = envBool("BOT_ALLOW_SERVICE_CONTROL");
    cfg.allowPowerControl = envBool("BOT_ALLOW_POWER_CONTROL");
    return cfg;
}

std::string markdownEscape(const std::string& text) {
    std::string out;
    const std::string special = "_*[]()~`>#+-=|{}.!";
    for (char c : text) {
        if (special.find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

std::string codeBlock(const std::string& text) {
    return "```cpp\n" + markdownEscape(text) + "\n```";
}

std::string bar(double percent) {
    int filled = static_cast<int>(percent / 10.0);
    filled = std::max(0, std::min(10, filled));

    std::string value = "[";
    for (int i = 0; i < 10; ++i) {
        value += (i < filled) ? "■" : "·";
    }
    value += "]";
    return value;
}

std::string telegramChatId(const json& chat) {
    if (chat.contains("id")) {
        if (chat["id"].is_string()) return chat["id"].get<std::string>();
        if (chat["id"].is_number_integer()) return std::to_string(chat["id"].get<long long>());
    }
    return "";
}

bool isAuthorizedChat(const json& chat, const Config& cfg) {
    return telegramChatId(chat) == cfg.chatId;
}

std::string urlencode(CURL* curl, const std::string& value) {
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!encoded) return "";
    std::string result(encoded);
    curl_free(encoded);
    return result;
}

std::string formEncode(CURL* curl, const std::map<std::string, std::string>& params) {
    std::ostringstream body;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!first) body << "&";
        first = false;
        body << urlencode(curl, key) << "=" << urlencode(curl, value);
    }
    return body.str();
}

ApiResponse callApi(
    const std::string& method,
    const std::map<std::string, std::string>& params,
    const Config& cfg
) {
    ApiResponse result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "curl_easy_init failed\n";
        return result;
    }

    std::string url = "https://api.telegram.org/bot" + cfg.token + "/" + method;
    std::string body = formEncode(curl, params);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::cerr << "Telegram API " << method << " failed: " << curl_easy_strerror(rc) << "\n";
        return result;
    }

    result.ok = result.httpCode >= 200 && result.httpCode < 300;
    if (!result.ok) {
        std::cerr << "Telegram API " << method << " HTTP " << result.httpCode << ": " << result.body << "\n";
        return result;
    }

    try {
        json parsed = json::parse(result.body);
        result.ok = parsed.value("ok", false);
        if (!result.ok) {
            std::cerr << "Telegram API " << method << " error: " << result.body << "\n";
        }
    } catch (...) {
        std::cerr << "Telegram API " << method << " returned non-JSON response\n";
        result.ok = false;
    }
    return result;
}

ApiResponse getUpdates(int offset, const Config& cfg) {
    ApiResponse result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "curl_easy_init failed\n";
        return result;
    }

    std::string url = "https://api.telegram.org/bot" + cfg.token + "/getUpdates";
    url += "?offset=" + std::to_string(offset);
    url += "&timeout=" + std::to_string(cfg.pollTimeoutSec);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg.pollTimeoutSec + 10L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::cerr << "getUpdates failed: " << curl_easy_strerror(rc) << "\n";
        return result;
    }
    result.ok = result.httpCode >= 200 && result.httpCode < 300;
    if (!result.ok) {
        std::cerr << "getUpdates HTTP " << result.httpCode << ": " << result.body << "\n";
    }
    return result;
}

std::string mainKeyboardJson(const Config& cfg) {
    json rows = json::array({
        json::array({
            {{"text", "Обновить"}, {"callback_data", "status"}},
            {{"text", "SERVICES"}, {"callback_data", "services"}}
        }),
        json::array({
            {{"text", "DISK"}, {"callback_data", "disk"}},
            {{"text", "NET"}, {"callback_data", "net"}}
        })
    });

    if (cfg.allowPowerControl) {
        rows.push_back(json::array({
            {{"text", "POWER"}, {"callback_data", "power"}}
        }));
    }

    json keyboard = {{"inline_keyboard", rows}};
    return keyboard.dump();
}

std::string servicesKeyboardJson(std::size_t page);
std::string keyboardForView(const std::string& view, const Config& cfg);
std::string powerKeyboardJson();
std::string powerConfirmKeyboardJson(const std::string& action);
std::vector<DiskInfo> collectDisks();
std::vector<std::string> failedBotUnits();

constexpr std::size_t SERVICES_PER_PAGE = 5;

Telemetry collectTelemetry(const Config& cfg) {
    Telemetry t;
    struct sysinfo mem {};
    if (sysinfo(&mem) == 0) {
        unsigned long long totalRam = mem.totalram * static_cast<unsigned long long>(mem.mem_unit);
        unsigned long long freeRam = mem.freeram * static_cast<unsigned long long>(mem.mem_unit);
        unsigned long long totalSwap = mem.totalswap * static_cast<unsigned long long>(mem.mem_unit);
        unsigned long long freeSwap = mem.freeswap * static_cast<unsigned long long>(mem.mem_unit);

        if (totalRam > 0) t.ramPercent = 100.0 * (totalRam - freeRam) / totalRam;
        if (totalSwap > 0) t.swapPercent = 100.0 * (totalSwap - freeSwap) / totalSwap;
        t.uptime = mem.uptime;
    }

    std::ifstream tempFile("/sys/class/thermal/thermal_zone0/temp");
    if (tempFile >> t.temp) t.temp /= 1000.0;

    getloadavg(t.load, 3);

    std::string cpuFreq = exec("vcgencmd measure_clock arm 2>/dev/null | cut -d= -f2");
    if (cpuFreq.empty()) {
        t.cpuFreq = "N/A";
    } else {
        try {
            long long hz = std::stoll(cpuFreq);
            t.cpuFreq = std::to_string(hz / 1000000) + " MHz";
        } catch (...) {
            t.cpuFreq = cpuFreq;
        }
    }

    t.localIp = exec("hostname -I | awk '{print $1}'");
    if (t.localIp.empty()) t.localIp = "N/A";

    t.throttled = exec("vcgencmd get_throttled 2>/dev/null | cut -d= -f2");
    if (t.throttled.empty()) t.throttled = "N/A";

    return t;
}

std::string telemetryStatus(const Telemetry& t, const Config& cfg) {
    std::vector<std::string> parts;

    parts.push_back(t.temp >= cfg.tempWarn ? "Temp high" : "Temp OK");
    parts.push_back(t.load[0] >= cfg.loadWarn ? "Load high" : "Load OK");
    parts.push_back(t.ramPercent >= cfg.ramWarn ? "RAM high" : "RAM OK");

    double maxDisk = 0.0;
    for (const DiskInfo& disk : collectDisks()) {
        maxDisk = std::max(maxDisk, disk.usedPercent);
    }
    std::ostringstream diskPart;
    diskPart << "Disk " << static_cast<int>(maxDisk) << "%";
    if (maxDisk >= cfg.diskWarn) diskPart << " high";
    parts.push_back(diskPart.str());

    std::vector<std::string> failed = failedBotUnits();
    if (failed.empty()) {
        parts.push_back("Services OK");
    } else {
        parts.push_back("Services failed: " + std::to_string(failed.size()));
    }

    std::ostringstream status;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) status << ", ";
        status << parts[i];
    }
    return status.str();
}

std::string formatTelemetry(const Config& cfg) {
    Telemetry t = collectTelemetry(cfg);

    std::ostringstream oss;
    oss << "System RPi Server Report:\n\n";
    oss << std::left << std::setw(12) << "TEMP" << "> " << std::fixed << std::setprecision(1) << t.temp << " °C\n";
    oss << std::left << std::setw(12) << "LOAD" << "> " << t.load[0] << " / " << t.load[1] << " / " << t.load[2] << "\n";
    oss << std::left << std::setw(12) << "RAM" << "> " << bar(t.ramPercent) << " " << static_cast<int>(t.ramPercent) << "%\n";
    oss << std::left << std::setw(12) << "SWAP" << "> " << bar(t.swapPercent) << " " << static_cast<int>(t.swapPercent) << "%\n";
    oss << std::left << std::setw(12) << "CPU" << "> " << t.cpuFreq << "\n";
    oss << std::left << std::setw(12) << "IP" << "> " << t.localIp << "\n";
    oss << std::left << std::setw(12) << "THROTTLED" << "> " << t.throttled << "\n";
    oss << std::left << std::setw(12) << "UPTIME" << "> " << t.uptime / 3600 << "H " << (t.uptime % 3600) / 60 << "M\n\n";
    oss << "Status: " << telemetryStatus(t, cfg);

    return codeBlock(oss.str());
}

DiskInfo readDisk(const std::string& mount) {
    DiskInfo info;
    info.mount = mount;

    struct statvfs fs {};
    if (statvfs(mount.c_str(), &fs) != 0 || fs.f_blocks == 0) {
        return info;
    }

    double total = static_cast<double>(fs.f_blocks) * fs.f_frsize;
    double available = static_cast<double>(fs.f_bavail) * fs.f_frsize;
    double used = total - available;

    info.usedPercent = total > 0 ? 100.0 * used / total : 0.0;
    info.totalGb = total / 1024.0 / 1024.0 / 1024.0;
    info.usedGb = used / 1024.0 / 1024.0 / 1024.0;
    info.freeGb = available / 1024.0 / 1024.0 / 1024.0;
    info.fsId = static_cast<unsigned long>(fs.f_fsid);
    info.ok = true;
    return info;
}

std::vector<DiskInfo> collectDisks() {
    std::vector<std::string> mounts = {"/", "/home", "/var/log"};
    std::vector<DiskInfo> disks;
    std::set<std::string> seen;
    std::set<unsigned long> seenFilesystems;

    for (const auto& mount : mounts) {
        if (!seen.insert(mount).second) continue;
        DiskInfo info = readDisk(mount);
        if (info.ok && seenFilesystems.insert(info.fsId).second) disks.push_back(info);
    }
    return disks;
}

std::string formatDiskReport() {
    std::ostringstream oss;
    oss << "Disk usage:\n\n";

    for (const DiskInfo& disk : collectDisks()) {
        oss << disk.mount << "\n";
        oss << bar(disk.usedPercent) << " " << static_cast<int>(disk.usedPercent) << "% used\n";
        oss << std::fixed << std::setprecision(1);
        oss << std::left << std::setw(10) << "Total" << disk.totalGb << "G\n";
        oss << std::left << std::setw(10) << "Used" << disk.usedGb << "G\n";
        oss << std::left << std::setw(10) << "Free" << disk.freeGb << "G\n\n";
    }

    return codeBlock(oss.str());
}

std::string formatBytes(double bytes) {
    const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
    int idx = 0;
    while (bytes >= 1024.0 && idx < 4) {
        bytes /= 1024.0;
        ++idx;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(idx == 0 ? 0 : 1) << bytes << suffixes[idx];
    return oss.str();
}

std::string formatNetReport() {
    std::ostringstream oss;
    oss << "Network report:\n\n";
    oss << std::left << std::setw(12) << "HOST" << "> " << exec("hostname") << "\n";
    oss << std::left << std::setw(12) << "IP" << "> " << exec("hostname -I") << "\n";
    oss << std::left << std::setw(12) << "PING 1.1.1.1" << "> " << exec("ping -c 1 -W 2 1.1.1.1 >/dev/null 2>&1 && echo OK || echo FAIL") << "\n\n";
    oss << "Traffic since interface start:\n\n";
    oss << std::left << std::setw(10) << "IFACE" << std::setw(14) << "RX IN" << "TX OUT\n\n";

    std::ifstream net("/proc/net/dev");
    std::string line;
    int lineNo = 0;
    while (std::getline(net, line)) {
        if (++lineNo <= 2) continue;
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string iface = trim(line.substr(0, colon));
        if (iface == "lo") continue;

        std::istringstream values(line.substr(colon + 1));
        unsigned long long rx = 0;
        unsigned long long tx = 0;
        unsigned long long ignored = 0;
        values >> rx;
        for (int i = 0; i < 7; ++i) values >> ignored;
        values >> tx;

        oss << std::left << std::setw(10) << iface << std::setw(14) << formatBytes(static_cast<double>(rx)) << formatBytes(static_cast<double>(tx)) << "\n";
    }

    return codeBlock(oss.str());
}

bool isSafeBotUnit(const std::string& unit);

std::vector<std::string> botUnits() {
    std::string raw = exec("systemctl list-units --type=service --all --no-legend | awk '$1 ~ /^bot-/ {print $1}'");
    std::vector<std::string> units;
    std::istringstream iss(raw);
    std::string svc;
    while (std::getline(iss, svc)) {
        svc = trim(svc);
        if (!svc.empty() && isSafeBotUnit(svc)) units.push_back(svc);
    }
    std::sort(units.begin(), units.end());
    return units;
}

bool isSafeBotUnit(const std::string& unit) {
    if (unit.rfind("bot-", 0) != 0) return false;
    if (unit.size() <= 12 || unit.substr(unit.size() - 8) != ".service") return false;
    return std::all_of(unit.begin(), unit.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '@';
    });
}

std::string shortUnitName(const std::string& unit) {
    std::string display = unit;
    if (display.rfind("bot-", 0) == 0) display = display.substr(4);
    if (display.size() > 8 && display.substr(display.size() - 8) == ".service") {
        display = display.substr(0, display.size() - 8);
    }
    return display.size() > 24 ? display.substr(0, 23) + "~" : display;
}

std::size_t servicesPageFromView(const std::string& view) {
    const std::string prefix = "services:";
    if (view.rfind(prefix, 0) != 0) return 0;

    try {
        return static_cast<std::size_t>(std::stoul(view.substr(prefix.size())));
    } catch (...) {
        return 0;
    }
}

std::string serviceMemory(const std::string& unit) {
    std::string memStr = exec(("systemctl show " + unit + " -p MemoryCurrent --value").c_str());

    if (memStr == "0" || memStr == "[not set]" || memStr.empty()) {
        std::string procName = unit.substr(0, unit.size() - 8);
        std::string psCmd = "ps -C " + procName + " -o rss --no-headers | awk '{sum+=$1} END {print sum}'";
        memStr = exec(psCmd.c_str());
    }

    try {
        if (!memStr.empty() && memStr != "0") {
            long long value = std::stoll(memStr);
            double mb = (value > 2000000) ? (value / 1024.0 / 1024.0) : (value / 1024.0);
            if (mb < 0.1) mb = 0.1;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << mb << "M";
            return oss.str();
        }
    } catch (...) {
        return "N/A";
    }
    return "0.1M";
}

std::string formatServicesReport() {
    std::vector<std::string> units = botUnits();
    std::ostringstream oss;
    oss << "systemd report:\n\n";

    std::size_t unitWidth = 35;
    for (const auto& unit : units) {
        unitWidth = std::max(unitWidth, unit.size() + 2);
    }

    oss << std::left << std::setw(static_cast<int>(unitWidth)) << "UNIT" << std::setw(8) << "STATUS" << "MEM\n\n";

    int activeCount = 0;
    int failedCount = 0;
    for (const auto& unit : units) {
        std::string active = exec(("systemctl is-active " + unit).c_str());
        oss << std::left << std::setw(static_cast<int>(unitWidth)) << unit;

        if (active == "active") {
            ++activeCount;
            oss << std::setw(8) << "[OK]" << serviceMemory(unit) << "\n";
        } else if (active == "failed") {
            ++failedCount;
            oss << std::setw(8) << "[FAIL]" << "\n";
        } else {
            oss << std::setw(8) << "[OFF]" << "\n";
        }
    }

    oss << "\nUnits: " << units.size() << "  Active: " << activeCount << "  Failed: " << failedCount << "\n";
    return codeBlock(oss.str());
}

std::string servicesKeyboardJson(std::size_t page) {
    std::vector<std::string> units = botUnits();
    json rows = json::array();

    std::size_t totalPages = std::max<std::size_t>(1, (units.size() + SERVICES_PER_PAGE - 1) / SERVICES_PER_PAGE);
    page = std::min(page, totalPages - 1);
    std::size_t start = page * SERVICES_PER_PAGE;
    std::size_t end = std::min(start + SERVICES_PER_PAGE, units.size());

    for (std::size_t i = start; i < end; ++i) {
        const std::string& unit = units[i];
        std::string callback = "restart:" + std::to_string(page) + ":" + unit;
        if (callback.size() > 64) continue;

        rows.push_back(json::array({
            {
                {"text", "Restart " + unit},
                {"callback_data", callback}
            }
        }));
    }

    if (totalPages > 1) {
        std::size_t prevPage = (page == 0) ? totalPages - 1 : page - 1;
        std::size_t nextPage = (page + 1) % totalPages;
        rows.push_back(json::array({
            {{"text", "<"}, {"callback_data", "services:" + std::to_string(prevPage)}},
            {{"text", "Page " + std::to_string(page + 1) + "/" + std::to_string(totalPages)}, {"callback_data", "services:" + std::to_string(page)}},
            {{"text", ">"}, {"callback_data", "services:" + std::to_string(nextPage)}}
        }));
    }

    rows.push_back(json::array({
        {{"text", "Обновить"}, {"callback_data", "services:" + std::to_string(page)}},
        {{"text", "STATUS"}, {"callback_data", "status"}}
    }));
    rows.push_back(json::array({
        {{"text", "DISK"}, {"callback_data", "disk"}},
        {{"text", "NET"}, {"callback_data", "net"}}
    }));

    json keyboard = {{"inline_keyboard", rows}};
    return keyboard.dump();
}

std::string powerKeyboardJson() {
    json keyboard = {
        {"inline_keyboard", {
            {
                {{"text", "Перезагрузка"}, {"callback_data", "power:ask:reboot"}},
                {{"text", "Выключение"}, {"callback_data", "power:ask:poweroff"}}
            },
            {
                {{"text", "STATUS"}, {"callback_data", "status"}},
                {{"text", "SERVICES"}, {"callback_data", "services"}}
            },
            {
                {{"text", "DISK"}, {"callback_data", "disk"}},
                {{"text", "NET"}, {"callback_data", "net"}}
            }
        }}
    };
    return keyboard.dump();
}

std::string powerConfirmKeyboardJson(const std::string& action) {
    json keyboard = {
        {"inline_keyboard", {
            {
                {{"text", "Да, выполнить"}, {"callback_data", "power:confirm:" + action}},
                {{"text", "Отмена"}, {"callback_data", "power"}}
            }
        }}
    };
    return keyboard.dump();
}

std::string keyboardForView(const std::string& view, const Config& cfg) {
    if ((view == "services" || view.rfind("services:", 0) == 0) && cfg.allowServiceControl) {
        return servicesKeyboardJson(servicesPageFromView(view));
    }
    if (view == "power" && cfg.allowPowerControl) return powerKeyboardJson();
    if (view == "power:ask:reboot" && cfg.allowPowerControl) return powerConfirmKeyboardJson("reboot");
    if (view == "power:ask:poweroff" && cfg.allowPowerControl) return powerConfirmKeyboardJson("poweroff");
    return mainKeyboardJson(cfg);
}

std::string restartUnit(const std::string& unit, const Config& cfg) {
    if (!cfg.allowServiceControl) {
        return codeBlock("Service control is disabled. Set BOT_ALLOW_SERVICE_CONTROL=1 to enable it.");
    }

    if (!isSafeBotUnit(unit)) {
        return codeBlock("Refused. Only bot-*.service units are allowed.");
    }

    std::vector<std::string> units = botUnits();
    if (std::find(units.begin(), units.end(), unit) == units.end()) {
        return codeBlock("Unit not found: " + unit);
    }

    std::string result = exec(("sudo -n systemctl restart " + unit + " 2>&1 && echo OK || echo FAIL").c_str());
    return codeBlock("Restart " + unit + "\n" + result);
}

std::string powerActionName(const std::string& action) {
    if (action == "reboot") return "reboot";
    if (action == "poweroff") return "poweroff";
    return "";
}

std::string formatPowerMenu(const Config& cfg) {
    if (!cfg.allowPowerControl) {
        return codeBlock("Power control is disabled. Set BOT_ALLOW_POWER_CONTROL=1 to enable it.");
    }

    std::ostringstream oss;
    oss << "Power control\n\n";
    oss << "Choose action for this server.\n";
    oss << "A confirmation step will be shown before execution.";
    return codeBlock(oss.str());
}

std::string formatPowerConfirm(const std::string& action, const Config& cfg) {
    if (!cfg.allowPowerControl) {
        return codeBlock("Power control is disabled. Set BOT_ALLOW_POWER_CONTROL=1 to enable it.");
    }

    std::string command = powerActionName(action);
    if (command.empty()) return codeBlock("Unknown power action.");

    std::ostringstream oss;
    oss << "Confirm " << command << "\n\n";
    oss << "This will run: sudo -n systemctl " << command;
    return codeBlock(oss.str());
}

std::string runPowerAction(const std::string& action, const Config& cfg) {
    if (!cfg.allowPowerControl) {
        return codeBlock("Power control is disabled. Set BOT_ALLOW_POWER_CONTROL=1 to enable it.");
    }

    std::string command = powerActionName(action);
    if (command.empty()) return codeBlock("Unknown power action.");

    std::string result = exec(("sudo -n systemctl " + command + " 2>&1 && echo OK || echo FAIL").c_str());
    return codeBlock("Power " + command + "\n" + result);
}

std::vector<std::string> failedBotUnits() {
    std::vector<std::string> failed;
    for (const auto& unit : botUnits()) {
        if (exec(("systemctl is-active " + unit).c_str()) == "failed") {
            failed.push_back(unit);
        }
    }
    return failed;
}

std::string buildAlertText(const Config& cfg) {
    Telemetry t = collectTelemetry(cfg);
    std::vector<std::string> alerts;

    if (t.temp >= cfg.tempWarn) alerts.push_back("TEMP " + std::to_string(static_cast<int>(t.temp)) + "C");
    if (t.load[0] >= cfg.loadWarn) alerts.push_back("LOAD " + std::to_string(t.load[0]).substr(0, 4));
    if (t.ramPercent >= cfg.ramWarn) alerts.push_back("RAM " + std::to_string(static_cast<int>(t.ramPercent)) + "%");

    for (const DiskInfo& disk : collectDisks()) {
        if (disk.usedPercent >= cfg.diskWarn) {
            alerts.push_back("DISK " + disk.mount + " " + std::to_string(static_cast<int>(disk.usedPercent)) + "%");
        }
    }

    std::vector<std::string> failed = failedBotUnits();
    for (const auto& unit : failed) {
        alerts.push_back("SERVICE " + unit + " failed");
    }

    if (alerts.empty()) return "";

    std::ostringstream oss;
    oss << "Alert\n\n";
    for (const auto& alert : alerts) {
        oss << "! " << alert << "\n";
    }
    return codeBlock(oss.str());
}

void sendMessage(const Config& cfg, const std::string& text) {
    callApi("sendMessage", {
        {"chat_id", cfg.chatId},
        {"parse_mode", "MarkdownV2"},
        {"text", text},
        {"reply_markup", mainKeyboardJson(cfg)}
    }, cfg);
}

int sendDashboard(const Config& cfg) {
    ApiResponse response = callApi("sendMessage", {
        {"chat_id", cfg.chatId},
        {"parse_mode", "MarkdownV2"},
        {"text", formatTelemetry(cfg)},
        {"reply_markup", mainKeyboardJson(cfg)}
    }, cfg);

    try {
        json parsed = json::parse(response.body);
        return parsed["result"].value("message_id", 0);
    } catch (...) {
        return 0;
    }
}

void editDashboard(const Config& cfg, int messageId, const std::string& text, const std::string& replyMarkup) {
    if (messageId <= 0) return;
    callApi("editMessageText", {
        {"chat_id", cfg.chatId},
        {"message_id", std::to_string(messageId)},
        {"parse_mode", "MarkdownV2"},
        {"text", text},
        {"reply_markup", replyMarkup}
    }, cfg);
}

std::string reportForCallback(const std::string& data, const Config& cfg) {
    if (data == "services" || data.rfind("services:", 0) == 0) return formatServicesReport();
    if (data == "disk") return formatDiskReport();
    if (data == "net") return formatNetReport();
    if (data == "power") return formatPowerMenu(cfg);
    if (data == "power:ask:reboot") return formatPowerConfirm("reboot", cfg);
    if (data == "power:ask:poweroff") return formatPowerConfirm("poweroff", cfg);
    if (data == "power:confirm:reboot") return runPowerAction("reboot", cfg);
    if (data == "power:confirm:poweroff") return runPowerAction("poweroff", cfg);

    const std::string restartPrefix = "restart:";
    if (data.rfind(restartPrefix, 0) == 0) {
        std::string payload = data.substr(restartPrefix.size());
        std::size_t delimiter = payload.find(':');
        if (delimiter != std::string::npos) {
            payload = payload.substr(delimiter + 1);
        }
        return restartUnit(payload, cfg) + "\n\n" + formatServicesReport();
    }
    return formatTelemetry(cfg);
}

std::string viewForCallback(const std::string& data) {
    if (data == "services" || data.rfind("services:", 0) == 0) return data;
    if (data == "power") return "power";
    if (data == "power:ask:reboot") return "power:ask:reboot";
    if (data == "power:ask:poweroff") return "power:ask:poweroff";
    if (data.rfind("power:confirm:", 0) == 0) return "status";
    if (data.rfind("restart:", 0) == 0) {
        std::string payload = data.substr(8);
        std::size_t delimiter = payload.find(':');
        if (delimiter != std::string::npos) {
            return "services:" + payload.substr(0, delimiter);
        }
        return "services";
    }
    return data;
}

std::string keyboardForTextCommand(const std::string& text, const Config& cfg) {
    if (text == "/services") return keyboardForView("services", cfg);
    if (text == "/power") return keyboardForView("power", cfg);
    return mainKeyboardJson(cfg);
}

std::string handleTextCommand(const std::string& text, const Config& cfg) {
    if (text == "/start" || text == "/status") return formatTelemetry(cfg);
    if (text == "/services") return formatServicesReport();
    if (text == "/disk") return formatDiskReport();
    if (text == "/net") return formatNetReport();
    if (text == "/power") return formatPowerMenu(cfg);

    const std::string restartPrefix = "/restart ";
    if (text.rfind(restartPrefix, 0) == 0) {
        if (!cfg.allowServiceControl) {
            return codeBlock("Service control is disabled. Set BOT_ALLOW_SERVICE_CONTROL=1 to enable it.");
        }

        std::string unit = trim(text.substr(restartPrefix.size()));
        if (unit.find(".service") == std::string::npos) unit += ".service";
        return restartUnit(unit, cfg);
    }

    return codeBlock("Commands: /start /status /services /disk /net /power /restart bot-name.service");
}

int main() {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    Config cfg;
    try {
        cfg = loadConfig();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    callApi("getUpdates", {{"offset", "-1"}}, cfg);
    int dashboardMessageId = sendDashboard(cfg);
    int lastUpdateId = 0;
    auto lastAlertAt = std::chrono::steady_clock::now() - std::chrono::seconds(cfg.alertIntervalSec);
    auto lastRefreshAt = std::chrono::steady_clock::now();

    while (running) {
        ApiResponse updates = getUpdates(lastUpdateId + 1, cfg);
        if (!updates.ok || updates.body.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        try {
            json payload = json::parse(updates.body);
            if (payload.contains("result") && payload["result"].is_array()) {
                for (auto& update : payload["result"]) {
                    lastUpdateId = update.value("update_id", lastUpdateId);

                    if (update.contains("callback_query")) {
                        auto& cb = update["callback_query"];
                        std::string cbId = cb.value("id", "");
                        std::string data = cb.value("data", "");

                        if (!cb.contains("message") || !isAuthorizedChat(cb["message"]["chat"], cfg)) {
                            callApi("answerCallbackQuery", {
                                {"callback_query_id", cbId},
                                {"text", "Not authorized"},
                                {"show_alert", "true"}
                            }, cfg);
                            continue;
                        }

                        int msgId = cb["message"].value("message_id", 0);
                        dashboardMessageId = msgId;
                        callApi("answerCallbackQuery", {{"callback_query_id", cbId}}, cfg);
                        editDashboard(cfg, msgId, reportForCallback(data, cfg), keyboardForView(viewForCallback(data), cfg));
                        continue;
                    }

                    if (update.contains("message")) {
                        auto& msg = update["message"];
                        if (!msg.contains("chat") || !isAuthorizedChat(msg["chat"], cfg)) continue;
                        if (!msg.contains("text")) continue;

                        std::string response = handleTextCommand(msg.value("text", ""), cfg);
                        ApiResponse sent = callApi("sendMessage", {
                            {"chat_id", cfg.chatId},
                            {"parse_mode", "MarkdownV2"},
                            {"text", response},
                            {"reply_markup", keyboardForTextCommand(msg.value("text", ""), cfg)}
                        }, cfg);

                        try {
                            json parsed = json::parse(sent.body);
                            dashboardMessageId = parsed["result"].value("message_id", dashboardMessageId);
                        } catch (...) {}
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Update parse error: " << e.what() << "\n";
        }

        auto now = std::chrono::steady_clock::now();
        if (cfg.autoRefreshSec > 0 &&
            now - lastRefreshAt >= std::chrono::seconds(cfg.autoRefreshSec)) {
            editDashboard(cfg, dashboardMessageId, formatTelemetry(cfg), mainKeyboardJson(cfg));
            lastRefreshAt = now;
        }

        if (cfg.alertIntervalSec > 0 &&
            now - lastAlertAt >= std::chrono::seconds(cfg.alertIntervalSec)) {
            std::string alert = buildAlertText(cfg);
            if (!alert.empty()) {
                sendMessage(cfg, alert);
                lastAlertAt = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    curl_global_cleanup();
    return 0;
}
