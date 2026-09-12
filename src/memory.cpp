// --- src/memory.cpp ---
#include "memory.h"

bool Memory::attach(const std::wstring& process_name) {
    DWORD pid = get_process_id(process_name);
    if (!pid) return false;

    process_handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process_handle) return false;

    base_address = get_module_base(pid, process_name);
    return base_address != 0;
}

void Memory::detach() {
    if (process_handle) {
        CloseHandle(process_handle);
        process_handle = nullptr;
    }
    base_address = 0;
}

DWORD Memory::get_process_id(const std::wstring& process_name) const {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snap, &entry)) {
        do {
            if (process_name == entry.szExeFile) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

uintptr_t Memory::get_module_base(DWORD pid, const std::wstring& module_name) const {
    uintptr_t base = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snap, &entry)) {
        do {
            if (module_name == entry.szModule) {
                base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return base;
}
