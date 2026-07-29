#include "config.h"
#include <fstream>
#include <sstream>
#include <unordered_map>

static void WriteDefaults(const std::string& path, const Settings& s) {
    std::ofstream out(path);
    out << "# Display Translation - PC server config\n";
    out << "# Регион экрана, который транслируется на планшет (в пикселях) - обычно переопределяется автоматически\n";
    out << "region_x=" << s.region_x << "\n";
    out << "region_y=" << s.region_y << "\n";
    out << "region_w=" << s.region_w << "\n";
    out << "region_h=" << s.region_h << "\n";
    out << "# Частота кадров\n";
    out << "fps=" << s.fps << "\n";
    out << "# Битрейт H.264-видео в кбит/с (выше = чётче, но больше трафика и нагрузка на декодер планшета)\n";
    out << "bitrate_kbps=" << s.bitrate_kbps << "\n";
    out << "# TCP порт сервера (должен совпадать с adb reverse и клиентом)\n";
    out << "port=" << s.port << "\n";
    out << "# Индекс монитора для захвата (смотрите список в консоли при запуске; 0 = основной)\n";
    out << "monitor_index=" << s.monitor_index << "\n";
}

Settings Settings::LoadOrCreate(const std::string& path) {
    Settings s;
    std::ifstream in(path);
    if (!in.good()) {
        WriteDefaults(path, s);
        return s;
    }

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        // trim possible \r
        if (!val.empty() && val.back() == '\r') val.pop_back();
        kv[key] = val;
    }

    auto geti = [&](const char* key, int def) {
        auto it = kv.find(key);
        if (it == kv.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    };

    s.region_x = geti("region_x", s.region_x);
    s.region_y = geti("region_y", s.region_y);
    s.region_w = geti("region_w", s.region_w);
    s.region_h = geti("region_h", s.region_h);
    s.fps = geti("fps", s.fps);
    s.jpeg_quality = geti("jpeg_quality", s.jpeg_quality);
    s.bitrate_kbps = geti("bitrate_kbps", s.bitrate_kbps);
    s.port = geti("port", s.port);
    s.monitor_index = geti("monitor_index", s.monitor_index);

    return s;
}
