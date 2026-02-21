#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <fstream>
#include <array>
#include <cstdlib>
#include <curl/curl.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <nlohmann/json.hpp>



using json = nlohmann::json;


size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string exec(const char* cmd) {
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        result += buffer.data();
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

std::string escape(const std::string& text) {
    std::string out;
    std::string spec = "_*[]()~`>#+-=|{}.!";
    for (char c : text) {
        if (spec.find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

std::string bar(float percent) {
    int filled = static_cast<int>(percent / 10);
    if (filled > 10) filled = 10;
    if (filled < 0) filled = 0;
    std::string b = "[";
    for (int i = 0; i < 10; ++i)
        b += (i < filled) ? "■" : "·";
    b += "]";
    return b;
}


std::string getTelemetry() {
    struct sysinfo mem;
    sysinfo(&mem);

    float ramP = 100.0f * (mem.totalram - mem.freeram) / mem.totalram;
    float swapP = mem.totalswap ? 100.0f * (mem.totalswap - mem.freeswap) / mem.totalswap : 0;

    std::ifstream tFile("/sys/class/thermal/thermal_zone0/temp");
    float temp = 0;
    if (tFile >> temp) temp /= 1000;

    double load[3]; getloadavg(load, 3);

    std::string cpuFreq = exec("vcgencmd measure_clock arm | cut -d= -f2");
    if(cpuFreq.empty()) cpuFreq = "N/A";
    else {
        try {
            long long hz = std::stoll(cpuFreq);
            cpuFreq = std::to_string(hz / 1000000) + " MHz";
        } catch(...) {}
    }

    std::string localIP = exec("hostname -I | awk '{print $1}'");
    if(localIP.empty()) localIP = "N/A";

    std::ostringstream oss;
    oss << "Системный отчёт сервера на базе RPi4\n\n";
    
    oss << std::left << std::setw(12) << "TEMP" << "▸ " << std::fixed << std::setprecision(1) << temp << "°C\n";
    oss << std::left << std::setw(12) << "LOAD" << "▸ " << load[0] << "\n";
    oss << std::left << std::setw(12) << "RAM"  << "▸ " << bar(ramP) << " " << (int)ramP << "%\n";
    oss << std::left << std::setw(12) << "SWAP" << "▸ " << bar(swapP) << " " << (int)swapP << "%\n";
    oss << std::left << std::setw(12) << "CPU"  << "▸ " << cpuFreq << "\n";
    oss << std::left << std::setw(12) << "IP"   << "▸ " << localIP << "\n";
    oss << std::left << std::setw(12) << "UPTIME" 
        << "▸ " << mem.uptime / 3600 << "H "
        << (mem.uptime % 3600) / 60 << "M\n\n";
   
    oss << "STATUS: OPERATIONAL";

    return "```\n" + escape(oss.str()) + "\n```";
}


std::string getSystemdStatus() {
    std::string cmd = "systemctl list-units --type=service --all --no-legend --no-pager";
    std::string raw = exec(cmd.c_str());

    std::ostringstream oss;
    oss << "ОТЧЁТ ПО ВСЕМ СЕРВИСАМ\n";
    oss << "──────────────────────────\n";
    oss << std::left << std::setw(18) << "UNIT" << " STATUS\n";
    oss << "──────────────────────────\n";

    if (raw.empty()) {
        oss << "СЕРВИСЫ НЕ НАЙДЕНЫ\n";
    } else {
        std::istringstream iss(raw);
        std::string line;
        int count = 0;
        while (std::getline(iss, line) && count < 70) {
            std::istringstream ls(line);
            std::string unit, load, active, sub;
            ls >> unit >> load >> active >> sub;
            
            if (unit.empty()) continue;

            
            if (unit.length() > 8 && unit.substr(unit.length() - 8) == ".service")
                unit = unit.substr(0, unit.length() - 8);

            
            if (unit.length() > 17) unit = unit.substr(0, 16) + "~";

            oss << std::left << std::setw(18) << unit 
                << (active == "active" ? "▸ [ OK ]" : "▸ [FAIL]") << "\n";
            count++;
        }
        if (count >= 70) oss << "...и другие\n";
    }

    oss << "──────────────────────────\n";
    return "```\n" + escape(oss.str()) + "\n```";
}


//tg api
void callApi(const std::string& method, const std::string& params, const std::string& token) {
    CURL* curl = curl_easy_init();
    if (!curl) return;

    std::string response_buffer; 
    std::string url = "https://api.telegram.org/bot" + token + "/" + method;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, params.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer); // Критически важно!
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
}


int main() {
    const char* tokenEnv = std::getenv("BOT_TOKEN");
    const char* chatIdEnv = std::getenv("CHAT_ID");

    if (!tokenEnv || !chatIdEnv) {
        std::cerr << "BOT_TOKEN or CHAT_ID not set!" << std::endl;
        return 1;
    }

    std::string TOKEN(tokenEnv);
    std::string CHAT_ID(chatIdEnv);

    std::string keyboard =
        "&reply_markup={\"inline_keyboard\":["
        "[{\"text\":\"⚡ REFRESH\",\"callback_data\":\"ref\"},"
        "{\"text\":\"🛠 SERVICES\",\"callback_data\":\"proc\"}]]}";

    
    callApi("getUpdates", "offset=-1", TOKEN);
    

    callApi("sendMessage", "chat_id=" + CHAT_ID + "&parse_mode=MarkdownV2&text=" + getTelemetry() + keyboard, TOKEN);

    int lastUpdateId = 0;

    while (true) {
        CURL* curl = curl_easy_init();
        std::string response;

        if (curl) {
            std::string url = "https://api.telegram.org/bot" + TOKEN + "/getUpdates?offset=" + 
                              std::to_string(lastUpdateId + 1) + "&timeout=20";
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
        }

        if (response.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        try {
            json j = json::parse(response);
            if (j.contains("result") && j["result"].is_array()) {
                for (auto& update : j["result"]) {
                    lastUpdateId = update["update_id"].get<int>();

                    if (!update.contains("callback_query")) continue;

                    auto& cb = update["callback_query"];
                    std::string data = cb.value("data", "");
                    std::string cbId = cb.value("id", "");
                    
                    if (cb.contains("message")) {
                        int msgId = cb["message"].value("message_id", 0);
                        std::string text = (data == "proc") ? getSystemdStatus() : getTelemetry();

                        callApi("answerCallbackQuery", "callback_query_id=" + cbId, TOKEN);
                        callApi("editMessageText",
                                "chat_id=" + CHAT_ID +
                                "&message_id=" + std::to_string(msgId) +
                                "&parse_mode=MarkdownV2&text=" + text + keyboard,
                                TOKEN);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "JSON Error: " << e.what() << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
} 


//nlohmann/json...парсинг ответов теперь происходит по стандартам, а не через поиск подстрок.
