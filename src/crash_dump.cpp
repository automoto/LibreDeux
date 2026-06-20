#include "crash_dump.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace {

void WriteCrashMarker(const wchar_t* path, EXCEPTION_POINTERS* exception_pointers) {
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD bytes_written = 0;
  char buffer[1024] = {};
  auto* record = exception_pointers ? exception_pointers->ExceptionRecord : nullptr;
  auto module_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  auto exception_address = reinterpret_cast<uintptr_t>(record ? record->ExceptionAddress : nullptr);
  auto exception_rva = module_base && exception_address >= module_base ? exception_address - module_base : 0;
  auto fault_operation = record && record->NumberParameters > 0 ? record->ExceptionInformation[0] : 0;
  auto fault_address = record && record->NumberParameters > 1 ? record->ExceptionInformation[1] : 0;
  std::snprintf(buffer, sizeof(buffer),
                "exception_code=0x%08lX\r\n"
                "exception_address=%p\r\n"
                "module_base=0x%p\r\n"
                "exception_rva=0x%llX\r\n"
                "exception_parameters=%lu\r\n"
                "fault_operation=%llu\r\n"
                "fault_address=0x%llX\r\n"
                "process_id=%lu\r\n"
                "thread_id=%lu\r\n",
                record ? record->ExceptionCode : 0,
                record ? record->ExceptionAddress : nullptr,
                reinterpret_cast<void*>(module_base),
                static_cast<unsigned long long>(exception_rva),
                record ? record->NumberParameters : 0,
                static_cast<unsigned long long>(fault_operation),
                static_cast<unsigned long long>(fault_address),
                GetCurrentProcessId(),
                GetCurrentThreadId());
  WriteFile(file, buffer, static_cast<DWORD>(std::strlen(buffer)), &bytes_written, nullptr);
  CloseHandle(file);
}

LONG WINAPI AotUnhandledExceptionFilter(EXCEPTION_POINTERS* exception_pointers) {
  CreateDirectoryW(L"artifacts", nullptr);
  CreateDirectoryW(L"artifacts\\dumps", nullptr);

  SYSTEMTIME now = {};
  GetLocalTime(&now);

  wchar_t dump_path[MAX_PATH] = {};
  wchar_t marker_path[MAX_PATH] = {};
  std::swprintf(dump_path, MAX_PATH,
                L"artifacts\\dumps\\aot_crash_%04u%02u%02u-%02u%02u%02u_%lu.dmp",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                GetCurrentProcessId());
  std::swprintf(marker_path, MAX_PATH,
                L"artifacts\\dumps\\aot_crash_%04u%02u%02u-%02u%02u%02u_%lu.txt",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                GetCurrentProcessId());

  HANDLE dump_file = CreateFileW(dump_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (dump_file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION exception_info = {};
    exception_info.ThreadId = GetCurrentThreadId();
    exception_info.ExceptionPointers = exception_pointers;
    exception_info.ClientPointers = FALSE;

    auto dump_type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules);

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump_file, dump_type,
                      &exception_info, nullptr, nullptr);
    CloseHandle(dump_file);
  }

  WriteCrashMarker(marker_path, exception_pointers);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void AotInstallCrashDumpHandler() {
  SetErrorMode(SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);
  SetUnhandledExceptionFilter(AotUnhandledExceptionFilter);
}

#else

void AotInstallCrashDumpHandler() {}

#endif
