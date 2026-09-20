#include "overlay/core/overlay_util.h"
#include "core/settings.h"
#include "core/star_common.h"

#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>

float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
float easeOut(float t) { float f = 1.f - t; return 1.f - f * f * f; }
float easeIn(float t) { return t * t * t; }

void draw_star(ImDrawList* dl, ImVec2 c, float r_out, float r_in, ImU32 col)
{
    const float PI = 3.14159265f;
    ImVec2 pts[10];
    for (int i = 0; i < 10; i++) {
        float r = (i % 2 == 0) ? r_out : r_in;
        float ang = -PI / 2.f + (float)i * PI / 5.f;
        pts[i] = { c.x + cosf(ang) * r, c.y + sinf(ang) * r };
    }
    dl->AddConvexPolyFilled(pts, 10, col);
}

std::string fmt_unlock_time(uint32_t t)
{
    if (!t) return {};
    time_t tt = (time_t)t;
    struct tm lt{};
    if (localtime_s(&lt, &tt) != 0) return {};
    char buf[16];
    strftime(buf, sizeof(buf), "%d.%m %H:%M", &lt);
    return buf;
}

int wrap_two_lines(ImFont* font, float size, const char* text, float max_w,
                   std::string& l1, std::string& l2)
{
    l1.clear(); l2.clear();
    if (!text || !*text || !font || max_w <= 8.f || size <= 0.f) return 0;
    std::vector<std::string> words;
    for (const char* p = text; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != ' ') e++;
        words.emplace_back(p, e);
        p = e;
    }
    if (words.empty()) return 0;
    auto width = [&](const std::string& s) {
        return font->CalcTextSizeA(size, FLT_MAX, 0.f, s.c_str()).x;
    };
    size_t i = 0;
    while (i < words.size()) {
        std::string t = l1.empty() ? words[i] : l1 + " " + words[i];
        if (width(t) <= max_w) { l1 = t; i++; } else break;
    }
    if (l1.empty()) { l1 = words[0]; i = 1; } // single overlong word: clip as before
    if (i >= words.size()) return 1;
    while (i < words.size()) {
        std::string t = l2.empty() ? words[i] : l2 + " " + words[i];
        if (width(t) <= max_w) { l2 = t; i++; } else break;
    }
    if (i < words.size()) {
        while (!l2.empty() && width(l2 + "...") > max_w) {
            size_t sp = l2.find_last_of(' ');
            if (sp == std::string::npos) { l2.clear(); break; }
            l2.resize(sp);
        }
        l2 += "...";
    }
    return 2;
}

ImU32 col(uint8_t r, uint8_t g, uint8_t b, float a) {
    return IM_COL32(r, g, b, (int)(a * 255.f + .5f));
}
ImVec4 v4(uint8_t r, uint8_t g, uint8_t b, float a) {
    return { r / 255.f, g / 255.f, b / 255.f, a };
}

void save_overlay_key(const std::string& key, const std::string& value)
{
    std::string path = Settings::get().settings_dir + "\\overlay.star";
    std::vector<std::string> lines;
    {
        std::ifstream in(utf8_to_wstring(path));
        std::string l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }
    bool found = false;
    for (auto& ln : lines) {
        size_t p = 0;
        while (p < ln.size() && isspace((unsigned char)ln[p])) p++;
        if (ln.compare(p, key.size(), key) == 0) {
            size_t q = p + key.size();
            while (q < ln.size() && isspace((unsigned char)ln[q])) q++;
            if (q < ln.size() && ln[q] == '=') {
                ln = ln.substr(0, p) + key + " = " + value;
                found = true;
            }
        }
    }
    if (!found) lines.push_back(key + " = " + value);
    std::ofstream out(utf8_to_wstring(path), std::ios::trunc);
    for (auto& ln : lines) out << ln << "\n";
}
