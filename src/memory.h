// --- src/memory.h ---
#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

class Memory {
public:
    HANDLE    process_handle = nullptr;
    uintptr_t base_address   = 0;
    uintptr_t client_dll     = 0;

    bool attach(const std::wstring& process_name);
    void detach();

    template <typename T>
    T read(uintptr_t address) const {
        static_assert(std::is_trivially_copyable_v<T>, "read<T> requires trivially copyable T");
        T value{};
        if (process_handle)
            ReadProcessMemory(process_handle,
                reinterpret_cast<LPCVOID>(address), &value, sizeof(T), nullptr);
        return value;
    }

    template <typename T>
    void write(uintptr_t address, const T& value) const {
        static_assert(std::is_trivially_copyable_v<T>, "write<T> requires trivially copyable T");
        if (process_handle)
            WriteProcessMemory(process_handle,
                reinterpret_cast<LPVOID>(address), &value, sizeof(T), nullptr);
    }

    // Bulk read into a caller buffer; false on any failure.
    bool read_bytes(uintptr_t address, void* out, std::size_t size) const {
        if (!process_handle) return false;
        return ReadProcessMemory(process_handle,
            reinterpret_cast<LPCVOID>(address), out, size, nullptr) == TRUE;
    }

    // Exactly what your working build had.
    bool is_valid() const {
        return process_handle != nullptr && client_dll != 0;
    }

    uintptr_t get_module_base(DWORD pid, const std::wstring& module_name) const;

private:
    DWORD get_process_id(const std::wstring& process_name) const;
};

extern Memory g_mem;
