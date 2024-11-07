#include "pch.h"
#include <windows.h>
#include <string>
#include <fstream>
#include <map>
#include <vector>
#include <algorithm>
#include "detours.h"

#pragma comment(lib, "detours.lib")
#pragma comment(lib, "winmm.lib")

// Global variables
float g_speedMultiplier = 1.0f;
LARGE_INTEGER g_startCounter;
double g_counterToSecondScale;

// Function prototypes for hooks
typedef DWORD(WINAPI* GetTickCount_t)(void);
typedef BOOL(WINAPI* QueryPerformanceCounter_t)(LARGE_INTEGER*);
typedef DWORD(WINAPI* TimeGetTime_t)(void);

// Original functions
static GetTickCount_t TrueGetTickCount = GetTickCount;
static QueryPerformanceCounter_t TrueQueryPerformanceCounter = QueryPerformanceCounter;
static TimeGetTime_t TrueTimeGetTime = timeGetTime;

// Hooked function implementations
DWORD WINAPI HookedGetTickCount() {
    DWORD originalTick = TrueGetTickCount();
    return static_cast<DWORD>(originalTick * g_speedMultiplier);
}

BOOL WINAPI HookedQueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount) {
    BOOL result = TrueQueryPerformanceCounter(lpPerformanceCount);
    if (result && g_speedMultiplier != 1.0f) {
        LARGE_INTEGER current;
        TrueQueryPerformanceCounter(&current);
        double elapsedSeconds = (current.QuadPart - g_startCounter.QuadPart) * g_counterToSecondScale;
        double modifiedSeconds = elapsedSeconds * g_speedMultiplier;
        lpPerformanceCount->QuadPart = g_startCounter.QuadPart + static_cast<LONGLONG>(modifiedSeconds / g_counterToSecondScale);
    }
    return result;
}

DWORD WINAPI HookedTimeGetTime() {
    DWORD originalTime = TrueTimeGetTime();
    return static_cast<DWORD>(originalTime * g_speedMultiplier);
}

// Helper function to trim strings
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t");
    return str.substr(first, (last - first + 1));
}

// Load speed multiplier from config
void LoadConfig() {
    std::ifstream config("config.txt");
    if (config.is_open()) {
        std::string line;
        while (std::getline(config, line)) {
            if (line.substr(0, 2) == "//") continue;

            size_t equalPos = line.find("=");
            if (equalPos != std::string::npos) {
                std::string key = trim(line.substr(0, equalPos));
                std::string value = trim(line.substr(equalPos + 1));

                if (_stricmp(key.c_str(), "GAME SPEED") == 0) {
                    try {
                        float speed = std::stof(value);
                        if (speed > 0.0f) {
                            g_speedMultiplier = speed;
                        }
                    }
                    catch (...) {}
                }
            }
        }
        config.close();
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // Initialize timing
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_counterToSecondScale = 1.0 / static_cast<double>(frequency.QuadPart);
        QueryPerformanceCounter(&g_startCounter);

        // Load config
        LoadConfig();

        // Only install hooks if speed is not 1.0
        if (g_speedMultiplier != 1.0f) {
            if (DetourTransactionBegin() != NO_ERROR) {
                return FALSE;
            }

            if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
                return FALSE;
            }

            DetourAttach(&(PVOID&)TrueGetTickCount, HookedGetTickCount);
            DetourAttach(&(PVOID&)TrueQueryPerformanceCounter, HookedQueryPerformanceCounter);
            DetourAttach(&(PVOID&)TrueTimeGetTime, HookedTimeGetTime);

            if (DetourTransactionCommit() != NO_ERROR) {
                return FALSE;
            }
        }
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_speedMultiplier != 1.0f) {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourDetach(&(PVOID&)TrueGetTickCount, HookedGetTickCount);
            DetourDetach(&(PVOID&)TrueQueryPerformanceCounter, HookedQueryPerformanceCounter);
            DetourDetach(&(PVOID&)TrueTimeGetTime, HookedTimeGetTime);
            DetourTransactionCommit();
        }
    }

    return TRUE;
}