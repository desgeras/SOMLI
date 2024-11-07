#include "pch.h"
#include <fstream>
#include <string>
#include <cctype>
#include <vector>
#include <map>
#include <sstream>
#include <set>
#include <algorithm>
#include <chrono>
#include "detours.h"

#pragma comment(lib, "detours.lib")
#pragma comment(lib, "winmm.lib")

const std::set<std::wstring> EXCLUDED_PROCESSES = {
    L"SOM_EDIT.exe",
    L"SOM_MAIN.exe",
    L"SOM_Map.exe",
    L"SOM_PRM.exe",
    L"SOM_RUN.exe",
    L"SOM_SYS.exe",
    L"MapComp.exe",
    L"dgVoodooCpl.exe"
};

typedef ULONGLONG(WINAPI* GetTickCount64_t)(void);
typedef BOOL(WINAPI* QueryPerformanceCounter_t)(LARGE_INTEGER*);
typedef DWORD(WINAPI* TimeGetTime_t)(void);

static GetTickCount64_t TrueGetTickCount64 = GetTickCount64;
static QueryPerformanceCounter_t TrueQueryPerformanceCounter = QueryPerformanceCounter;
static TimeGetTime_t TrueTimeGetTime = timeGetTime;

LARGE_INTEGER g_startCounter;
LARGE_INTEGER g_qpcFrequency;
double g_counterToSecondScale;

bool g_trainerActive = false;
float g_sensitivity = 0.001f;
float g_fov = 0.8726646304f;
bool g_sprinting = true;
bool g_equipmentWeight = true;
bool g_saveAnywhere = true;
bool g_pauseWhenInactive = true;
bool g_modernControls = true;
float g_speedMultiplier = 1.0f;
float g_currentSpeed = 1.0f;
float g_walkSpeed = 1.0f;
float g_sprintSpeed = 3.0f;
WORD g_turnRate = 50;
HWND g_gameWindow = NULL;
bool g_keyStates[256] = { false };
bool g_wasBusy = false;
bool g_wasActive = true;
float g_lastCameraX = 0.0f;
float g_lastCameraY = 0.0f;

bool g_showCompass = true;
bool g_showMeleeStamina = true;
bool g_showMagicStamina = true;
bool g_showStatusEffects = true;
bool g_showHUDDecorations = true;
bool g_showHPMP = true;
bool g_damageFlashing = true;
bool g_poisonFlashing = true;
bool g_darkFlashing = true;

const DWORD CAMERA_X_ADDRESS = 0x019C0BB8;
const DWORD CAMERA_Y_ADDRESS = 0x019C0BB4;
const DWORD FOV_ADDRESS = 0x004C11FC;
const DWORD PAUSE_INACTIVE_ADDRESS = 0x004C1163;
const DWORD SPRINT_ADDRESS = 0x01D1199E;
const DWORD BUSY_CHECK_ADDRESS = 0x01D44448;
const DWORD MAIN_MENU_ADDRESS = 0x004C16CF;
const DWORD EQUIPMENT_WEIGHT_ADDRESS = 0x01D11B73;
const DWORD SAVE_ANYWHERE_ADDRESS = 0x01D11B72;
const DWORD WALK_SPEED_ADDRESS = 0x01D119A0;
const DWORD SPRINT_SPEED_ADDRESS = 0x01D119A4;
const DWORD TURN_RATE_ADDRESS = 0x01D119A8;

const DWORD COMPASS_ADDRESS = 0x019C07E0;
const DWORD COMPASS_BG_ADDRESS = 0x019C07E4;
const DWORD MELEE_STAMINA_ADDRESS = 0x019C07E8;
const DWORD MAGIC_STAMINA_ADDRESS = 0x019C07EC;
const DWORD HUD_DECORATION_ADDRESS = 0x019C07F0;
const DWORD STATUS_EFFECT_ADDRESS = 0x019C07F4;
const DWORD HEALTH_MANA_ADDRESS = 0x019C07F8;

const DWORD RED_FLASH_INSTRUCTION = 0x424D97;
const DWORD POISON_FLASH_INSTRUCTION = 0x424DEA;
const DWORD BLACK_FLASH_INSTRUCTION = 0x4251BA;

std::map<std::string, int> g_actionToVirtualKey = {
    {"MOVE FORWARD", VK_NUMPAD5},
    {"MOVE BACKWARDS", VK_NUMPAD2},
    {"MOVE LEFT", VK_NUMPAD4},
    {"MOVE RIGHT", VK_NUMPAD6},
    {"USE/ACTIVATE", VK_RETURN},
    {"SPRINT", VK_SPACE},
    {"MELEE ATTACK", VK_RSHIFT},
    {"MAGIC ATTACK", VK_RCONTROL},
    {"INVENTORY", VK_TAB},
    {"EXIT", VK_ESCAPE},
    {"CENTER VIEW", VK_NUMPAD8},
    {"LOOK UP", VK_NUMPAD9},
    {"LOOK DOWN", VK_NUMPAD7},
    {"LOOK LEFT", VK_NUMPAD1},
    {"LOOK RIGHT", VK_NUMPAD3}
};

struct KeyMap {
    int virtualKey;
    bool lastState;
};

std::map<int, KeyMap> g_keyMaps;

std::map<std::string, int> g_nameToVK = {
    {"LEFT MOUSE", VK_LBUTTON},
    {"RIGHT MOUSE", VK_RBUTTON},
    {"MIDDLE MOUSE", VK_MBUTTON},
    {"TAB", VK_TAB},
    {"ENTER", VK_RETURN},
    {"SPACE", VK_SPACE},
    {"LEFT SHIFT", VK_LSHIFT},
    {"RIGHT SHIFT", VK_RSHIFT},
    {"LEFT CONTROL", VK_LCONTROL},
    {"RIGHT CONTROL", VK_RCONTROL},
    {"ESCAPE", VK_ESCAPE},
    {"UP ARROW", VK_UP},
    {"DOWN ARROW", VK_DOWN},
    {"LEFT ARROW", VK_LEFT},
    {"RIGHT ARROW", VK_RIGHT}
};

bool ShouldHookProcess() {
    wchar_t processName[MAX_PATH];
    GetModuleFileNameW(NULL, processName, MAX_PATH);

    wchar_t* fileName = wcsrchr(processName, L'\\');
    fileName = fileName ? fileName + 1 : processName;

    std::wstring upperFileName = fileName;
    std::transform(upperFileName.begin(), upperFileName.end(), upperFileName.begin(), ::towupper);

    for (const auto& excluded : EXCLUDED_PROCESSES) {
        std::wstring upperExcluded = excluded;
        std::transform(upperExcluded.begin(), upperExcluded.end(), upperExcluded.begin(), ::towupper);
        if (upperFileName == upperExcluded) {
            return false;
        }
    }

    return true;
}

ULONGLONG WINAPI HookedGetTickCount64() {
    return static_cast<ULONGLONG>(TrueGetTickCount64() * g_currentSpeed);
}

BOOL WINAPI HookedQueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount) {
    BOOL result = TrueQueryPerformanceCounter(lpPerformanceCount);
    if (result && g_currentSpeed != 1.0f) {
        LARGE_INTEGER current = *lpPerformanceCount;
        double elapsedSeconds = static_cast<double>(current.QuadPart - g_startCounter.QuadPart) / static_cast<double>(g_qpcFrequency.QuadPart);
        double adjustedSeconds = elapsedSeconds * g_currentSpeed;
        lpPerformanceCount->QuadPart = g_startCounter.QuadPart + static_cast<LONGLONG>(adjustedSeconds * g_qpcFrequency.QuadPart);
    }
    return result;
}

DWORD WINAPI HookedTimeGetTime() {
    return static_cast<DWORD>(TrueTimeGetTime() * g_currentSpeed);
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t");
    return str.substr(first, (last - first + 1));
}

std::string toUpper(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
    return str;
}

void InitializeKeyNameMap() {
    for (char c = 'A'; c <= 'Z'; c++) {
        std::string keyName(1, c);
        g_nameToVK[keyName] = c;
    }
    for (int i = 0; i <= 9; i++) {
        g_nameToVK[std::to_string(i)] = '0' + i;
    }
}

int GetVirtualKeyFromName(const std::string& keyName) {
    auto it = g_nameToVK.find(toUpper(keyName));
    if (it != g_nameToVK.end()) {
        return it->second;
    }
    if (keyName.length() == 1) {
        return toupper(keyName[0]);
    }
    return 0;
}

void SimulateKeyPress(int key, bool down) {
    keybd_event(key, MapVirtualKey(key, 0), down ? 0 : KEYEVENTF_KEYUP, 0);
}

bool IsGameWindowActive() {
    return (GetForegroundWindow() == g_gameWindow);
}

bool IsInMainMenu() {
    BYTE menuValue = 0;
    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)MAIN_MENU_ADDRESS, &menuValue, sizeof(BYTE), nullptr);
    return menuValue == 0;
}

bool IsInGameMenu() {
    BYTE busyValue = 0;
    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)BUSY_CHECK_ADDRESS, &busyValue, sizeof(BYTE), nullptr);
    return busyValue != 0;
}

float GetCurrentSpeedMultiplier() {
    return g_speedMultiplier;
}

void UpdateCurrentSpeed() {
    g_currentSpeed = g_speedMultiplier;
}

void HandleKeybinds() {
    if (!g_modernControls || !IsGameWindowActive()) {
        for (auto& keyPair : g_keyMaps) {
            if (keyPair.second.lastState) {
                SimulateKeyPress(keyPair.second.virtualKey, false);
                keyPair.second.lastState = false;
            }
        }
        return;
    }

    for (auto& keyPair : g_keyMaps) {
        bool currentState = (GetAsyncKeyState(keyPair.first) & 0x8000) != 0;
        if (currentState != keyPair.second.lastState) {
            SimulateKeyPress(keyPair.second.virtualKey, currentState);
            keyPair.second.lastState = currentState;
        }
    }
}

void WriteByte(DWORD address, BYTE value) {
    WriteProcessMemory(GetCurrentProcess(), (LPVOID)address, &value, sizeof(BYTE), nullptr);
}

void WriteWord(DWORD address, WORD value) {
    WriteProcessMemory(GetCurrentProcess(), (LPVOID)address, &value, sizeof(WORD), nullptr);
}

float ReadFloat(DWORD address) {
    float value = 0.0f;
    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, &value, sizeof(float), nullptr);
    return value;
}

void WriteFloat(DWORD address, float value) {
    WriteProcessMemory(GetCurrentProcess(), (LPVOID)address, &value, sizeof(float), nullptr);
}

void UpdateGameplayToggles() {
    WriteByte(EQUIPMENT_WEIGHT_ADDRESS, g_equipmentWeight ? 1 : 0);
    WriteByte(SAVE_ANYWHERE_ADDRESS, g_saveAnywhere ? 1 : 0);
    WriteByte(SPRINT_ADDRESS, g_sprinting ? 1 : 0);
}

void UpdateMovementSettings() {
    WriteFloat(WALK_SPEED_ADDRESS, g_walkSpeed);
    WriteFloat(SPRINT_SPEED_ADDRESS, g_sprintSpeed);
    WriteWord(TURN_RATE_ADDRESS, g_turnRate);
}

void PatchFlashEffects() {
    DWORD oldProtect;

    if (!g_damageFlashing) {
        unsigned char nop[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        VirtualProtect((LPVOID)RED_FLASH_INSTRUCTION, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((void*)RED_FLASH_INSTRUCTION, nop, 6);
        VirtualProtect((LPVOID)RED_FLASH_INSTRUCTION, 6, oldProtect, &oldProtect);
    }

    if (!g_poisonFlashing) {
        unsigned char nop[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        VirtualProtect((LPVOID)POISON_FLASH_INSTRUCTION, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((void*)POISON_FLASH_INSTRUCTION, nop, 6);
        VirtualProtect((LPVOID)POISON_FLASH_INSTRUCTION, 6, oldProtect, &oldProtect);
    }

    if (!g_darkFlashing) {
        unsigned char nop[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        VirtualProtect((LPVOID)BLACK_FLASH_INSTRUCTION, 7, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((void*)BLACK_FLASH_INSTRUCTION, nop, 7);
        VirtualProtect((LPVOID)BLACK_FLASH_INSTRUCTION, 7, oldProtect, &oldProtect);
    }
}

void UpdateHUDElements() {
    if (!g_showCompass) {
        WriteByte(COMPASS_ADDRESS, 255);
        WriteByte(COMPASS_BG_ADDRESS, 255);
    }

    if (!g_showMeleeStamina) {
        WriteByte(MELEE_STAMINA_ADDRESS, 255);
    }

    if (!g_showMagicStamina) {
        WriteByte(MAGIC_STAMINA_ADDRESS, 255);
    }

    if (!g_showHUDDecorations) {
        WriteByte(HUD_DECORATION_ADDRESS, 255);
    }

    if (!g_showStatusEffects) {
        WriteByte(STATUS_EFFECT_ADDRESS, 255);
    }

    if (!g_showHPMP) {
        WriteByte(HEALTH_MANA_ADDRESS, 255);
    }
}

void EnforceFOVSetting() {
    WriteFloat(FOV_ADDRESS, g_fov);
}

void UpdateCamera() {
    if (!g_trainerActive || !IsGameWindowActive() || IsInGameMenu()) {
        if (!g_wasBusy || g_wasActive) {
            g_lastCameraX = ReadFloat(CAMERA_X_ADDRESS);
            g_lastCameraY = ReadFloat(CAMERA_Y_ADDRESS);
            g_wasBusy = true;
            g_wasActive = false;
        }
        return;
    }

    if (g_wasBusy || !g_wasActive) {
        WriteFloat(CAMERA_X_ADDRESS, g_lastCameraX);
        WriteFloat(CAMERA_Y_ADDRESS, g_lastCameraY);
        g_wasBusy = false;
        g_wasActive = true;

        if (g_trainerActive) {
            const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            SetCursorPos(screenWidth / 2, screenHeight / 2);
        }
        return;
    }

    if (g_trainerActive) {
        POINT cursorPos;
        GetCursorPos(&cursorPos);

        const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        const int centerX = screenWidth / 2;
        const int centerY = screenHeight / 2;

        const float deltaX = static_cast<float>(cursorPos.x - centerX);
        const float deltaY = static_cast<float>(cursorPos.y - centerY);

        if (deltaX != 0.0f || deltaY != 0.0f) {
            float cameraX = ReadFloat(CAMERA_X_ADDRESS);
            cameraX = cameraX - (deltaX * g_sensitivity);
            cameraX = fmodf(cameraX + 3.14159f, 6.28318f) - 3.14159f;
            WriteFloat(CAMERA_X_ADDRESS, cameraX);
            g_lastCameraX = cameraX;

            float cameraY = ReadFloat(CAMERA_Y_ADDRESS);
            cameraY = cameraY - (deltaY * g_sensitivity);
            const float minY = -0.7853981853f;
            const float maxY = 0.7853981853f;
            cameraY = (cameraY < minY) ? minY : ((cameraY > maxY) ? maxY : cameraY);
            WriteFloat(CAMERA_Y_ADDRESS, cameraY);
            g_lastCameraY = cameraY;

            SetCursorPos(centerX, centerY);
        }
    }
}

void InstallSpeedHooks() {
    if (DetourTransactionBegin() != NO_ERROR) {
        return;
    }
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
        return;
    }
    DetourAttach(&(PVOID&)TrueGetTickCount64, HookedGetTickCount64);
    DetourAttach(&(PVOID&)TrueQueryPerformanceCounter, HookedQueryPerformanceCounter);
    DetourAttach(&(PVOID&)TrueTimeGetTime, HookedTimeGetTime);
    if (DetourTransactionCommit() != NO_ERROR) {
        return;
    }
}

void RemoveSpeedHooks() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)TrueGetTickCount64, HookedGetTickCount64);
    DetourDetach(&(PVOID&)TrueQueryPerformanceCounter, HookedQueryPerformanceCounter);
    DetourDetach(&(PVOID&)TrueTimeGetTime, HookedTimeGetTime);
    DetourTransactionCommit();
}

void EnforcePauseSettings() {
    if (!g_pauseWhenInactive) {
        WriteByte(PAUSE_INACTIVE_ADDRESS, 1);
    }
}

bool IsSupportedGame() {
    return true;
}

HWND GetCurrentProcessWindow() {
    DWORD processId = GetCurrentProcessId();
    HWND result = NULL;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        DWORD windowProcessId;
        GetWindowThreadProcessId(hwnd, &windowProcessId);
        if (windowProcessId == GetCurrentProcessId()) {
            *((HWND*)lParam) = hwnd;
            return FALSE;
        }
        return TRUE;
        }, (LPARAM)&result);

    return result;
}

void LoadConfig() {
    InitializeKeyNameMap();

    g_walkSpeed = ReadFloat(WALK_SPEED_ADDRESS);
    g_sprintSpeed = ReadFloat(SPRINT_SPEED_ADDRESS);
    g_turnRate = (WORD)ReadFloat(TURN_RATE_ADDRESS);
    g_fov = ReadFloat(FOV_ADDRESS);

    BYTE sprintEnabled = 0;
    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)SPRINT_ADDRESS, &sprintEnabled, sizeof(BYTE), nullptr);
    g_sprinting = (sprintEnabled != 0);
    BYTE equipmentWeight = 0;
    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)EQUIPMENT_WEIGHT_ADDRESS, &equipmentWeight, sizeof(BYTE), nullptr);
    g_equipmentWeight = (equipmentWeight != 0);
    BYTE saveAnywhere = 0;
    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)SAVE_ANYWHERE_ADDRESS, &saveAnywhere, sizeof(BYTE), nullptr);
    g_saveAnywhere = (saveAnywhere != 0);

    std::ifstream config("config.txt");
    if (config.is_open()) {
        std::string line;
        bool inKeybindings = false;
        while (std::getline(config, line)) {
            line = trim(line);
            if (line.empty()) continue;

            if (line.substr(0, 2) == "//") {
                if (line.find("Keybindings") != std::string::npos) {
                    inKeybindings = true;
                }
                continue;
            }

            size_t equalPos = line.find("=");
            if (equalPos != std::string::npos) {
                std::string key = trim(line.substr(0, equalPos));
                std::string value = trim(line.substr(equalPos + 1));

                if (inKeybindings) {
                    auto targetIt = g_actionToVirtualKey.find(toUpper(key));
                    if (targetIt != g_actionToVirtualKey.end()) {
                        int sourceKey = GetVirtualKeyFromName(value);
                        if (sourceKey != 0) {
                            g_keyMaps[sourceKey] = { targetIt->second, false };
                        }
                    }
                }
                else {
                    if (_stricmp(key.c_str(), "MOUSE SENSITIVITY") == 0) {
                        try {
                            float newSensitivity = std::stof(value);
                            if (newSensitivity > 0.0f) {
                                g_sensitivity = newSensitivity;
                            }
                        }
                        catch (...) {}
                    }
                    else if (_stricmp(key.c_str(), "FOV (RADIANS)") == 0) {
                        try {
                            float newFov = std::stof(value);
                            g_fov = newFov;
                            WriteFloat(FOV_ADDRESS, g_fov);
                        }
                        catch (...) {}
                    }
                    else if (_stricmp(key.c_str(), "SPRINT ENABLED") == 0) {
                        g_sprinting = (_stricmp(value.c_str(), "TRUE") == 0);
                        WriteByte(SPRINT_ADDRESS, g_sprinting ? 1 : 0);
                    }
                    else if (_stricmp(key.c_str(), "SAVE ANYWHERE") == 0) {
                        g_saveAnywhere = (_stricmp(value.c_str(), "TRUE") == 0);
                        WriteByte(SAVE_ANYWHERE_ADDRESS, g_saveAnywhere ? 1 : 0);
                    }
                    else if (_stricmp(key.c_str(), "INACTIVE WINDOW PAUSE") == 0) {
                        g_pauseWhenInactive = (_stricmp(value.c_str(), "TRUE") == 0);
                    }
                    else if (_stricmp(key.c_str(), "MODERN KEYBINDINGS") == 0) {
                        g_modernControls = (_stricmp(value.c_str(), "TRUE") == 0);
                    }
                    else if (_stricmp(key.c_str(), "MOUSELOOK") == 0) {
                        g_trainerActive = (_stricmp(value.c_str(), "TRUE") == 0);
                    }
                    else if (_stricmp(key.c_str(), "GAMEPLAY SPEED") == 0) {
                        try {
                            float speed = std::stof(value);
                            if (speed > 0.0f) {
                                g_speedMultiplier = speed;
                            }
                        }
                        catch (...) {}
                    }
                    else if (_stricmp(key.c_str(), "WALK SPEED") == 0) {
                        try {
                            float speed = std::stof(value);
                            if (speed >= 0.0f) {
                                g_walkSpeed = speed;
                                WriteFloat(WALK_SPEED_ADDRESS, speed);
                            }
                        }
                        catch (...) {}
                    }
                    else if (_stricmp(key.c_str(), "SPRINT SPEED") == 0) {
                        try {
                            float speed = std::stof(value);
                            if (speed >= 0.0f) {
                                g_sprintSpeed = speed;
                                WriteFloat(SPRINT_SPEED_ADDRESS, speed);
                            }
                        }
                        catch (...) {}
                    }
                    else if (_stricmp(key.c_str(), "TURN RATE") == 0) {
                        try {
                            int rate = std::stoi(value);
                            if (rate >= 0 && rate <= 999) {
                                g_turnRate = static_cast<WORD>(rate);
                                WriteWord(TURN_RATE_ADDRESS, g_turnRate);
                            }
                        }
                        catch (...) {}
                    }
                }
            }
        }
        config.close();
    }

    if (!g_pauseWhenInactive) {
        EnforcePauseSettings();
    }

    PatchFlashEffects();
    UpdateHUDElements();
}

DWORD WINAPI MainThread(LPVOID param) {
    QueryPerformanceFrequency(&g_qpcFrequency);
    QueryPerformanceCounter(&g_startCounter);

    LoadConfig();
    g_gameWindow = GetCurrentProcessWindow();

    InstallSpeedHooks();

    g_lastCameraX = ReadFloat(CAMERA_X_ADDRESS);
    g_lastCameraY = ReadFloat(CAMERA_Y_ADDRESS);

    UpdateHUDElements();

    while (true) {
        UpdateCamera();
        HandleKeybinds();
        EnforcePauseSettings();
        EnforceFOVSetting();
        UpdateHUDElements();
        UpdateCurrentSpeed();
        UpdateGameplayToggles();
        UpdateMovementSettings();
        Sleep(1);
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        if (!ShouldHookProcess()) {
            return TRUE;
        }
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        if (ShouldHookProcess()) {
            RemoveSpeedHooks();
        }
        break;
    }
    return TRUE;
}