#include <windows.h>
#include <psapi.h>
#include <common/Win32Error.h>
#include <ostream>
#include <fstream>
#include <ctime>
#include <cassert>
#include <algorithm>
#include <unordered_map>
#include <optional>
#include <cstddef>
#include "ProcessMemoryCache.h"

inline std::string StringFormat(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int size = vsnprintf(nullptr, 0, format, args) + 1;// Extra space for '\0'
    va_end(args);
    std::unique_ptr<char[]> buf(new char[size]);
    va_start(args, format);
    vsnprintf(buf.get(), size, format, args);
    va_end(args);
    return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

class TextDumpHelper {
private:
    SYSTEM_INFO m_sys_info;
    std::vector<MEMORY_BASIC_INFORMATION> m_memory_map;
    ProcessMemoryCache m_mem_cache;

public:
    TextDumpHelper(HANDLE process) : m_mem_cache(process) {
        GetSystemInfo(&m_sys_info);
    }

    void write_dump(const char* filename, EXCEPTION_POINTERS* exception_pointers, HANDLE process)
    {
        std::fstream out(filename, std::fstream::out);
        write_dump(out, exception_pointers, process);
    }

    void write_dump(std::ostream& out, EXCEPTION_POINTERS* exception_pointers, HANDLE process)
    {
        auto [exc_rec, ctx] = fetch_exception_info(exception_pointers);

        out << "Unhandled exception\n";
        write_current_time(out);
        if (exc_rec.ExceptionCode == STATUS_ACCESS_VIOLATION && exc_rec.NumberParameters >= 2) {
            auto prefix = exc_rec.ExceptionInformation[0] ? "Write to" : "Read from";
            out << prefix << StringFormat(" location %08X caused an access violation.\n", exc_rec.ExceptionInformation[1]);
        }
        out << std::endl;
        write_exception_info(out, exc_rec);
        write_context(out, ctx);

        m_memory_map = fetch_memory_map(process);

        write_ebp_based_backtrace(out, ctx);
        write_backtrace(out, ctx, process);
        write_stack_dump(out, ctx, process);
        write_modules(out, process);
        write_memory_map(out);
    }

private:
    std::tuple<EXCEPTION_RECORD, CONTEXT> fetch_exception_info(EXCEPTION_POINTERS* exception_pointers)
    {
        EXCEPTION_POINTERS exception_pointers_local;

        auto exception_ptrs_addr = reinterpret_cast<uintptr_t>(exception_pointers);
        if (!m_mem_cache.read(exception_ptrs_addr, &exception_pointers_local, sizeof(exception_pointers_local))) {
            THROW_WIN32_ERROR();
        }

        CONTEXT context;
        auto context_record_addr = reinterpret_cast<uintptr_t>(exception_pointers_local.ContextRecord);
        if (!m_mem_cache.read(context_record_addr, &context, sizeof(context))) {
            THROW_WIN32_ERROR();
        }

        EXCEPTION_RECORD exception_record;
        auto exception_record_addr = reinterpret_cast<uintptr_t>(exception_pointers_local.ExceptionRecord);
        if (!m_mem_cache.read(exception_record_addr, &exception_record, sizeof(exception_record))) {
            THROW_WIN32_ERROR();
        }

        return {exception_record, context};
    }

    void write_current_time(std::ostream& out)
    {
        time_t rawtime;
        time(&rawtime);
        auto local_time_info = localtime(&rawtime);
        out << "Date and time (local): " << asctime(local_time_info);
        auto utc_time_info = gmtime(&rawtime);
        out << "Date and time (UTC): " << asctime(utc_time_info);
    }

    void write_exception_info(std::ostream& out, const EXCEPTION_RECORD& exc_rec)
    {
        out << "Exception Record:\n";
        out << "  ExceptionCode = " << StringFormat("%08X", exc_rec.ExceptionCode) << "\n";
        out << "  ExceptionFlags = " << StringFormat("%08X", exc_rec.ExceptionFlags) << "\n";
        out << "  ExceptionAddress = " << StringFormat("%08X", exc_rec.ExceptionAddress) << "\n";
        for (unsigned i = 0; i < exc_rec.NumberParameters; ++i)
            out << "  ExceptionInformation[" << i << "] = " << StringFormat("%08X", exc_rec.ExceptionInformation[i]) << "\n";
        out << std::endl;
    }

    void write_context(std::ostream& out, const CONTEXT& ctx)
    {
        out << "Context:\n";
        out << StringFormat("EIP=%08X EFLAGS=%08X\n", ctx.Eip, ctx.EFlags);
        out << StringFormat("EAX=%08X EBX=%08X ECX=%08X EDX=%08X\n", ctx.Eax, ctx.Ebx, ctx.Ecx, ctx.Edx);
        out << StringFormat("ESP=%08X EBP=%08X ESI=%08X EDI=%08X\n", ctx.Esp, ctx.Ebp, ctx.Esi, ctx.Edi);
        out << std::endl;
    }

    std::vector<MEMORY_BASIC_INFORMATION> fetch_memory_map(HANDLE process)
    {
        MEMORY_BASIC_INFORMATION mbi;
        std::vector<MEMORY_BASIC_INFORMATION> memory_map;
        const char* addr = reinterpret_cast<const char*>(m_sys_info.lpMinimumApplicationAddress);
        while (VirtualQueryEx(process, addr, &mbi, sizeof(mbi)) && addr <= m_sys_info.lpMaximumApplicationAddress) {
            memory_map.push_back(mbi);
            addr = reinterpret_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
        }
        return memory_map;
    }

    void write_memory_map(std::ostream& out)
    {
        out << "Memory map:\n";
        for (auto& mbi: m_memory_map) {
            out << StringFormat("%08X: State %08X Protect %08X Type %08X RegionSize %08X\n",
                mbi.BaseAddress, mbi.State, mbi.Protect, mbi.Type, mbi.RegionSize);
            //out << StringFormat("  AllocBase %x AllocProtect %x\n", mbi.AllocationBase, mbi.AllocationProtect);
        }
        out << std::endl;
    }

    bool is_executable_addr(void* addr)
    {
        if (addr < m_sys_info.lpMinimumApplicationAddress || addr > m_sys_info.lpMaximumApplicationAddress) {
            return false;
        }
        auto mem_map_it = find_in_memory_map(addr);
        if (mem_map_it == m_memory_map.end()) {
            return false;
        }
        auto protect = mem_map_it->Protect;
        return protect == PAGE_EXECUTE
            || protect == PAGE_EXECUTE_READ
            || protect == PAGE_EXECUTE_READWRITE
            || protect == PAGE_EXECUTE_WRITECOPY;
    }

    void write_ebp_based_backtrace(std::ostream& out, const CONTEXT& ctx)
    {
        out << "Backtrace (EBP chain):\n";
        out << StringFormat("%08X\n", ctx.Eip);
        auto frame_addr = ctx.Ebp;
        uintptr_t stack_max_addr = reinterpret_cast<uintptr_t>(find_stack_max_addr(ctx));
        for (int i = 0; i < 1000; ++i) {
            // Check EBP points to stack area
            if (frame_addr < ctx.Esp || frame_addr >= stack_max_addr) {
                break;
            }
            // Read next frame pointer and return address
            uintptr_t buf[2];
            if (!m_mem_cache.read(frame_addr, &buf, sizeof(buf))) {
                break;
            }
            // Make sure we don't have an infinite loop here
            if (buf[0] == frame_addr) {
                break;
            }
            frame_addr = buf[0];
            // Check if return address points to executable section
            auto ret_addr = buf[1];
            auto call_addr = ret_addr - 5;
            if (!is_executable_addr(reinterpret_cast<void*>(call_addr))) {
                continue;
            }
            // Check if return address points near a call instruction
            unsigned char insn_buf[5];
            if (!m_mem_cache.read(call_addr, insn_buf, sizeof(insn_buf))) {
                continue;
            }
            if (insn_buf[0] != 0xE8 && insn_buf[2] != 0xFF && insn_buf[3] != 0xFF) {
                break;
            }
            // All checks succeeded so print the address
            out << StringFormat("%08X\n", static_cast<unsigned>(call_addr));
        }
        out << std::endl;
    }

    void write_backtrace(std::ostream& out, const CONTEXT& ctx, HANDLE process)
    {
        out << "Backtrace (potential calls):\n";
        out << StringFormat("%08X\n", ctx.Eip);

        SIZE_T bytes_read;
        uintptr_t addr = ctx.Esp & ~3;
        uintptr_t stack_max_addr = reinterpret_cast<uintptr_t>(find_stack_max_addr(ctx));
        size_t stack_size = stack_max_addr - addr;
        auto stack_buf = std::make_unique<uint32_t[]>(stack_size / sizeof(uint32_t));

        bool read_success = ReadProcessMemory(process, reinterpret_cast<void*>(addr), stack_buf.get(), stack_size, &bytes_read);
        if (!read_success || !bytes_read) {
            return;
        }

        uint8_t insn_buf[6];
        size_t dwords_read = bytes_read / sizeof(uint32_t);
        for (unsigned i = 0; i < dwords_read; ++i) {
            uint32_t potential_call_addr = stack_buf[i] - 6;
            if (!is_executable_addr(reinterpret_cast<void*>(potential_call_addr))) {
                continue;
            }

            if (!m_mem_cache.read(potential_call_addr, insn_buf, sizeof(insn_buf))) {
                continue;
            }

            if (insn_buf[1] == 0xE8) {
                out << StringFormat("%08X\n", static_cast<unsigned>(potential_call_addr + 1));
            }
            else if (insn_buf[0] == 0xFF && insn_buf[1] == 0x15) {
                out << StringFormat("%08X\n", static_cast<unsigned>(potential_call_addr));
            }
        }

        out << std::endl;
    }

    std::vector<MEMORY_BASIC_INFORMATION>::const_iterator find_in_memory_map(void* addr) const
    {
        auto first = std::lower_bound(m_memory_map.begin(), m_memory_map.end(), addr, [](const auto& e, const auto& v) {
            return e.BaseAddress < v;
        });
        if (first == m_memory_map.end()) {
            return m_memory_map.end();
        }
        if (first->BaseAddress > addr) {
            if (first == m_memory_map.begin()) {
                return m_memory_map.end();
            }
            --first;
        }
        assert(first->BaseAddress <= addr);
        return first;
    }

    const MEMORY_BASIC_INFORMATION* find_page_in_memory_map(void* addr)
    {
        auto first = std::lower_bound(m_memory_map.begin(), m_memory_map.end(), addr, [](const auto& e, const auto& v) {
            return e.BaseAddress < v;
        });
        if (first == m_memory_map.end()) {
            return nullptr;
        }
        if (first->BaseAddress > addr) {
            if (first == m_memory_map.begin()) {
                return nullptr;
            }
            --first;
        }
        assert(first->BaseAddress <= addr);
        return &*first;
    }

    void* find_stack_max_addr(const CONTEXT& ctx)
    {
        void* addr = reinterpret_cast<void*>(ctx.Esp);
        auto it = find_in_memory_map(addr);

        while (it != m_memory_map.end()) {
            if (it->State != MEM_COMMIT || it->Protect != PAGE_READWRITE) {
                return it->BaseAddress;
            }
            ++it;
        }
        return addr;
    }

    void write_stack_dump(std::ostream& out, const CONTEXT& ctx, HANDLE process)
    {
        SIZE_T bytes_read;
        uintptr_t addr = ctx.Esp & ~3;
        uintptr_t stack_max_addr = reinterpret_cast<uintptr_t>(find_stack_max_addr(ctx));
        size_t stack_size = stack_max_addr - addr;
        auto buf = std::make_unique<uint32_t[]>(stack_size / sizeof(uint32_t));
        out << "Stack dump:\n";

        bool read_success = ReadProcessMemory(process, reinterpret_cast<void*>(addr), buf.get(), stack_size, &bytes_read);
        if (!read_success || !bytes_read) {
            return;
        }

        int col = 0;
        size_t dwords_read = bytes_read / sizeof(uint32_t);
        for (unsigned i = 0; i < dwords_read; ++i) {
            if (col == 0) {
                out << StringFormat("%08X: ", addr + i * 4);
            }

            out << StringFormat("%08X", buf[i]);
            col = (col + 1) % 8;
            out << (col == 0 ? '\n' : ' ');
        }
        out << std::endl;
    }

    void write_modules(std::ostream& out, HANDLE process)
    {
        out << "Modules:\n";

        HMODULE modules[256];
        DWORD bytes_needed;
        if (EnumProcessModules(process, modules, sizeof(modules), &bytes_needed)) {
            size_t num_modules = std::min<size_t>(bytes_needed / sizeof(HMODULE), std::size(modules));
            std::sort(modules, modules + num_modules);

            char mod_name[MAX_PATH];
            MODULEINFO mod_info;
            for (size_t i = 0; i < num_modules; i++) {
                if (GetModuleFileNameEx(process, modules[i], mod_name, std::size(mod_name)) &&
                    GetModuleInformation(process, modules[i], &mod_info, sizeof(mod_info))) {
                    uintptr_t start_addr = reinterpret_cast<uintptr_t>(mod_info.lpBaseOfDll);
                    uintptr_t end_addr = start_addr + mod_info.SizeOfImage;
                    out << StringFormat("%08X - %08X: %s\n", start_addr, end_addr, mod_name);
                }
            }
        }

        out << std::endl;
    }
};
