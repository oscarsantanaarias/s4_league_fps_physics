#include "pch.h"
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstring>
#include <thread>
#include <chrono>

#pragma comment(lib, "psapi.lib")

const int TARGET_FPS = 360;
const float TARGET_FRAMETIME = 1.0f / TARGET_FPS;
const float BASE_FRAMETIME = 1.0f / 60.0f;

static LARGE_INTEGER freq;
static LARGE_INTEGER lastFrame;
static float currentFrametime = 0.0f;

// not sure if this is actually used but some games use this array for physics
float* speedDampeners = nullptr;

uintptr_t FindPattern(const uint8_t* pattern, const char* mask, uintptr_t base, uintptr_t size) {
    for (uintptr_t i = 0; i < size - strlen(mask); i++) {
        bool found = true;
        for (size_t j = 0; mask[j]; j++) {
            if (mask[j] != '?' && pattern[j] != *(uint8_t*)(base + i + j)) {
                found = false;
                break;
            }
        }
        if (found) {
            return base + i;
        }
    }
    return 0;
}

void PatchMemory(uintptr_t address, const uint8_t* patch, size_t size) {
    DWORD oldProtect;
    VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)address, patch, size);
    VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
}

// quick way to patch FPS limiter (just skip it)
void UnlockFPS() {
    MODULEINFO moduleInfo = { 0 };
    HMODULE hModule = GetModuleHandle(NULL);
    if (hModule && GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo))) {
        uintptr_t base = (uintptr_t)moduleInfo.lpBaseOfDll;
        uintptr_t size = moduleInfo.SizeOfImage;

        uint8_t fpsPattern[] = { 0x7E, 0x0A, 0x6A, 0x01, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x02, 0xEB, 0x05, 0xE9 };
        const char fpsMask[] = "xxxxxx????xxxxx";

        uintptr_t fpsAddress = FindPattern(fpsPattern, fpsMask, base, size);
        if (fpsAddress) {
            uint8_t patch = 0xEB;
            PatchMemory(fpsAddress, &patch, 1); // force jump always
        }
    }
}

// kinda ugly but patches physics dampener array directly
void ApplyPhysicsFix() {
    if (!speedDampeners) return;

    float original = 0.015f;
    float fixed = currentFrametime * original / BASE_FRAMETIME;

    speedDampeners[3] = fixed; // probably horizontal
    speedDampeners[4] = fixed; // maybe vertical?
    speedDampeners[8] = fixed; // might be camera or something
}

// limit framerate manually so it doesn't go nuts
void LimitFPS() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    LONGLONG elapsed = now.QuadPart - lastFrame.QuadPart;
    LONGLONG waitTicks = (LONGLONG)(freq.QuadPart * TARGET_FRAMETIME);

    currentFrametime = (float)elapsed / freq.QuadPart;

    if (elapsed < waitTicks) {
        LONGLONG sleepTime = waitTicks - elapsed;
        DWORD sleepMs = (DWORD)(sleepTime * 1000 / freq.QuadPart);

        if (sleepMs > 1)
            Sleep(sleepMs - 1);

        do {
            QueryPerformanceCounter(&now);
            elapsed = now.QuadPart - lastFrame.QuadPart;
        } while (elapsed < waitTicks);
    }

    lastFrame = now;
}

void LocateSpeedDampeners() {
    // guessing based on pattern
    MODULEINFO moduleInfo = { 0 };
    HMODULE hModule = GetModuleHandle(NULL);
    if (hModule && GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo))) {
        uintptr_t base = (uintptr_t)moduleInfo.lpBaseOfDll;
        uintptr_t size = moduleInfo.SizeOfImage;

        uint8_t physicsPattern[] = { 0xD9, 0x05, 0x00, 0x00, 0x00, 0x00, 0xD8, 0x77, 0x00 };
        const char physicsMask[] = "xx????xx?";

        uintptr_t addr = FindPattern(physicsPattern, physicsMask, base, size);
        if (addr) {
            uintptr_t ptr = *(uintptr_t*)(addr + 2); // address is loaded as address
            speedDampeners = (float*)ptr;
        }
    }
}

DWORD WINAPI MainThread(LPVOID) {
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastFrame);

    UnlockFPS();
    LocateSpeedDampeners();

    while (true) {
        LimitFPS();
        ApplyPhysicsFix();
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // just so it's not 100% CPU
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    return TRUE;
}
