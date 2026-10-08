// Actual DLL transport/bootstrap, in a process with no LLM environment flags.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include "../REDALERT/LLMBRIDGE.H"
#include "../REDALERT/AILOG.H"

int main()
{
    unsigned int low = 0, high = 0;
    LLMBridge::New_Match(low, high);
    AILog::Prepare(low, high, 0, "autostart_fixture");
    AILog::Begin(0, AILog::Fields());
    char match[24]; std::snprintf(match, sizeof(match), "%08x%08x", high, low);
    std::cout << GetCurrentProcessId() << std::endl << AILog::Path() << std::endl << match << std::endl;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "quit") break;
        if (command == "local") {
            LLMBridge::Set_Local_Game(true); std::cout << "local" << std::endl;
        } else if (command == "offline") {
            LLMBridge::Set_Local_Game(false); std::cout << "offline" << std::endl;
        } else if (command == "allowed") {
            std::cout << (LLMBridge::Can_Control(4) ? "allowed" : "blocked") << std::endl;
        } else if (command.compare(0, 8, "publish ") == 0) {
            std::ifstream file(command.substr(8).c_str(), std::ios::binary);
            std::string snapshot((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::cout << (LLMBridge::Publish(snapshot) ? "published" : "failed") << std::endl;
        } else if (command == "receive") {
            LLM::Plan plan = {}; bool invalid = false;
            if (!LLMBridge::Receive(plan, invalid)) std::cout << "none" << std::endl;
            else if (invalid) std::cout << "invalid" << std::endl;
            else std::cout << plan.SnapshotSeq << ':' << plan.Count << ':' << plan.Orders[0].ActionID << std::endl;
        } else return 2;
    }
    LLMBridge::Shutdown();
    AILog::End(9999, "fixture_shutdown");
    return 0;
}
