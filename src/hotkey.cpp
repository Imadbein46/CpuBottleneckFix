#include "hotkey.h"

#include <cctype>
#include <cstdlib>

namespace cbf {
namespace {

std::string Normalize(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == ';') break;  // inline comment
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool KeyFromName(const std::string& n, uint8_t& vk) {
    if (n.empty()) return false;

    // F1..F24
    if (n.size() >= 2 && n[0] == 'F' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        char* end = nullptr;
        long v = std::strtol(n.c_str() + 1, &end, 10);
        if (end && *end == '\0' && v >= 1 && v <= 24) {
            vk = static_cast<uint8_t>(0x70 + v - 1);
            return true;
        }
        return false;
    }
    // single letter / digit: VK code equals the ASCII code
    if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) {
        vk = static_cast<uint8_t>(n[0]);
        return true;
    }
    // NUM0..NUM9
    if (n.size() == 4 && n.compare(0, 3, "NUM") == 0 && n[3] >= '0' && n[3] <= '9') {
        vk = static_cast<uint8_t>(0x60 + (n[3] - '0'));
        return true;
    }
    // hex: 0x01..0xFE
    if (n.size() > 2 && n[0] == '0' && n[1] == 'X') {
        char* end = nullptr;
        long v = std::strtol(n.c_str() + 2, &end, 16);
        if (end && *end == '\0' && v >= 1 && v <= 0xFE) {
            vk = static_cast<uint8_t>(v);
            return true;
        }
        return false;
    }
    static const struct {
        const char* name;
        uint8_t vk;
    } table[] = {
        {"BACKSPACE", 0x08}, {"TAB", 0x09},    {"ENTER", 0x0D},    {"PAUSE", 0x13},  {"ESC", 0x1B},      {"ESCAPE", 0x1B},
        {"SPACE", 0x20},     {"PAGEUP", 0x21}, {"PAGEDOWN", 0x22}, {"END", 0x23},    {"HOME", 0x24},     {"LEFT", 0x25},
        {"UP", 0x26},        {"RIGHT", 0x27},  {"DOWN", 0x28},     {"INSERT", 0x2D}, {"DELETE", 0x2E},   {"SCROLLLOCK", 0x91},
    };
    for (const auto& e : table) {
        if (n == e.name) {
            vk = e.vk;
            return true;
        }
    }
    return false;
}

}  // namespace

bool ParseHotkey(const std::string& text, Hotkey& out) {
    out = Hotkey{};
    const std::string s = Normalize(text);
    if (s.empty() || s == "NONE" || s == "OFF" || s == "DISABLED") return true;

    Hotkey hk;
    bool haveKey = false;
    size_t pos = 0;
    for (;;) {
        size_t next = s.find('+', pos);
        if (next == std::string::npos) next = s.size();
        const std::string tok = s.substr(pos, next - pos);
        if (tok.empty()) return false;  // "CTRL+", "++", "+F9"
        if (tok == "CTRL" || tok == "CONTROL") {
            hk.ctrl = true;
        } else if (tok == "ALT") {
            hk.alt = true;
        } else if (tok == "SHIFT") {
            hk.shift = true;
        } else {
            uint8_t vk = 0;
            if (haveKey || !KeyFromName(tok, vk)) return false;  // two keys, or unknown key
            hk.vk = vk;
            haveKey = true;
        }
        if (next >= s.size()) break;
        pos = next + 1;
    }
    if (!haveKey) return false;  // modifiers only
    out = hk;
    return true;
}

}  // namespace cbf
