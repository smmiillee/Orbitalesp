// --- src/memory.h ---
#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <string>

class Memory {
public:
    HANDLE    process_handle = nullptr;
    uintptr_t base_address   = 0;
    uintptr_t client_dll     = 0;

    bool attach(const std::wstring& process_name);
    void detach();

    template<typename T>
    T read(uintptr_t address) const {
        T value{};
        ReadProcessMemory(process_handle,
            reinterpret_cast<LPCVOID>(address),
            &value, sizeof(T), nullptr);
        return value;
    }

    template<typename T>
    void write(uintptr_t address, T value) const {
        WriteProcessMemory(process_handle,
            reinterpret_cast<LPVOID>(address),
            &value, sizeof(T), nullptr);
    }

    bool read_bytes(uintptr_t address, void* buf, size_t size) const {
        SIZE_T bytes_read = 0;
        return ReadProcessMemory(process_handle,
            reinterpret_cast<LPCVOID>(address),
            buf, size, &bytes_read) && bytes_read == size;
    }

    bool is_valid() const {
        return process_handle != nullptr && client_dll != 0;
    }

    uintptr_t get_module_base(DWORD pid, const std::wstring& module_name) const;

private:
    DWORD get_process_id(const std::wstring& process_name) const;
};

extern Memory g_mem;
