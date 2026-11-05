#pragma once
// The Win32 names input.cpp keeps on other systems: binds are Windows key codes (scda.toml stores them) and a pad
// is read into XInput's layout. Without a keyboard and mouse the Win32 calls do nothing.
#ifndef _WIN32
#include <strings.h>

#include <chrono>
#include <cstdint>
#include <thread>

using BYTE = uint8_t;
using WORD = uint16_t;
using SHORT = int16_t;
using UINT = uint32_t;
using LONG = int32_t;
using HWND = void*;
using HKL = void*;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
struct RECT {
    LONG left, top, right, bottom;
};
struct POINT {
    LONG x, y;
};

struct XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
enum : WORD {
    XINPUT_GAMEPAD_DPAD_UP = 0x0001,
    XINPUT_GAMEPAD_DPAD_DOWN = 0x0002,
    XINPUT_GAMEPAD_DPAD_LEFT = 0x0004,
    XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008,
    XINPUT_GAMEPAD_START = 0x0010,
    XINPUT_GAMEPAD_BACK = 0x0020,
    XINPUT_GAMEPAD_LEFT_THUMB = 0x0040,
    XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080,
    XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
    XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200,
    XINPUT_GAMEPAD_A = 0x1000,
    XINPUT_GAMEPAD_B = 0x2000,
    XINPUT_GAMEPAD_X = 0x4000,
    XINPUT_GAMEPAD_Y = 0x8000,
};
constexpr BYTE XINPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;

enum : int {
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04, VK_XBUTTON1 = 0x05, VK_XBUTTON2 = 0x06,
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
    VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22,
    VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D,
    VK_NUMPAD0 = 0x60, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7,
    VK_NUMPAD8, VK_NUMPAD9, VK_MULTIPLY, VK_ADD, VK_SEPARATOR, VK_SUBTRACT, VK_DECIMAL, VK_DIVIDE,
    VK_F1 = 0x70, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
    VK_NUMLOCK = 0x90, VK_SCROLL = 0x91, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2,
    VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_BROWSER_BACK = 0xA6, VK_LAUNCH_APP2 = 0xB7,
    VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE,
    VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE,
};
enum : UINT { MAPVK_VK_TO_VSC = 0, MAPVK_VK_TO_CHAR = 2, MAPVK_VSC_TO_VK_EX = 3 };
enum : UINT { WM_SETCURSOR = 0x0020, WM_MOUSEMOVE = 0x0200, HTCLIENT = 1 };
constexpr int WHEEL_DELTA = 120;

inline SHORT GetAsyncKeyState(int) { return 0; }
inline HKL GetKeyboardLayout(uint32_t) { return nullptr; }
inline uint32_t GetWindowThreadProcessId(HWND, void*) { return 0; }
inline UINT MapVirtualKeyExW(UINT, UINT, HKL) { return 0; }
inline LPARAM MAKELPARAM(int lo, int hi) { return LPARAM(uint16_t(lo) | uint32_t(uint16_t(hi)) << 16); }
inline bool PostMessageW(HWND, UINT, WPARAM, LPARAM) { return true; }
inline bool GetCursorPos(POINT*) { return false; }
inline bool ScreenToClient(HWND, POINT*) { return false; }
inline bool GetClientRect(HWND, RECT*) { return false; }
inline int MapWindowPoints(HWND, HWND, POINT*, UINT) { return 0; }
inline bool GetClipCursor(RECT*) { return false; }
inline bool EqualRect(const RECT*, const RECT*) { return false; }
inline bool ClipCursor(const RECT*) { return true; }

inline void Sleep(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
#endif
