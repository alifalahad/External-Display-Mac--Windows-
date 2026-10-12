#pragma once
// =============================================================================
// CrashHandler.h — Last-chance crash reporting
// =============================================================================
// On an unhandled SEH exception (access violation, ...) or std::terminate
// (uncaught C++ exception on a worker thread) this writes, next to the .exe:
//   receiver-crash.txt  — exception code + symbolized stack trace
//   receiver-crash.dmp  — minidump (open in Visual Studio / WinDbg with the .pdb)
// and prints the stack to the console before the process exits.
// =============================================================================

void InstallCrashHandler();
