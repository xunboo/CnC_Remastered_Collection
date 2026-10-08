// Copyright 2026. Distributed under the project's GPLv3 license.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include "LLMBRIDGE.H"
#include "AILOG.H"
#include "LLMAUTO.INC"

namespace {
struct Connection {
    HANDLE Mapping;
    unsigned char * Memory;
    DWORD NextConnect;
    LONG LastCommand;
    int SelectedHouse;
    bool LocalGame;
    bool ConnectedReported;
    int ConfiguredHouse;
    char ActiveChannel[64];
    Connection() : Mapping(NULL), Memory(NULL), NextConnect(0), LastCommand(0),
        SelectedHouse(-1), LocalGame(false), ConnectedReported(false), ConfiguredHouse(-1) { ActiveChannel[0] = 0; }
    ~Connection() { Close(); }
    void Close() {
        if (Memory) {
            InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(Memory + 32), 0,
                static_cast<LONG>(GetCurrentProcessId()));
            UnmapViewOfFile(Memory);
        }
        if (Mapping) CloseHandle(Mapping);
        Mapping = NULL; Memory = NULL; LastCommand = 0; SelectedHouse = -1;
        ActiveChannel[0] = 0;
        ConnectedReported = false;
    }
    LONG Read(int offset) const {
        return InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(Memory + offset), 0, 0);
    }
    bool Ready() {
        if (!LocalGame) return false;
        AutoBridge::Settings settings = AutoBridge::Poll();
        ConfiguredHouse = settings.House;
        if (!settings.Enabled) { Close(); return false; }
        if (Memory && std::strcmp(ActiveChannel, settings.Channel)) { Close(); NextConnect = 0; }
        DWORD now = GetTickCount();
        if (!Memory) {
            if (NextConnect && static_cast<LONG>(now - NextConnect) < 0) return false;
            NextConnect = now + 1000;
            char channel[96];
            std::snprintf(channel, sizeof(channel), "Local\\AIBoostLLM-%s", settings.Channel);
            Mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, channel);
            if (!Mapping) return false;
            Memory = static_cast<unsigned char *>(MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, LLM::MappingBytes));
            if (!Memory) { Close(); return false; }
            std::snprintf(ActiveChannel, sizeof(ActiveChannel), "%s", settings.Channel);
        }
        if (Read(0) != LLM::MappingMagic || Read(4) != LLM::MappingVersion) { Close(); return false; }
        if (static_cast<DWORD>(now - static_cast<DWORD>(Read(28))) > 5000) return false;
        LONG pid = static_cast<LONG>(GetCurrentProcessId());
        LONG owner = InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(Memory + 32), pid, 0);
        if (owner != 0 && owner != pid) {
            // A crashed game cannot release its claim. Recover only when
            // Windows confirms the previous process has exited.
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(owner));
            bool exited = process ? WaitForSingleObject(process, 0) == WAIT_OBJECT_0
                : GetLastError() == ERROR_INVALID_PARAMETER;
            if (process) CloseHandle(process);
            if (exited) owner = InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(Memory + 32), pid, owner);
            else return false;
            if (owner != pid && Read(32) != pid) return false;
        }
        if (!ConnectedReported) {
            if (!AILog::Path().empty()) {
                AILog::Record(AILog::Current_Frame(), -1, "llm_bridge_connected",
                    AILog::Fields().Text("channel", settings.Channel));
                ConnectedReported = true;
            }
        }
        return true;
    }
} Channel;
LLM::Identities Identities;
unsigned int MatchCounter = 0;
unsigned int CurrentMatchLow = 0, CurrentMatchHigh = 0;
}

void LLMBridge::Set_Local_Game(bool allowed)
{
    Channel.LocalGame = allowed;
    AutoBridge::Ensure_Started(allowed);
    if (!allowed) Channel.Close();
}
bool LLMBridge::Can_Control(int house)
{
    if (!Channel.Ready()) return false;
    if (Channel.ConfiguredHouse >= 0 && house != Channel.ConfiguredHouse) return false;
    int requested = Channel.Read(24);
    if (requested >= 0) return house == requested;
    if (requested != -1) return false;
    if (Channel.SelectedHouse < 0) Channel.SelectedHouse = house;
    return house == Channel.SelectedHouse;
}
void LLMBridge::Reset()
{
    Channel.Close();
    Channel.NextConnect = 0;
    Identities.Clear();
}
void LLMBridge::Shutdown()
{
    Channel.LocalGame = false;
    Channel.Close();
    AutoBridge::Shutdown();
}
void LLMBridge::New_Match(unsigned int & low, unsigned int & high)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    low = static_cast<unsigned int>(counter.LowPart) ^ ++MatchCounter;
    high = static_cast<unsigned int>(counter.HighPart) ^ GetCurrentProcessId();
    CurrentMatchLow = low; CurrentMatchHigh = high;
}
void LLMBridge::Object_Created(unsigned int target) { Identities.Created(target); }
unsigned int LLMBridge::Object_Generation(unsigned int target) { return Identities.Generation(target); }

bool LLMBridge::Publish(std::string const & snapshot)
{
    if (!Channel.Ready() || snapshot.empty() || snapshot.size() > LLM::SnapshotBytes) return false;
    std::string path = AILog::Path();
    LONG path_sequence = Channel.Read(36);
    LONG odd_path = static_cast<LONG>((static_cast<unsigned int>(path_sequence) + 1u) | 1u);
    InterlockedExchange(reinterpret_cast<volatile LONG *>(Channel.Memory + 36), odd_path);
    unsigned int path_size = path.size() < LLM::LogPathBytes ? static_cast<unsigned int>(path.size()) : 0;
    if (path_size) std::memcpy(Channel.Memory + LLM::LogPathOffset, path.data(), path_size);
    InterlockedExchange(reinterpret_cast<volatile LONG *>(Channel.Memory + 40), path_size);
    InterlockedExchange(reinterpret_cast<volatile LONG *>(Channel.Memory + 44), CurrentMatchLow);
    InterlockedExchange(reinterpret_cast<volatile LONG *>(Channel.Memory + 48), CurrentMatchHigh);
    InterlockedExchange(reinterpret_cast<volatile LONG *>(Channel.Memory + 36), static_cast<LONG>(static_cast<unsigned int>(odd_path) + 1u));
    volatile LONG * sequence = reinterpret_cast<volatile LONG *>(Channel.Memory + 8);
    LONG previous = Channel.Read(8);
    LONG odd = static_cast<LONG>((static_cast<unsigned int>(previous) + 1u) | 1u);
    InterlockedExchange(sequence, odd);
    std::memcpy(Channel.Memory + LLM::HeaderBytes, snapshot.data(), snapshot.size());
    InterlockedExchange(reinterpret_cast<volatile LONG *>(Channel.Memory + 12), static_cast<LONG>(snapshot.size()));
    InterlockedExchange(sequence, static_cast<LONG>(static_cast<unsigned int>(odd) + 1u));
    return true;
}
bool LLMBridge::Receive(LLM::Plan & plan, bool & invalid)
{
    invalid = false;
    if (!Channel.Ready()) return false;
    LONG sequence = Channel.Read(16);
    if (!sequence || (sequence & 1) || sequence == Channel.LastCommand) return false;
    LONG length = Channel.Read(20);
    unsigned char bytes[LLM::CommandBytes];
    if (length > 0 && length <= LLM::CommandBytes)
        std::memcpy(bytes, Channel.Memory + LLM::HeaderBytes + LLM::SnapshotBytes, length);
    MemoryBarrier();
    if (Channel.Read(16) != sequence) return false;
    Channel.LastCommand = sequence;
    invalid = length <= 0 || length > LLM::CommandBytes
        || !LLM::Decode(bytes, static_cast<unsigned int>(length), plan);
    std::string hex;
    if (length > 0 && length <= LLM::CommandBytes) {
        static char const digits[] = "0123456789abcdef";
        for (int i = 0; i < length; ++i) { hex += digits[bytes[i] >> 4]; hex += digits[bytes[i] & 15]; }
    }
    AILog::Record(AILog::Current_Frame(), invalid ? -1 : plan.House, "llm_command_received",
        AILog::Fields().Number("transport_seq", sequence).Number("byte_count", length)
            .Boolean("wire_valid", !invalid).Text("packet_hex", hex.c_str()));
    return true;
}
