// Copyright 2026. Distributed under the project's GPLv3 license.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#include <cstdio>
#include <cwchar>
#include <deque>
#include "AILOG.H"

namespace {
struct Logger {
    CRITICAL_SECTION Lock;
    HANDLE Wake, Room, Thread, File;
    std::deque<std::string> Queue;
    size_t Bytes;
    bool Closing, Prepared, Attempted;
    volatile LONG Active;
    unsigned int Low, High;
    int Frame;
    unsigned long long Sequence;
    std::string Filename, Reason, Previous;
    Logger() : Thread(NULL), File(INVALID_HANDLE_VALUE), Bytes(0), Closing(false), Prepared(false),
        Attempted(false), Active(0), Low(0), High(0), Frame(0), Sequence(0) {
        InitializeCriticalSection(&Lock);
        Wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        Room = CreateEventW(NULL, TRUE, TRUE, NULL);
    }
};
// Explicit Shutdown/Prepare drains the worker outside the DLL loader lock.
Logger & Log() { static Logger * state = new Logger; return *state; }

void Failure() {
    OutputDebugStringA("AI-Boost: match log could not be written; check the DLL's log directory and disk space.\n");
    InterlockedExchange(&Log().Active, 0);
}
std::string Utf8(std::wstring const & text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), NULL, 0, NULL, NULL);
    std::string result(size, '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], size, NULL, NULL);
    return result;
}
std::string Timestamp() {
    SYSTEMTIME now; GetSystemTime(&now);
    char text[40];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth,
        now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return text;
}
bool Append(std::string const & text) {
    Logger & log = Log();
    OVERLAPPED overlap = {};
    if (!LockFileEx(log.File, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlap)) return false;
    DWORD written = 0;
    OVERLAPPED append = {}; append.Offset = 0xffffffff; append.OffsetHigh = 0xffffffff;
    bool ok = WriteFile(log.File, text.data(), static_cast<DWORD>(text.size()), &written, &append) != FALSE
        && written == text.size();
    UnlockFileEx(log.File, 0, 1, 0, &overlap);
    return ok;
}
unsigned int __stdcall Writer(void *) {
    Logger & log = Log();
    DWORD last_flush = GetTickCount();
    for (;;) {
        WaitForSingleObject(log.Wake, 1000);
        for (;;) {
            std::string batch;
            EnterCriticalSection(&log.Lock);
            while (!log.Queue.empty() && batch.size() < 262144) {
                log.Bytes -= log.Queue.front().size();
                batch += log.Queue.front(); log.Queue.pop_front();
            }
            SetEvent(log.Room);
            bool done = log.Closing && log.Queue.empty();
            LeaveCriticalSection(&log.Lock);
            if (!batch.empty() && !Append(batch)) Failure();
            if (done) { FlushFileBuffers(log.File); return 0; }
            if (batch.empty()) break;
        }
        DWORD now = GetTickCount();
        if (now - last_flush >= 1000) { if (!FlushFileBuffers(log.File)) Failure(); last_flush = now; }
    }
}
}

std::string AILog::Quote(char const * text) {
    if (text && *text && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0)) {
        int count = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
        if (count > 0) {
            std::wstring wide(count, L'\0');
            MultiByteToWideChar(CP_ACP, 0, text, -1, &wide[0], count);
            wide.resize(count - 1);
            return Quote(Utf8(wide).c_str());
        }
    }
    std::string result = "\"";
    if (text) for (unsigned char const * c = reinterpret_cast<unsigned char const *>(text); *c; ++c) {
        if (*c == '"' || *c == '\\') { result += '\\'; result += static_cast<char>(*c); }
        else if (*c < 32) { char escape[7]; std::snprintf(escape, sizeof(escape), "\\u%04x", *c); result += escape; }
        else result += static_cast<char>(*c);
    }
    return result + '"';
}
void AILog::Fields::Key(char const * name) { if (!Body.empty()) Body += ','; Body += Quote(name) + ':'; }
AILog::Fields & AILog::Fields::Number(char const * name, long long value) { Key(name); Body += std::to_string(value); return *this; }
AILog::Fields & AILog::Fields::Text(char const * name, char const * value) { Key(name); Body += Quote(value); return *this; }
AILog::Fields & AILog::Fields::Boolean(char const * name, bool value) { Key(name); Body += value ? "true" : "false"; return *this; }
AILog::Fields & AILog::Fields::Raw(char const * name, std::string const & json) { Key(name); Body += json; return *this; }
bool AILog::Enabled() { return InterlockedCompareExchange(&Log().Active, 0, 0) != 0; }
int AILog::Current_Frame() { return Log().Frame; }
std::string AILog::Path() { return Enabled() ? Log().Filename : std::string(); }

void AILog::Prepare(unsigned int low, unsigned int high, int frame, char const * reason) {
    Logger & log = Log();
    log.Previous = log.Filename;
    End(log.Frame, reason);
    log.Frame = frame; log.Low = low; log.High = high; log.Reason = reason;
    log.Prepared = true; log.Attempted = false; log.Sequence = 0; log.Filename.clear();
}
void AILog::Start_Reason(char const * reason) { Log().Reason = reason; }
void AILog::Begin(int frame, Fields const & context) {
    Logger & log = Log();
    log.Frame = frame;
    if (!log.Prepared || log.Attempted) return;
    log.Attempted = true;
    char setting[8] = {};
    if (GetEnvironmentVariableA("AIBOOST_LOG", setting, sizeof(setting)) == 1 && setting[0] == '0') return;
    HMODULE module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&AILog::Begin), &module)) { Failure(); return; }
    wchar_t path[32768]; DWORD size = GetModuleFileNameW(module, path, 32768);
    if (!size || size >= 32768) { Failure(); return; }
    std::wstring directory(path, size);
    size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) { Failure(); return; }
    directory = directory.substr(0, slash) + L"\\log";
    if (!CreateDirectoryW(directory.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { Failure(); return; }
    SYSTEMTIME now; GetLocalTime(&now);
    wchar_t name[160];
    std::swprintf(name, 160, L"%04u-%02u-%02u_%02u-%02u-%02u-%03u_p%lu_%08x%08x.jsonl",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId(), log.High, log.Low);
    std::wstring filename = directory + L"\\" + name;
    log.File = CreateFileW(filename.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log.File == INVALID_HANDLE_VALUE || !log.Wake || !log.Room) {
        if (log.File != INVALID_HANDLE_VALUE) CloseHandle(log.File);
        log.File = INVALID_HANDLE_VALUE; Failure(); return;
    }
    log.Filename = Utf8(filename); log.Closing = false;
    log.Thread = reinterpret_cast<HANDLE>(_beginthreadex(NULL, 0, Writer, NULL, 0, NULL));
    if (!log.Thread) { CloseHandle(log.File); log.File = INVALID_HANDLE_VALUE; Failure(); return; }
    SetThreadPriority(log.Thread, THREAD_PRIORITY_BELOW_NORMAL);
    InterlockedExchange(&log.Active, 1);
    Fields data = context; data.Text("reason", log.Reason.c_str()).Text("dll_path", Utf8(std::wstring(path, size)).c_str())
        .Text("previous_log", log.Previous.c_str());
    Record(frame, -1, "match_start", data);
}
void AILog::Record(int frame, int house, char const * event, std::string const & data) {
    if (!Enabled()) return;
    Logger & log = Log();
    char match[17]; std::snprintf(match, sizeof(match), "%08x%08x", log.High, log.Low);
    Fields row;
    row.Number("log_version", 1).Text("timestamp", Timestamp().c_str()).Text("source", "dll")
        .Number("pid", GetCurrentProcessId()).Number("seq", ++log.Sequence).Text("match_id", match)
        .Number("sim_frame", frame < 0 ? log.Frame : frame).Number("house_id", house).Text("event", event).Raw("data", data);
    std::string text = row.Json() + '\n';
    for (;;) {
        EnterCriticalSection(&log.Lock);
        if (log.Bytes < 16 * 1024 * 1024 || log.Closing || !Enabled()) break;
        ResetEvent(log.Room); LeaveCriticalSection(&log.Lock);
        SetEvent(log.Wake); WaitForSingleObject(log.Room, INFINITE);
    }
    if (!log.Closing && Enabled()) { log.Bytes += text.size(); log.Queue.push_back(text); }
    LeaveCriticalSection(&log.Lock); SetEvent(log.Wake);
}
void AILog::End(int frame, char const * reason) {
    Logger & log = Log();
    if (!log.Thread) return;
    Record(frame, -1, "match_end", Fields().Text("reason", reason));
    EnterCriticalSection(&log.Lock); log.Closing = true; LeaveCriticalSection(&log.Lock);
    SetEvent(log.Wake); WaitForSingleObject(log.Thread, INFINITE);
    CloseHandle(log.Thread); CloseHandle(log.File);
    log.Thread = NULL; log.File = INVALID_HANDLE_VALUE; InterlockedExchange(&log.Active, 0);
}
