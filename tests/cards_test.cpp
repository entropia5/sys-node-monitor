#define main monitorBotMain
#include "../SMB.cpp"
#undef main
int main() {
    Config cfg;
    if (metricColor(37.5,75,85) != "#54dfa2" || metricColor(80,75,85) != "#ffb454" || metricColor(90,75,85) != "#ff626b") return 1;
    if (cardEscape("<x>&") != "&lt;x&gt;&amp;") return 2;
    Telemetry t; t.temp=37.5; t.load[0]=0.3;t.load[1]=0.4;t.load[2]=0.5;t.ramPercent=35;t.cpuFreq="1800 MHz";t.localIp="192.168.3.247";t.throttled="0x0";t.uptime=252*3600;
    std::filesystem::create_directories("data/previews");
    auto save=[&](const std::string& name,const std::string& text) {
        auto path=renderReportCard(text,cfg); if(path.empty()) throw std::runtime_error("Render failed");
        std::filesystem::copy_file(path,"data/previews/"+name+".jpg",std::filesystem::copy_options::overwrite_existing);
    };
    save("status-normal",formatTelemetry(t,{},6,0));
    t.temp=80;t.ramPercent=85;t.load[0]=4;
    save("status-warning",formatTelemetry(t,{Severity::WARN,Severity::WARN,Severity::WARN,Severity::WARN,Severity::OK},92,0));
    t.temp=90;t.ramPercent=95;t.load[0]=6;
    save("status-critical",formatTelemetry(t,{Severity::CRIT,Severity::CRIT,Severity::CRIT,Severity::CRIT,Severity::OK},97,0));
    save("disk",formatDiskReport());save("net",formatNetReport());
    std::deque<HealthSnapshot> history{{std::time(nullptr),37.5,0.3,35,6,100},{std::time(nullptr),80,4,85,92,80},{std::time(nullptr),90,6,95,97,40}};
    save("history",formatHistoryReport(history));
    cfg.allowPowerControl=true;save("power",formatPowerMenu(cfg));save("power-confirm",formatPowerConfirm("reboot",cfg));
    std::cout << "Color boundaries, escaping and 8 card previews: OK\n";
}
