// Hotkey text parsing ("CTRL+F9", "alt+shift+x", "NONE"). Pure logic, no Windows dependency.
#pragma once
#include <cstdint>
#include <string>

namespace cbf {

struct Hotkey {
    uint8_t vk = 0;  // Windows virtual-key code, 0 = unbound
    bool ctrl = false, alt = false, shift = false;
    bool bound() const { return vk != 0; }
};

// Accepts: F1..F24, A-Z, 0-9, NUM0..NUM9, named keys (ESC, TAB, SPACE, ENTER, INSERT, DELETE, HOME, END,
// PAGEUP, PAGEDOWN, PAUSE, SCROLLLOCK, BACKSPACE, UP, DOWN, LEFT, RIGHT), hex "0x77",
// optionally prefixed with CTRL+, ALT+, SHIFT+ in any order. "", "NONE", "OFF", "DISABLED" mean unbound.
// Returns false (and leaves `out` unbound) when the text is not understood.
bool ParseHotkey(const std::string& text, Hotkey& out);

}  // namespace cbf
