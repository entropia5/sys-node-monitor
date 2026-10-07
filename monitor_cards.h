#pragma once
#include <filesystem>
#include <regex>

inline std::string cardEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}
inline std::string plainReport(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 7, "```cpp\n") == 0) { i += 6; continue; }
        if (text.compare(i, 3, "```") == 0) { i += 2; continue; }
        if (text[i] == '\\' && i + 1 < text.size()) ++i;
        out += text[i];
    }
    return trim(out);
}
inline std::string metricColor(double value, double warn, double critical) {
    return value >= critical ? "#ff626b" : value >= warn ? "#ffb454" : "#54dfa2";
}
inline std::string coloredNumbers(const std::string& value, const std::string& color, double warn = -1, double critical = -1) {
    static const std::regex number("[0-9]+(?:\\.[0-9]+)?");
    std::string out; std::size_t pos = 0;
    for (std::sregex_iterator it(value.begin(), value.end(), number), end; it != end; ++it) {
        out += cardEscape(value.substr(pos, it->position() - pos));
        const std::string valueColor = warn >= 0 ? metricColor(std::stod(it->str()),warn,critical) : color;
        out += "<span style='color:" + valueColor + "'>" + it->str() + "</span>";
        pos = it->position() + it->length();
    }
    return out + cardEscape(value.substr(pos));
}
inline std::string reportCardHtml(const std::string& report, const Config& cfg) {
    std::istringstream input(plainReport(report)); std::string line;
    std::string html = "<!doctype html><html><head><meta charset='utf-8'><style>"
        "*{box-sizing:border-box}body{margin:0;padding:38px;background:#101215;color:#e8ecf1;"
        "font-family:'DejaVu Sans',sans-serif;width:1100px}h1{font-size:38px;margin:8px 0 12px}"
        ".tag{color:#929aa7;font-size:17px;letter-spacing:3px}.legend{color:#939ba8;font-size:19px;margin:0 0 28px}"
        ".row{background:#1c2026;border:1px solid #303640;border-radius:15px;padding:17px 23px;margin:10px 0;"
        "font-family:'DejaVu Sans Mono',monospace;font-size:25px;white-space:pre-wrap;word-wrap:break-word}"
        ".label{color:#a6afbd;font-size:20px;display:block;margin-bottom:8px}"
        "table{width:100%;border-collapse:collapse;background:#1c2026;font-family:'DejaVu Sans Mono',monospace;font-size:22px}"
        "td,th{padding:14px 10px;border-bottom:1px solid #303640;text-align:left}th{color:#929aa7;font-size:18px}"
        "</style></head><body><div class='tag'>RASPBERRY PI · SERVER MONITOR</div>";
    bool history = false, table = false; bool first = true;
    while (std::getline(input, line)) {
        line = trim(line); if (line.empty()) continue;
        if (first) {
            first = false; history = line.find("History") == 0;
            html += "<h1>" + cardEscape(line) + "</h1><div class='legend'>"
                "<span style='color:#54dfa2'>● Норма</span> &nbsp; "
                "<span style='color:#ffb454'>● Повышено</span> &nbsp; "
                "<span style='color:#ff626b'>● Критично</span></div>";
            continue;
        }
        if (history && line.find("TIME") == 0) {
            table = true; html += "<table><tr><th>Время</th><th>Health</th><th>°C</th><th>Load</th><th>RAM %</th><th>Disk %</th></tr>"; continue;
        }
        if (table) {
            std::istringstream row(line); std::string time; double health,temp,load,ram,disk;
            if (row >> time >> health >> temp >> load >> ram >> disk) {
                html += "<tr><td>" + cardEscape(time) + "</td>";
                const double values[] = {health,temp,load,ram,disk};
                const std::string colors[] = {health < 50 ? "#ff626b" : health < 90 ? "#ffb454" : "#54dfa2",
                    metricColor(temp,cfg.tempWarn,cfg.tempCrit),metricColor(load,cfg.loadWarn,cfg.loadCrit),
                    metricColor(ram,cfg.ramWarn,cfg.ramCrit),metricColor(disk,cfg.diskWarn,cfg.diskCrit)};
                for (int i=0;i<5;++i) { std::ostringstream v; v << values[i]; html += "<td style='color:"+colors[i]+"'>"+v.str()+"</td>"; }
                html += "</tr>"; continue;
            }
        }
        const auto split = line.find('>');
        std::string label = split != std::string::npos ? trim(line.substr(0,split)) : "";
        std::string value = split != std::string::npos ? trim(line.substr(split+1)) : line;
        std::string color = "#d7dce5";
        std::smatch match; static const std::regex numeric("[0-9]+(?:\\.[0-9]+)?");
        double number = std::regex_search(value, match, numeric) ? std::stod(match.str()) : 0;
        if (label == "TEMP") color = metricColor(number,cfg.tempWarn,cfg.tempCrit);
        else if (label == "LOAD") color = metricColor(number,cfg.loadWarn,cfg.loadCrit);
        else if (label == "RAM" || label == "SWAP") color = metricColor(number,cfg.ramWarn,cfg.ramCrit);
        else if (value.find("% used") != std::string::npos) color = metricColor(number,cfg.diskWarn,cfg.diskCrit);
        else if (label == "HEALTH") color = number < 50 ? "#ff626b" : number < 90 ? "#ffb454" : "#54dfa2";
        else if (label == "THROTTLED") color = value == "0x0" ? "#54dfa2" : "#ff626b";
        else if (label.find("PING") == 0) color = value == "OK" ? "#54dfa2" : "#ff626b";
        html += "<div class='row'>";
        if (!label.empty()) html += "<span class='label'>" + cardEscape(label) + "</span>";
        if (label.find("PING") == 0) html += "<span style='color:"+color+"'>"+cardEscape(value)+"</span>";
        else if (label == "LOAD") html += coloredNumbers(value,color,cfg.loadWarn,cfg.loadCrit);
        else html += coloredNumbers(value,color);
        html += "</div>";
    }
    if (table) html += "</table>";
    return html + "</body></html>";
}
inline std::string renderReportCard(const std::string& text, const Config& cfg) {
    std::filesystem::create_directories("data/rendered");
    const std::string html = reportCardHtml(text,cfg);
    const std::string input = "data/rendered/panel.html", output = "data/rendered/panel.jpg";
    std::ifstream existing(input); std::string old((std::istreambuf_iterator<char>(existing)),{});
    if (old == html && std::filesystem::exists(output)) return output;
    { std::ofstream file(input); file << html; if (!file.good()) return ""; }
    const auto staged = "data/rendered/panel.staged.jpg";
    const int rc = std::system("timeout 12s wkhtmltoimage --quiet --enable-local-file-access --format jpg --width 1100 --quality 90 data/rendered/panel.html data/rendered/panel.staged.jpg 2>data/rendered/render-error.log");
    if (rc != 0 || !std::filesystem::exists(staged)) { std::filesystem::remove(input); return ""; }
    std::filesystem::rename(staged,output);
    return output;
}
