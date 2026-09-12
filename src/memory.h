// --- src/memory.h ---
#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <string>

class Memory {
public:
    HANDLE process_handle = nullptr;
    uintptr_t base_address = 0;

    bool attach(const std::wstring& process_name);
    void detach();

    template<typename T>
    T read(uintptr_t address) const {
        T value{};
        ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(address), &value, sizeof(T), nullptr);
        return value;
    }

    bool is_valid() const { return process_handle != nullptr && base_address != 0; }

private:
    DWORD get_process_id(const std::wstring& process_name) const;
    uintptr_t get_module_base(DWORD pid, const std::wstring& module_name) const;
};
