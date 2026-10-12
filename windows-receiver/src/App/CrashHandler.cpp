// =============================================================================
// CrashHandler.cpp — Unhandled exception / terminate reporting (DbgHelp)
// =============================================================================

#include "App/CrashHandler.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>

#pragma comment(lib, "dbghelp.lib")

namespace {

wchar_t gTextPath[MAX_PATH] = {};
wchar_t gDumpPath[MAX_PATH] = {};
volatile LONG gReporting = 0;

// Write to the console and the crash report file
void Out(FILE* file, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fputs(buf, stderr);
    if (file) fputs(buf, file);
}

void WalkStack(CONTEXT ctx, FILE* file) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);

    STACKFRAME64 frame = {};
    DWORD machine = 0;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = ctx.Pc;
    frame.AddrFrame.Offset = ctx.Fp;
    frame.AddrStack.Offset = ctx.Sp;
#else
    Out(file, "  (stack walk not supported on this architecture)\n");
    return;
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;

    Out(file, "Stack:\n");
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machine, process, thread, &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        DWORD64 addr = frame.AddrPC.Offset;
        if (addr == 0) break;

        // Module name
        char module[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &hm)) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(hm, path, MAX_PATH)) {
                const char* base = strrchr(path, '\\');
                strncpy_s(module, base ? base + 1 : path, _TRUNCATE);
            }
        }

        // Function + source line (needs the .pdb next to the .exe)
        ULONG64 symBuf[(sizeof(SYMBOL_INFO) + 256 + sizeof(ULONG64) - 1) / sizeof(ULONG64)] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;
        DWORD64 symDisp = 0;
        const char* name = SymFromAddr(process, addr, &symDisp, sym) ? sym->Name : "?";

        IMAGEHLP_LINE64 line = { sizeof(line) };
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(process, addr, &lineDisp, &line)) {
            Out(file, "  #%02d %s!%s+0x%llx  (%s:%lu)\n", i, module, name,
                (unsigned long long)symDisp, line.FileName, line.LineNumber);
        } else {
            Out(file, "  #%02d %s!%s+0x%llx\n", i, module, name, (unsigned long long)symDisp);
        }
    }
    SymCleanup(process);
}

void WriteDump(EXCEPTION_POINTERS* info) {
    HANDLE file = CreateFileW(gDumpPath, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), info, FALSE };
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                                                 MiniDumpWithIndirectlyReferencedMemory),
                      info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

// Shared report path. `info` is null when there is no SEH exception record.
void ReportFatal(const char* reason, EXCEPTION_POINTERS* info) {
    if (InterlockedExchange(&gReporting, 1) != 0) return;  // one report only

    FILE* file = nullptr;
    _wfopen_s(&file, gTextPath, L"w");

    Out(file, "\n*** CRASH: %s (thread %lu)\n", reason, GetCurrentThreadId());
    if (info) {
        const EXCEPTION_RECORD* rec = info->ExceptionRecord;
        Out(file, "Exception 0x%08lX at %p\n",
            (unsigned long)rec->ExceptionCode, rec->ExceptionAddress);
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            Out(file, "Access violation %s address %p\n",
                rec->ExceptionInformation[0] == 1 ? "writing" : "reading",
                reinterpret_cast<void*>(rec->ExceptionInformation[1]));
        }
        WalkStack(*info->ContextRecord, file);
    } else {
        CONTEXT ctx = {};
        RtlCaptureContext(&ctx);
        WalkStack(ctx, file);
    }

    WriteDump(info);
    Out(file, "*** Report saved next to the .exe: receiver-crash.txt / receiver-crash.dmp\n");
    if (file) fclose(file);
    fflush(stderr);

    MessageBoxW(nullptr,
        L"The receiver crashed.\n\nA report was saved next to the .exe:\n"
        L"  receiver-crash.txt\n  receiver-crash.dmp\n\n"
        L"Please send receiver-crash.txt.",
        L"External Display Receiver", MB_ICONERROR | MB_OK | MB_TOPMOST);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    ReportFatal("unhandled exception", info);
    return EXCEPTION_EXECUTE_HANDLER;
}

void OnTerminate() {
    // Typically an exception escaping a std::thread
    const char* reason = "std::terminate";
    char buf[512];
    if (std::exception_ptr ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            snprintf(buf, sizeof(buf), "uncaught C++ exception: %s", e.what());
            reason = buf;
        } catch (...) {
            reason = "uncaught non-standard C++ exception";
        }
    }
    ReportFatal(reason, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    ReportFatal("invalid parameter passed to a CRT function", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnPureCall() {
    ReportFatal("pure virtual function call", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnAbort(int) {
    ReportFatal("abort()", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void PathNextToExe(wchar_t* out, const wchar_t* fileName) {
    GetModuleFileNameW(nullptr, out, MAX_PATH);
    wchar_t* slash = wcsrchr(out, L'\\');
    wchar_t* dir = slash ? slash + 1 : out;
    wcsncpy_s(dir, static_cast<size_t>(MAX_PATH - (dir - out)), fileName, _TRUNCATE);
}

} // namespace

void InstallCrashHandler() {
    PathNextToExe(gTextPath, L"receiver-crash.txt");
    PathNextToExe(gDumpPath, L"receiver-crash.dmp");

    SetUnhandledExceptionFilter(OnUnhandledException);
    std::set_terminate(OnTerminate);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
    signal(SIGABRT, OnAbort);
}
