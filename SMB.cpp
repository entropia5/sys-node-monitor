#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <deque>
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
#include <fcntl.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

using json = nlohmann::json;

std::atomic<bool> running(true);

int acquireInstanceLock(const std::string& lockPath) {
    int fd = open(lockPath.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct Config {
    std::string token;
    std::string chatId;
    double tempWarn = 75.0;
    double tempCrit = 85.0;
    double loadWarn = 3.5;
    double loadCrit = 5.0;
    double ramWarn = 80.0;
    double ramCrit = 90.0;
    double diskWarn = 90.0;
    double diskCrit = 95.0;
    double hysteresis = 2.0;
    int historySize = 120;
    int pollTimeoutSec = 20;
    int autoRefreshSec = 30;
    int alarmTtlSec = 30;
    bool allowServiceControl = false;
    bool allowPowerControl = false;
    std::string selfUnit;
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

enum class Severity {
    OK = 0,
    WARN = 1,
    CRIT = 2
};

struct TelemetryLevels {
    Severity temp = Severity::OK;
    Severity load = Severity::OK;
    Severity ram = Severity::OK;
    Severity disk = Severity::OK;
    Severity services = Severity::OK;
};

struct HealthSnapshot {
    std::time_t timestamp = 0;
    double temp = 0.0;
    double load = 0.0;
    double ram = 0.0;
    double disk = 0.0;
    int score = 100;
};

struct DashboardData {
    Telemetry telemetry;
    double diskUsage = 0.0;
    int failedServiceCount = 0;
    TelemetryLevels levels;
    std::string text;
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

struct ServiceInfo {
    std::string unit;
    std::string active;
    std::string memory;
};

struct PendingAlarm {
    int messageId = 0;
    std::chrono::steady_clock::time_point deleteAt;
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

std::string shellEscapeSingleQuotes(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

std::string exec(const std::string& cmd, int timeoutSec = 3, int retries = 1) {
    std::string safeCmd = "timeout " + std::to_string(std::max(1, timeoutSec))
        + " sh -c '" + shellEscapeSingleQuotes(cmd) + "'";
    for (int attempt = 0; attempt <= retries; ++attempt) {
        std::array<char, 256> buffer;
        std::string result;
        FILE* pipe = popen(safeCmd.c_str(), "r");
        if (!pipe) return "";
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
        }
        int rc = pclose(pipe);
        std::string trimmed = trim(result);
        if (rc == 0 || !trimmed.empty()) {
            return trimmed;
        }
    }
    return "";
}

std::string exec(const char* cmd) {
    return exec(std::string(cmd), 3, 1);
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
    cfg.tempCrit = envDouble("TEMP_CRIT", cfg.tempCrit);
    cfg.loadWarn = envDouble("LOAD_WARN", cfg.loadWarn);
    cfg.loadCrit = envDouble("LOAD_CRIT", cfg.loadCrit);
    cfg.ramWarn = envDouble("RAM_WARN", cfg.ramWarn);
    cfg.ramCrit = envDouble("RAM_CRIT", cfg.ramCrit);
    cfg.diskWarn = envDouble("DISK_WARN", cfg.diskWarn);
    cfg.diskCrit = envDouble("DISK_CRIT", cfg.diskCrit);
    cfg.hysteresis = envDouble("LEVEL_HYSTERESIS", cfg.hysteresis);
    cfg.historySize = envInt("HISTORY_SIZE", cfg.historySize);
    if (cfg.tempCrit < cfg.tempWarn) cfg.tempCrit = cfg.tempWarn;
    if (cfg.loadCrit < cfg.loadWarn) cfg.loadCrit = cfg.loadWarn;
    if (cfg.ramCrit < cfg.ramWarn) cfg.ramCrit = cfg.ramWarn;
    if (cfg.diskCrit < cfg.diskWarn) cfg.diskCrit = cfg.diskWarn;
    if (cfg.hysteresis < 0.0) cfg.hysteresis = 0.0;
    cfg.historySize = std::max(10, std::min(500, cfg.historySize));
    cfg.pollTimeoutSec = envInt("POLL_TIMEOUT_SEC", cfg.pollTimeoutSec);
    cfg.autoRefreshSec = envInt("AUTO_REFRESH_SEC", cfg.autoRefreshSec);
    cfg.alarmTtlSec = envInt("ALARM_TTL_SEC", cfg.alarmTtlSec);
    cfg.alarmTtlSec = std::max(1, std::min(300, cfg.alarmTtlSec));
    cfg.allowServiceControl = envBool("BOT_ALLOW_SERVICE_CONTROL");
    cfg.allowPowerControl = envBool("BOT_ALLOW_POWER_CONTROL");
    const char* selfUnitEnv = std::getenv("BOT_SELF_UNIT");
    cfg.selfUnit = selfUnitEnv ? trim(selfUnitEnv) : "";
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

ApiResponse getUpdates(int offset, const Config& cfg, int timeoutSec) {
    ApiResponse result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "curl_easy_init failed\n";
        return result;
    }

    timeoutSec = std::max(1, timeoutSec);
    std::string url = "https://api.telegram.org/bot" + cfg.token + "/getUpdates";
    url += "?offset=" + std::to_string(offset);
    url += "&timeout=" + std::to_string(timeoutSec);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec + 10L);

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

ApiResponse getUpdates(int offset, const Config& cfg) {
    return getUpdates(offset, cfg, cfg.pollTimeoutSec);
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
        }),
        json::array({
            {{"text", "HISTORY"}, {"callback_data", "history"}}
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
std::string formatHistoryReport(const std::deque<HealthSnapshot>& history);

constexpr std::size_t SERVICES_PER_PAGE = 5;

Telemetry collectTelemetry() {
    Telemetry t;
    struct sysinfo mem {};
    if (sysinfo(&mem) == 0) {
        const double totalSwap = static_cast<double>(mem.totalswap) * static_cast<double>(mem.mem_unit);
        const double freeSwap = static_cast<double>(mem.freeswap) * static_cast<double>(mem.mem_unit);

        if (totalSwap > 0.0) t.swapPercent = 100.0 * (totalSwap - freeSwap) / totalSwap;
        t.uptime = mem.uptime;
    }

    // Use MemAvailable for realistic Linux memory pressure accounting.
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo) {
        double memTotalKb = 0.0;
        double memAvailableKb = 0.0;
        std::string key;
        double value = 0.0;
        std::string unit;
        while (meminfo >> key >> value >> unit) {
            if (key == "MemTotal:") memTotalKb = value;
            if (key == "MemAvailable:") memAvailableKb = value;
        }
        if (memTotalKb > 0.0) {
            double used = memTotalKb - memAvailableKb;
            t.ramPercent = 100.0 * used / memTotalKb;
            t.ramPercent = std::max(0.0, std::min(100.0, t.ramPercent));
        }
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

double maxDiskUsagePercent() {
    double maxDisk = 0.0;
    for (const DiskInfo& disk : collectDisks()) {
        maxDisk = std::max(maxDisk, disk.usedPercent);
    }
    return maxDisk;
}

Severity classifyWithHysteresis(
    double value,
    double warn,
    double crit,
    Severity previous,
    double hysteresis
) {
    const double h = std::max(0.0, hysteresis);
    if (previous == Severity::CRIT) {
        if (value < crit - h) return (value >= warn ? Severity::WARN : Severity::OK);
        return Severity::CRIT;
    }
    if (previous == Severity::WARN) {
        if (value >= crit) return Severity::CRIT;
        if (value < warn - h) return Severity::OK;
        return Severity::WARN;
    }
    if (value >= crit) return Severity::CRIT;
    if (value >= warn) return Severity::WARN;
    return Severity::OK;
}

std::string severityText(Severity severity) {
    if (severity == Severity::CRIT) return "CRIT";
    if (severity == Severity::WARN) return "WARN";
    return "OK";
}

int healthScore(const TelemetryLevels& levels) {
    int penalty = 0;
    auto addPenalty = [&](Severity s, int warnPenalty, int critPenalty) {
        if (s == Severity::CRIT) penalty += critPenalty;
        else if (s == Severity::WARN) penalty += warnPenalty;
    };
    addPenalty(levels.temp, 10, 22);
    addPenalty(levels.load, 10, 22);
    addPenalty(levels.ram, 14, 28);
    addPenalty(levels.disk, 12, 25);
    addPenalty(levels.services, 20, 35);
    return std::max(0, 100 - penalty);
}

TelemetryLevels evaluateLevels(
    const Telemetry& t,
    const Config& cfg,
    const TelemetryLevels& previous,
    double diskUsage,
    int failedServiceCount
) {
    TelemetryLevels levels;
    levels.temp = classifyWithHysteresis(t.temp, cfg.tempWarn, cfg.tempCrit, previous.temp, cfg.hysteresis);
    levels.load = classifyWithHysteresis(t.load[0], cfg.loadWarn, cfg.loadCrit, previous.load, cfg.hysteresis);
    levels.ram = classifyWithHysteresis(t.ramPercent, cfg.ramWarn, cfg.ramCrit, previous.ram, cfg.hysteresis);
    levels.disk = classifyWithHysteresis(diskUsage, cfg.diskWarn, cfg.diskCrit, previous.disk, cfg.hysteresis);
    levels.services = failedServiceCount > 0 ? Severity::CRIT : Severity::OK;
    return levels;
}

std::string telemetryStatus(const TelemetryLevels& levels, double diskUsage, int failedServiceCount) {
    std::vector<std::string> parts;
    std::ostringstream diskPart;
    diskPart << "Disk " << static_cast<int>(diskUsage) << "% " << severityText(levels.disk);
    parts.push_back("Temp " + severityText(levels.temp));
    parts.push_back("Load " + severityText(levels.load));
    parts.push_back("RAM " + severityText(levels.ram));
    parts.push_back(diskPart.str());
    if (failedServiceCount == 0) {
        parts.push_back("Services OK");
    } else {
        parts.push_back("Services failed: " + std::to_string(failedServiceCount));
    }

    std::ostringstream status;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) status << ", ";
        status << parts[i];
    }
    return status.str();
}

std::string formatTelemetry(
    const Telemetry& t,
    const TelemetryLevels& levels,
    double diskUsage,
    int failedServiceCount
) {
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
    oss << "HEALTH" << " > " << healthScore(levels) << "/100\n";
    oss << "Status: " << telemetryStatus(levels, diskUsage, failedServiceCount);

    return codeBlock(oss.str());
}

DashboardData buildDashboardData(const Config& cfg, const TelemetryLevels& previousLevels) {
    DashboardData data;
    data.telemetry = collectTelemetry();
    data.diskUsage = maxDiskUsagePercent();
    data.failedServiceCount = static_cast<int>(failedBotUnits().size());
    data.levels = evaluateLevels(data.telemetry, cfg, previousLevels, data.diskUsage, data.failedServiceCount);
    data.text = formatTelemetry(data.telemetry, data.levels, data.diskUsage, data.failedServiceCount);
    return data;
}

DiskInfo readDisk(const std::string& mount) {
    DiskInfo info;
    info.mount = mount;

    struct statvfs fs {};
    if (statvfs(mount.c_str(), &fs) != 0 || fs.f_blocks == 0) {
        return info;
    }

    const double blockSize = static_cast<double>(fs.f_frsize);
    const double total = static_cast<double>(fs.f_blocks) * blockSize;
    const double available = static_cast<double>(fs.f_bavail) * blockSize;
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
double parseServiceMemoryMb(const std::string& memoryValue, bool fromPsFallback);

bool isSafeBotUnit(const std::string& unit) {
    if (unit.rfind("bot-", 0) != 0) return false;
    if (unit.size() <= 12 || unit.substr(unit.size() - 8) != ".service") return false;
    return std::all_of(unit.begin(), unit.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '@';
    });
}

std::vector<ServiceInfo> collectServiceInfos(bool includeMemory) {
    std::string raw = exec(
        "systemctl list-units --type=service --all --no-legend --plain 2>/dev/null | awk '$1 ~ /^bot-/ {print $1 \"|\" $3}'",
        2,
        0
    );

    std::vector<ServiceInfo> services;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty()) continue;

        std::size_t delimiter = line.find('|');
        std::string unit = delimiter == std::string::npos ? line : line.substr(0, delimiter);
        std::string active = delimiter == std::string::npos ? "unknown" : line.substr(delimiter + 1);
        unit = trim(unit);
        active = trim(active);
        if (active.empty()) active = "unknown";
        if (isSafeBotUnit(unit)) services.push_back({unit, active, "N/A"});
    }

    std::sort(services.begin(), services.end(), [](const ServiceInfo& a, const ServiceInfo& b) {
        return a.unit < b.unit;
    });

    if (!includeMemory) return services;

    std::ostringstream memCmd;
    bool hasActiveServices = false;
    memCmd << "for u in";
    for (const ServiceInfo& service : services) {
        if (service.active != "active") continue;
        hasActiveServices = true;
        memCmd << " '" << shellEscapeSingleQuotes(service.unit) << "'";
    }
    memCmd << "; do m=$(systemctl show \"$u\" -p MemoryCurrent --value 2>/dev/null); printf '%s|%s\\n' \"$u\" \"$m\"; done";

    if (!hasActiveServices) return services;

    std::string memRaw = exec(memCmd.str(), 2, 0);
    std::istringstream memStream(memRaw);
    std::string memLine;
    while (std::getline(memStream, memLine)) {
        std::size_t delimiter = memLine.find('|');
        if (delimiter == std::string::npos) continue;

        std::string unit = trim(memLine.substr(0, delimiter));
        std::string memStr = trim(memLine.substr(delimiter + 1));
        if (memStr.empty() || memStr == "0" || memStr == "[not set]") continue;

        for (ServiceInfo& service : services) {
            if (service.unit != unit) continue;
            try {
                double mb = parseServiceMemoryMb(memStr, false);
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1) << mb << "M";
                service.memory = oss.str();
            } catch (...) {
                service.memory = "N/A";
            }
            break;
        }
    }

    return services;
}

std::vector<std::string> botUnits() {
    std::vector<std::string> units;
    for (const ServiceInfo& service : collectServiceInfos(false)) {
        units.push_back(service.unit);
    }
    return units;
}

std::vector<std::string> serviceUnitNames(const std::vector<ServiceInfo>& services) {
    std::vector<std::string> units;
    units.reserve(services.size());
    for (const ServiceInfo& service : services) {
        units.push_back(service.unit);
    }
    return units;
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

double parseServiceMemoryMb(const std::string& memoryValue, bool fromPsFallback) {
    if (memoryValue.empty() || memoryValue == "0" || memoryValue == "[not set]") return 0.1;
    const long long value = std::stoll(memoryValue);
    if (value <= 0) return 0.1;
    const double asDouble = static_cast<double>(value);
    double mb = fromPsFallback ? (asDouble / 1024.0) : (asDouble / 1024.0 / 1024.0);
    return std::max(0.1, mb);
}

std::string formatServicesReport(const std::vector<ServiceInfo>& services) {
    std::ostringstream oss;
    oss << "systemd report:\n\n";

    std::size_t unitWidth = 35;
    for (const ServiceInfo& service : services) {
        unitWidth = std::max(unitWidth, service.unit.size() + 2);
    }

    oss << std::left << std::setw(static_cast<int>(unitWidth)) << "UNIT" << std::setw(8) << "STATUS" << "MEM\n\n";

    int activeCount = 0;
    int failedCount = 0;
    for (const ServiceInfo& service : services) {
        oss << std::left << std::setw(static_cast<int>(unitWidth)) << service.unit;

        if (service.active == "active") {
            ++activeCount;
            oss << std::setw(8) << "[OK]" << service.memory << "\n";
        } else if (service.active == "failed") {
            ++failedCount;
            oss << std::setw(8) << "[FAIL]" << "\n";
        } else {
            oss << std::setw(8) << "[OFF]" << "\n";
        }
    }

    oss << "\nUnits: " << services.size() << "  Active: " << activeCount << "  Failed: " << failedCount << "\n";
    return codeBlock(oss.str());
}

std::string formatServicesReport() {
    return formatServicesReport(collectServiceInfos(true));
}

std::string servicesKeyboardJson(std::size_t page, const std::vector<std::string>& units) {
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
    rows.push_back(json::array({
        {{"text", "HISTORY"}, {"callback_data", "history"}}
    }));

    json keyboard = {{"inline_keyboard", rows}};
    return keyboard.dump();
}

std::string servicesKeyboardJson(std::size_t page) {
    return servicesKeyboardJson(page, botUnits());
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
            },
            {
                {{"text", "HISTORY"}, {"callback_data", "history"}}
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
    if (view == "history") return mainKeyboardJson(cfg);
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
    if (!cfg.selfUnit.empty() && unit == cfg.selfUnit) {
        return codeBlock("Refused. Self-restart is blocked for " + unit + ".");
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
    for (const ServiceInfo& service : collectServiceInfos(false)) {
        if (service.active == "failed") {
            failed.push_back(service.unit);
        }
    }
    return failed;
}

int fetchLatestUpdateId(const Config& cfg) {
    ApiResponse response = callApi("getUpdates", {
        {"offset", "-1"},
        {"timeout", "1"},
        {"allowed_updates", "[\"message\",\"callback_query\"]"}
    }, cfg);

    if (!response.ok || response.body.empty()) return 0;

    try {
        json parsed = json::parse(response.body);
        if (!parsed.contains("result") || !parsed["result"].is_array() || parsed["result"].empty()) return 0;
        return parsed["result"].back().value("update_id", 0);
    } catch (...) {
        return 0;
    }
}

int loadMessageIdState(const std::string& path) {
    std::ifstream in(path);
    int messageId = 0;
    if (!(in >> messageId)) return 0;
    return messageId;
}

void saveMessageIdState(const std::string& path, int messageId) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << messageId;
}

int loadIntState(const std::string& path) {
    std::ifstream in(path);
    int value = 0;
    if (!(in >> value)) return 0;
    return value;
}

void saveIntState(const std::string& path, int value) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << value;
}

TelemetryLevels loadLevelsState(const std::string& path) {
    TelemetryLevels levels;
    std::ifstream in(path);
    int t = 0, l = 0, r = 0, d = 0, s = 0;
    if (!(in >> t >> l >> r >> d >> s)) return levels;
    auto toSeverity = [](int v) {
        if (v >= 2) return Severity::CRIT;
        if (v == 1) return Severity::WARN;
        return Severity::OK;
    };
    levels.temp = toSeverity(t);
    levels.load = toSeverity(l);
    levels.ram = toSeverity(r);
    levels.disk = toSeverity(d);
    levels.services = toSeverity(s);
    return levels;
}

void saveLevelsState(const std::string& path, const TelemetryLevels& levels) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << static_cast<int>(levels.temp) << " "
        << static_cast<int>(levels.load) << " "
        << static_cast<int>(levels.ram) << " "
        << static_cast<int>(levels.disk) << " "
        << static_cast<int>(levels.services);
}

std::deque<HealthSnapshot> loadHistoryState(const std::string& path, int maxItems) {
    std::deque<HealthSnapshot> history;
    std::ifstream in(path);
    if (!in) return history;

    HealthSnapshot s;
    while (in >> s.timestamp >> s.temp >> s.load >> s.ram >> s.disk >> s.score) {
        history.push_back(s);
        while (static_cast<int>(history.size()) > maxItems) history.pop_front();
    }
    return history;
}

void saveHistoryState(const std::string& path, const std::deque<HealthSnapshot>& history) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    for (const auto& s : history) {
        out << s.timestamp << " " << s.temp << " " << s.load << " "
            << s.ram << " " << s.disk << " " << s.score << "\n";
    }
}

void appendHistory(std::deque<HealthSnapshot>& history, const HealthSnapshot& snapshot, int maxItems) {
    history.push_back(snapshot);
    while (static_cast<int>(history.size()) > maxItems) history.pop_front();
}

std::string formatHistoryReport(const std::deque<HealthSnapshot>& history) {
    std::ostringstream oss;
    oss << "History (latest samples):\n\n";
    if (history.empty()) {
        oss << "No samples yet.\n";
        return codeBlock(oss.str());
    }

    oss << std::left << std::setw(6) << "TIME"
        << std::setw(7) << "HEALTH"
        << std::setw(7) << "TEMP"
        << std::setw(7) << "LOAD"
        << std::setw(7) << "RAM"
        << "DISK\n\n";

    std::size_t start = history.size() > 15 ? history.size() - 15 : 0;
    for (std::size_t i = start; i < history.size(); ++i) {
        std::tm tmValue {};
        std::time_t ts = history[i].timestamp;
        localtime_r(&ts, &tmValue);
        char timeBuf[16] = {0};
        std::strftime(timeBuf, sizeof(timeBuf), "%H:%M", &tmValue);

        oss << std::left << std::setw(6) << timeBuf
            << std::setw(7) << history[i].score
            << std::setw(7) << static_cast<int>(history[i].temp)
            << std::setw(7) << std::fixed << std::setprecision(1) << history[i].load
            << std::setw(7) << static_cast<int>(history[i].ram)
            << static_cast<int>(history[i].disk) << "\n";
    }
    return codeBlock(oss.str());
}

int sendDashboard(const Config& cfg, const std::string& dashboardText) {
    ApiResponse response = callApi("sendMessage", {
        {"chat_id", cfg.chatId},
        {"parse_mode", "MarkdownV2"},
        {"text", dashboardText},
        {"reply_markup", mainKeyboardJson(cfg)}
    }, cfg);

    try {
        json parsed = json::parse(response.body);
        return parsed["result"].value("message_id", 0);
    } catch (...) {
        return 0;
    }
}

bool editDashboard(const Config& cfg, int messageId, const std::string& text, const std::string& replyMarkup) {
    if (messageId <= 0) return false;
    ApiResponse response = callApi("editMessageText", {
        {"chat_id", cfg.chatId},
        {"message_id", std::to_string(messageId)},
        {"parse_mode", "MarkdownV2"},
        {"text", text},
        {"reply_markup", replyMarkup}
    }, cfg);
    if (response.ok) return true;
    return response.body.find("message is not modified") != std::string::npos;
}

int sendTemporaryAlarmMessage(const Config& cfg, const std::string& text) {
    ApiResponse response = callApi("sendMessage", {
        {"chat_id", cfg.chatId},
        {"parse_mode", "MarkdownV2"},
        {"text", codeBlock(text)}
    }, cfg);

    try {
        json parsed = json::parse(response.body);
        return parsed["result"].value("message_id", 0);
    } catch (...) {
        return 0;
    }
}

bool deleteMessage(const Config& cfg, int messageId) {
    if (messageId <= 0) return false;
    ApiResponse response = callApi("deleteMessage", {
        {"chat_id", cfg.chatId},
        {"message_id", std::to_string(messageId)}
    }, cfg);
    return response.ok;
}

std::string levelTransitionMessage(
    const std::string& name,
    Severity previous,
    Severity current,
    double value
) {
    if (previous == current) return "";
    std::ostringstream oss;
    oss << name << ": " << severityText(previous) << " -> " << severityText(current)
        << " (" << std::fixed << std::setprecision(1) << value << ")";
    return oss.str();
}

int sendLevelAlarm(
    const Config& cfg,
    const TelemetryLevels& previous,
    const TelemetryLevels& current,
    const Telemetry& telemetry,
    double diskUsage,
    int failedServiceCount
) {
    std::vector<std::string> changes;
    auto pushIfChanged = [&](const std::string& line) {
        if (!line.empty()) changes.push_back(line);
    };

    pushIfChanged(levelTransitionMessage("Temp", previous.temp, current.temp, telemetry.temp));
    pushIfChanged(levelTransitionMessage("Load", previous.load, current.load, telemetry.load[0]));
    pushIfChanged(levelTransitionMessage("RAM", previous.ram, current.ram, telemetry.ramPercent));
    pushIfChanged(levelTransitionMessage("Disk", previous.disk, current.disk, diskUsage));
    if (previous.services != current.services) {
        std::ostringstream oss;
        oss << "Services: " << severityText(previous.services) << " -> " << severityText(current.services)
            << " (" << failedServiceCount << " failed)";
        changes.push_back(oss.str());
    }
    if (changes.empty()) return 0;

    std::ostringstream text;
    text << "ALARM\n";
    text << "Auto-delete in " << cfg.alarmTtlSec << " sec\n\n";
    for (const auto& line : changes) text << line << "\n";
    return sendTemporaryAlarmMessage(cfg, text.str());
}

void queueTemporaryAlarm(std::vector<PendingAlarm>& pendingAlarms, int messageId, const Config& cfg) {
    if (messageId <= 0) return;
    pendingAlarms.push_back({
        messageId,
        std::chrono::steady_clock::now() + std::chrono::seconds(cfg.alarmTtlSec)
    });
}

void deleteExpiredTemporaryAlarms(std::vector<PendingAlarm>& pendingAlarms, const Config& cfg) {
    auto now = std::chrono::steady_clock::now();
    auto it = pendingAlarms.begin();
    while (it != pendingAlarms.end()) {
        if (now >= it->deleteAt) {
            deleteMessage(cfg, it->messageId);
            it = pendingAlarms.erase(it);
        } else {
            ++it;
        }
    }
}

int pollTimeoutForTemporaryAlarms(const std::vector<PendingAlarm>& pendingAlarms, const Config& cfg) {
    int timeoutSec = std::max(1, cfg.pollTimeoutSec);
    if (pendingAlarms.empty()) return timeoutSec;

    auto now = std::chrono::steady_clock::now();
    auto nextDelete = pendingAlarms.front().deleteAt;
    for (const PendingAlarm& alarm : pendingAlarms) {
        nextDelete = std::min(nextDelete, alarm.deleteAt);
    }

    if (nextDelete <= now) return 1;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(nextDelete - now).count();
    int secondsUntilDelete = static_cast<int>((ms + 999) / 1000);
    return std::max(1, std::min(timeoutSec, secondsUntilDelete));
}

std::string reportForCallback(
    const std::string& data,
    const Config& cfg,
    const std::deque<HealthSnapshot>& history,
    const TelemetryLevels& levels
) {
    if (data == "services" || data.rfind("services:", 0) == 0) return formatServicesReport();
    if (data == "disk") return formatDiskReport();
    if (data == "net") return formatNetReport();
    if (data == "history") return formatHistoryReport(history);
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
    DashboardData dataSnapshot = buildDashboardData(cfg, levels);
    return dataSnapshot.text;
}

std::string viewForCallback(const std::string& data) {
    if (data == "services" || data.rfind("services:", 0) == 0) return data;
    if (data == "power") return "power";
    if (data == "history") return "history";
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
    if (text == "/history") return keyboardForView("history", cfg);
    return mainKeyboardJson(cfg);
}

std::string handleTextCommand(
    const std::string& text,
    const Config& cfg,
    const std::deque<HealthSnapshot>& history,
    const TelemetryLevels& levels
) {
    if (text == "/start" || text == "/status") {
        DashboardData dataSnapshot = buildDashboardData(cfg, levels);
        return dataSnapshot.text;
    }
    if (text == "/services") return formatServicesReport();
    if (text == "/disk") return formatDiskReport();
    if (text == "/net") return formatNetReport();
    if (text == "/history") return formatHistoryReport(history);
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

    return codeBlock("Commands: /start /status /services /disk /net /history /power /restart bot-name.service");
}

bool nearlyEqual(double a, double b, double eps = 0.01) {
    return std::fabs(a - b) <= eps;
}

bool runSelfTests() {
    bool ok = true;
    auto expect = [&](bool condition, const std::string& name) {
        if (!condition) {
            ok = false;
            std::cerr << "Self-test failed: " << name << "\n";
        }
    };

    expect(classifyWithHysteresis(70.0, 80.0, 90.0, Severity::OK, 2.0) == Severity::OK, "level ok");
    expect(classifyWithHysteresis(82.0, 80.0, 90.0, Severity::OK, 2.0) == Severity::WARN, "level warn enter");
    expect(classifyWithHysteresis(79.0, 80.0, 90.0, Severity::WARN, 2.0) == Severity::WARN, "level warn hold");
    expect(classifyWithHysteresis(77.0, 80.0, 90.0, Severity::WARN, 2.0) == Severity::OK, "level warn exit");
    expect(classifyWithHysteresis(91.0, 80.0, 90.0, Severity::WARN, 2.0) == Severity::CRIT, "level crit enter");
    expect(classifyWithHysteresis(89.0, 80.0, 90.0, Severity::CRIT, 2.0) == Severity::CRIT, "level crit hold");
    expect(classifyWithHysteresis(87.0, 80.0, 90.0, Severity::CRIT, 2.0) == Severity::WARN, "level crit exit");

    expect(nearlyEqual(parseServiceMemoryMb("104857600", false), 100.0), "memory bytes to mb");
    expect(nearlyEqual(parseServiceMemoryMb("102400", true), 100.0), "memory kb to mb");
    expect(nearlyEqual(parseServiceMemoryMb("0", false), 0.1), "memory minimum floor");
    return ok;
}

int main() {
    int instanceLockFd = acquireInstanceLock("/tmp/systemmonitorbot.lock");
    if (instanceLockFd < 0) {
        std::cerr << "Another SystemMonitorBot instance is already running\n";
        return 1;
    }

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
    if (!runSelfTests()) {
        std::cerr << "Self-tests failed, refusing to start\n";
        curl_global_cleanup();
        return 1;
    }

    const std::string dashboardStatePath = "/tmp/systemmonitorbot-dashboard-message-id.state";
    const std::string updatesStatePath = "/tmp/systemmonitorbot-last-update-id.state";
    const std::string levelsStatePath = "/tmp/systemmonitorbot-levels.state";
    const std::string historyStatePath = "/tmp/systemmonitorbot-history.state";

    TelemetryLevels levels = loadLevelsState(levelsStatePath);
    std::deque<HealthSnapshot> history = loadHistoryState(historyStatePath, cfg.historySize);

    DashboardData bootData = buildDashboardData(cfg, levels);
    appendHistory(history, {std::time(nullptr), bootData.telemetry.temp, bootData.telemetry.load[0], bootData.telemetry.ramPercent, bootData.diskUsage, healthScore(bootData.levels)}, cfg.historySize);
    levels = bootData.levels;
    saveLevelsState(levelsStatePath, levels);
    saveHistoryState(historyStatePath, history);

    int lastUpdateId = std::max(fetchLatestUpdateId(cfg), loadIntState(updatesStatePath));
    int dashboardMessageId = loadMessageIdState(dashboardStatePath);
    if (dashboardMessageId > 0) {
        if (!editDashboard(cfg, dashboardMessageId, bootData.text, mainKeyboardJson(cfg))) {
            dashboardMessageId = 0;
        }
    }
    if (dashboardMessageId <= 0) {
        dashboardMessageId = sendDashboard(cfg, bootData.text);
        if (dashboardMessageId > 0) saveMessageIdState(dashboardStatePath, dashboardMessageId);
    }
    auto lastRefreshAt = std::chrono::steady_clock::now();
    std::string currentView = "status";
    std::vector<PendingAlarm> pendingAlarms;

    while (running) {
        deleteExpiredTemporaryAlarms(pendingAlarms, cfg);
        ApiResponse updates = getUpdates(
            lastUpdateId + 1,
            cfg,
            pollTimeoutForTemporaryAlarms(pendingAlarms, cfg)
        );
        deleteExpiredTemporaryAlarms(pendingAlarms, cfg);
        if (!updates.ok || updates.body.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        try {
            json payload = json::parse(updates.body);
            if (payload.contains("result") && payload["result"].is_array()) {
                for (auto& update : payload["result"]) {
                    lastUpdateId = update.value("update_id", lastUpdateId);
                    saveIntState(updatesStatePath, lastUpdateId);

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
                        std::string nextView = viewForCallback(data);
                        callApi("answerCallbackQuery", {{"callback_query_id", cbId}}, cfg);

                        std::string response;
                        std::string replyMarkup;
                        if (data == "services" || data.rfind("services:", 0) == 0) {
                            std::vector<ServiceInfo> services = collectServiceInfos(true);
                            response = formatServicesReport(services);
                            replyMarkup = cfg.allowServiceControl
                                ? servicesKeyboardJson(servicesPageFromView(nextView), serviceUnitNames(services))
                                : mainKeyboardJson(cfg);
                        } else {
                            response = reportForCallback(data, cfg, history, levels);
                            replyMarkup = keyboardForView(nextView, cfg);
                        }

                        if (editDashboard(cfg, msgId, response, replyMarkup)) {
                            dashboardMessageId = msgId;
                            currentView = nextView;
                            saveMessageIdState(dashboardStatePath, dashboardMessageId);
                        }
                        continue;
                    }

                    if (update.contains("message")) {
                        auto& msg = update["message"];
                        if (!msg.contains("chat") || !isAuthorizedChat(msg["chat"], cfg)) continue;
                        if (!msg.contains("text")) continue;

                        std::string response = handleTextCommand(msg.value("text", ""), cfg, history, levels);
                        ApiResponse sent = callApi("sendMessage", {
                            {"chat_id", cfg.chatId},
                            {"parse_mode", "MarkdownV2"},
                            {"text", response},
                            {"reply_markup", keyboardForTextCommand(msg.value("text", ""), cfg)}
                        }, cfg);
                        if (!sent.ok) {
                            std::cerr << "sendMessage failed HTTP " << sent.httpCode << ": " << sent.body << "\n";
                        }

                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Update parse error: " << e.what() << "\n";
        }

        auto now = std::chrono::steady_clock::now();
        if (cfg.autoRefreshSec > 0 &&
            now - lastRefreshAt >= std::chrono::seconds(cfg.autoRefreshSec)) {
            TelemetryLevels previousLevels = levels;
            DashboardData data = buildDashboardData(cfg, previousLevels);
            queueTemporaryAlarm(
                pendingAlarms,
                sendLevelAlarm(cfg, previousLevels, data.levels, data.telemetry, data.diskUsage, data.failedServiceCount),
                cfg
            );
            levels = data.levels;
            saveLevelsState(levelsStatePath, levels);

            appendHistory(history, {std::time(nullptr), data.telemetry.temp, data.telemetry.load[0], data.telemetry.ramPercent, data.diskUsage, healthScore(data.levels)}, cfg.historySize);
            saveHistoryState(historyStatePath, history);

            std::string refreshText = data.text;
            std::string refreshKeyboard = mainKeyboardJson(cfg);
            if (currentView != "status") {
                if (currentView == "services" || currentView.rfind("services:", 0) == 0) {
                    std::vector<ServiceInfo> services = collectServiceInfos(true);
                    refreshText = formatServicesReport(services);
                    refreshKeyboard = cfg.allowServiceControl
                        ? servicesKeyboardJson(servicesPageFromView(currentView), serviceUnitNames(services))
                        : mainKeyboardJson(cfg);
                } else {
                    refreshText = reportForCallback(currentView, cfg, history, levels);
                    refreshKeyboard = keyboardForView(currentView, cfg);
                }
            }

            if (!editDashboard(cfg, dashboardMessageId, refreshText, refreshKeyboard)) {
                dashboardMessageId = sendDashboard(cfg, data.text);
                if (dashboardMessageId > 0) saveMessageIdState(dashboardStatePath, dashboardMessageId);
                currentView = "status";
            }
            lastRefreshAt = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (const PendingAlarm& alarm : pendingAlarms) {
        deleteMessage(cfg, alarm.messageId);
    }

    curl_global_cleanup();
    close(instanceLockFd);
    return 0;
}
