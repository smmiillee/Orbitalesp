#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <cstdint>

class Memory {
public:
    HANDLE process = nullptr;
    uintptr_t clientBase = 0;

    bool Attach(const std::string& procName) {
        DWORD pid = GetPID(procName);
        if (!pid) return false;
        // PROCESS_VM_READ only — zero writes to game memory ever
        process = OpenProcess(PROCESS_VM_READ, FALSE, pid);
        return process != nullptr;
    }

    bool GetModule(const std::string& modName) {
        DWORD pid = 0;
        GetWindowThreadProcessId(FindWindowA("SDL_app", nullptr), &pid);
        if (!pid) return false;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return false;

        MODULEENTRY32 me32{};
        me32.dwSize = sizeof(me32);
        bool found = false;

        if (Module32First(snap, &me32)) {
            do {
                if (modName == me32.szModule) {
                    clientBase = (uintptr_t)me32.modBaseAddr;
                    found = true;
                    break;
                }
            } while (Module32Next(snap, &me32));
        }

        CloseHandle(snap);
        return found;
    }

    template<typename T>
    T Read(uintptr_t addr) const {
        T val{};
        ReadProcessMemory(process, (LPCVOID)addr, &val, sizeof(T), nullptr);
        return val;
    }

    std::string ReadString(uintptr_t addr, size_t maxLen = 64) const {
        char buf[128]{};
        ReadProcessMemory(process, (LPCVOID)addr, buf,
            min(maxLen, sizeof(buf) - 1), nullptr);
        return std::string(buf);
    }

private:
    DWORD GetPID(const std::string& name) const {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;

        if (Process32First(snap, &pe)) {
            do {
                if (name == pe.szExeFile) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }

        CloseHandle(snap);
        return pid;
    }
};

inline Memory g_Mem;
