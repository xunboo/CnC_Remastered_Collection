// Included only by LLMBRIDGE.CPP. Bootstrap work never reads engine objects.
// This state is deliberately not destroyed under the Windows DLL loader lock.
#include <process.h>
#include <cwchar>

namespace AutoBridge {
struct Settings {
    bool Enabled, AutoStart, Mock;
    int House;
    char Channel[64];
    Settings() : Enabled(false), AutoStart(false), Mock(false), House(-1) {
        std::snprintf(Channel, sizeof(Channel), "%s", "v1");
    }
};
struct Runtime {
    CRITICAL_SECTION Lock;
    HANDLE Stop, Thread, Child, ChildStop;
    volatile LONG LocalGame;
    Settings Config;
    std::wstring Directory, Ini;
    DWORD NextStart;
    unsigned int Revision;
    char const * Event;
    DWORD Error, Pid;
    Runtime() : Stop(NULL), Thread(NULL), Child(NULL), ChildStop(NULL), LocalGame(0),
        NextStart(0), Revision(0), Event("llm_bridge_config"), Error(0), Pid(0) {
        InitializeCriticalSection(&Lock);
    }
    void Report(char const * event, DWORD error = 0, DWORD pid = 0) {
        EnterCriticalSection(&Lock);
        Event = event; Error = error; Pid = pid; ++Revision;
        LeaveCriticalSection(&Lock);
    }
};
Runtime * State = NULL;
unsigned int Reported = 0;

bool Valid_Channel(char const * channel) {
    size_t size = std::strlen(channel);
    if (!size || size > 63) return false;
    for (size_t i = 0; i < size; ++i) {
        char c = channel[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')
            && !(c >= '0' && c <= '9') && c != '-' && c != '_') return false;
    }
    return true;
}
// A legacy explicit 0 still disables control. Explicit 1 works with a manually
// started bridge even when the portable INI/EXE has not been installed.
void Environment(Settings & settings) {
    char enabled[8] = {}, channel[64] = {};
    DWORD size = GetEnvironmentVariableA("AIBOOST_LLM", enabled, sizeof(enabled));
    if (size) settings.Enabled = size == 1 && enabled[0] == '1';
    size = GetEnvironmentVariableA("AIBOOST_LLM_CHANNEL", channel, sizeof(channel));
    if (size) {
        if (size >= sizeof(channel) || !Valid_Channel(channel)) settings.Enabled = false;
        else std::snprintf(settings.Channel, sizeof(settings.Channel), "%s", channel);
    }
}
bool Flag(std::wstring const & path, wchar_t const * key, bool fallback, bool & valid) {
    wchar_t value[16] = {};
    GetPrivateProfileStringW(L"bridge", key, fallback ? L"true" : L"false", value, 16, path.c_str());
    if (!_wcsicmp(value, L"true") || !_wcsicmp(value, L"yes") || !_wcsicmp(value, L"on") || !std::wcscmp(value, L"1")) return true;
    if (!_wcsicmp(value, L"false") || !_wcsicmp(value, L"no") || !_wcsicmp(value, L"off") || !std::wcscmp(value, L"0")) return false;
    valid = false; return false;
}
Settings Read_Config(Runtime & state, bool & valid) {
    Settings result;
    valid = true;
    DWORD attributes = GetFileAttributesW(state.Ini.c_str());
    bool exists = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    if (exists) {
        result.Enabled = Flag(state.Ini, L"enabled", true, valid);
        result.AutoStart = Flag(state.Ini, L"auto_start", true, valid);
        wchar_t value[80] = {};
        GetPrivateProfileStringW(L"bridge", L"channel", L"v1", value, 80, state.Ini.c_str());
        size_t size = std::wcslen(value);
        if (size >= sizeof(result.Channel)) valid = false;
        else {
            for (size_t i = 0; i <= size; ++i) {
                if (value[i] > 127) valid = false;
                result.Channel[i] = static_cast<char>(value[i]);
            }
            if (!Valid_Channel(result.Channel)) valid = false;
        }
        GetPrivateProfileStringW(L"bridge", L"house", L"-1", value, 80, state.Ini.c_str());
        wchar_t * end = NULL;
        long house = std::wcstol(value, &end, 10);
        if (end == value || *end || house < -1 || house > 255) valid = false;
        else result.House = static_cast<int>(house);
        GetPrivateProfileStringW(L"bridge", L"mode", L"real", value, 80, state.Ini.c_str());
        result.Mock = !_wcsicmp(value, L"mock");
        if (!result.Mock && _wcsicmp(value, L"real")) valid = false;
    }
    Environment(result);
    if (!valid) { result.Enabled = false; result.AutoStart = false; }
    return result;
}
bool Healthy(char const * channel) {
    char name[96];
    std::snprintf(name, sizeof(name), "Local\\AIBoostLLM-%s", channel);
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!mapping) return false;
    unsigned char const * data = static_cast<unsigned char const *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, LLM::MappingBytes));
    bool healthy = false;
    if (data) {
        // Aligned 32-bit header fields are atomic across the 32/64-bit processes.
        volatile LONG const * fields = reinterpret_cast<volatile LONG const *>(data);
        healthy = fields[0] == LLM::MappingMagic && fields[1] == LLM::MappingVersion
            && static_cast<DWORD>(GetTickCount() - static_cast<DWORD>(fields[7])) <= 5000;
        UnmapViewOfFile(data);
    }
    CloseHandle(mapping);
    return healthy;
}
std::wstring Quote(std::wstring const & value) {
    // All generated arguments are file paths without trailing backslashes,
    // validated channel names or numbers. No shell is involved.
    return L"\"" + value + L"\"";
}
void Start_Child(Runtime & state, Settings const & settings) {
    DWORD now = GetTickCount();
    if (state.NextStart && static_cast<LONG>(now - state.NextStart) < 0) return;
    state.NextStart = now + 30000;
    std::wstring executable = state.Directory + L"\\LLMBridge.exe";
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        state.Report("llm_bridge_executable_missing", ERROR_FILE_NOT_FOUND); return;
    }
    wchar_t suffix[100];
    std::swprintf(suffix, 100, L"Local\\AIBoostLLM-stop-%lu", GetCurrentProcessId());
    state.ChildStop = CreateEventW(NULL, TRUE, FALSE, suffix);
    if (!state.ChildStop) { state.Report("llm_bridge_start_failed", GetLastError()); return; }
    ResetEvent(state.ChildStop);
    std::wstring log_directory = state.Directory + L"\\log";
    CreateDirectoryW(log_directory.c_str(), NULL);
    SYSTEMTIME time; GetLocalTime(&time);
    wchar_t log_name[128];
    std::swprintf(log_name, 128, L"\\bridge_%04u-%02u-%02u_%02u-%02u-%02u-%03u_p%lu.log",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentProcessId());
    wchar_t numbers[128];
    std::swprintf(numbers, 128, L" --house %d --parent-pid %lu", settings.House, GetCurrentProcessId());
    std::wstring channel(settings.Channel, settings.Channel + std::strlen(settings.Channel));
    std::wstring command = Quote(executable) + L" --config " + Quote(state.Ini)
        + L" --channel " + channel + numbers + L" --stop-event " + Quote(suffix)
        + L" --runtime-log " + Quote(log_directory + log_name);
    if (settings.Mock) command += L" --mock";
    STARTUPINFOW startup = {}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable.c_str(), &command[0], NULL, NULL, FALSE, CREATE_NO_WINDOW,
            NULL, state.Directory.c_str(), &startup, &process)) {
        DWORD error = GetLastError(); CloseHandle(state.ChildStop); state.ChildStop = NULL;
        state.Report("llm_bridge_start_failed", error); return;
    }
    CloseHandle(process.hThread); state.Child = process.hProcess;
    state.Report("llm_bridge_started", 0, process.dwProcessId);
}
unsigned int __stdcall Worker(void * context) {
    Runtime & state = *static_cast<Runtime *>(context);
    Settings previous;
    bool loaded = false;
    while (WaitForSingleObject(state.Stop, 0) == WAIT_TIMEOUT) {
        bool valid = true;
        Settings settings = Read_Config(state, valid);
        bool changed = !loaded || settings.Enabled != previous.Enabled || settings.AutoStart != previous.AutoStart
            || settings.Mock != previous.Mock || settings.House != previous.House
            || std::strcmp(settings.Channel, previous.Channel);
        EnterCriticalSection(&state.Lock); state.Config = settings; LeaveCriticalSection(&state.Lock);
        if (changed) {
            if (state.ChildStop) SetEvent(state.ChildStop);
            state.NextStart = 0;
            state.Report(valid ? "llm_bridge_config" : "llm_bridge_config_invalid");
        }
        previous = settings;
        loaded = true;
        if (state.Child && WaitForSingleObject(state.Child, 0) == WAIT_OBJECT_0) {
            DWORD code = 0; GetExitCodeProcess(state.Child, &code);
            CloseHandle(state.Child); state.Child = NULL;
            CloseHandle(state.ChildStop); state.ChildStop = NULL;
            state.Report("llm_bridge_exited", code);
        }
        bool local = InterlockedCompareExchange(&state.LocalGame, 0, 0) != 0;
        if (!local) {
            if (state.ChildStop) SetEvent(state.ChildStop);
            state.NextStart = 0;
        }
        if (local && settings.Enabled && settings.AutoStart && !state.Child && !Healthy(settings.Channel)) {
            Start_Child(state, settings);
        }
        if (WaitForSingleObject(state.Stop, 1000) != WAIT_TIMEOUT) break;
    }
    if (state.ChildStop) { SetEvent(state.ChildStop); CloseHandle(state.ChildStop); state.ChildStop = NULL; }
    if (state.Child) { CloseHandle(state.Child); state.Child = NULL; }
    return 0;
}
void Ensure_Started(bool local) {
    if (!State && local) {
        State = new Runtime;
        HMODULE module = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Ensure_Started), &module);
        wchar_t path[32768];
        DWORD length = GetModuleFileNameW(module, path, 32768);
        if (!length || length >= 32768) { State->Report("llm_bridge_start_failed", ERROR_BAD_PATHNAME); return; }
        State->Directory.assign(path, length);
        size_t slash = State->Directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;
        State->Directory.resize(slash); State->Ini = State->Directory + L"\\llm.ini";
        State->Stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!State->Stop) { State->Report("llm_bridge_start_failed", GetLastError()); return; }
        InterlockedExchange(&State->LocalGame, 1);
        State->Thread = reinterpret_cast<HANDLE>(_beginthreadex(NULL, 0, Worker, State, 0, NULL));
        if (!State->Thread) State->Report("llm_bridge_start_failed", ERROR_NOT_ENOUGH_MEMORY);
    }
    if (State) InterlockedExchange(&State->LocalGame, local ? 1 : 0);
}
Settings Poll() {
    Settings settings;
    if (State) {
        EnterCriticalSection(&State->Lock);
        settings = State->Config;
        unsigned int revision = State->Revision;
        char const * event = State->Event;
        DWORD error = State->Error, pid = State->Pid;
        LeaveCriticalSection(&State->Lock);
        if (revision != Reported && !AILog::Path().empty()) {
            AILog::Record(AILog::Current_Frame(), -1, event, AILog::Fields()
                .Boolean("enabled", settings.Enabled).Boolean("auto_start", settings.AutoStart)
                .Boolean("mock", settings.Mock).Text("channel", settings.Channel)
                .Number("house", settings.House).Number("windows_error_or_exit_code", error).Number("bridge_pid", pid));
            Reported = revision;
        }
    }
    Environment(settings);
    return settings;
}
void Shutdown() {
    if (!State) return;
    if (State->Stop) SetEvent(State->Stop);
    // Called explicitly by DLL_Shutdown, never by DllMain/global destructors.
    if (State->Thread) { WaitForSingleObject(State->Thread, INFINITE); CloseHandle(State->Thread); }
    if (State->Stop) CloseHandle(State->Stop);
    DeleteCriticalSection(&State->Lock);
    delete State; State = NULL; Reported = 0;
}
}
