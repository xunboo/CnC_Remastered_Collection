// Exercise the actual Windows DLL transport from a separate Python process.
#include "../REDALERT/LLMBRIDGE.H"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static void Require(bool passed, char const * message)
{
    if (!passed) { std::cerr << message << std::endl; std::exit(1); }
}
int main(int argc, char ** argv)
{
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 2;
    std::string snapshot((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    LLMBridge::Set_Local_Game(false);
    Require(!LLMBridge::Can_Control(4), "online/campaign gate must reject control");
    LLMBridge::Set_Local_Game(true);
    SetEnvironmentVariableA("AIBOOST_LLM", "0");
    Require(!LLMBridge::Can_Control(4), "the environment opt-in is required");
    SetEnvironmentVariableA("AIBOOST_LLM", "1");
    Require(LLMBridge::Can_Control(4) && !LLMBridge::Can_Control(3), "requested AI house is respected");
    Require(!LLMBridge::Publish("") && !LLMBridge::Publish(std::string(LLM::SnapshotBytes + 1, 'x')),
        "snapshot size is bounded");
    Require(LLMBridge::Publish(snapshot), "native snapshot publication failed");
    unsigned int before = LLMBridge::Object_Generation(12);
    LLMBridge::Object_Created(12);
    Require(LLMBridge::Object_Generation(12) != before, "instance construction must invalidate old references");
    std::cout << "ready" << std::endl;
    std::string command;
    while (std::cin >> command) {
        if (command == "quit") break;
        if (command == "allowed") {
            std::cout << (LLMBridge::Can_Control(4) ? "allowed" : "blocked") << std::endl;
        } else if (command == "reset") {
            LLMBridge::Reset();
            std::cout << "reset" << std::endl;
        } else if (command == "offline") {
            LLMBridge::Set_Local_Game(false);
            std::cout << "offline" << std::endl;
        } else if (command == "receive") {
            LLM::Plan plan = {};
            bool invalid = false;
            if (!LLMBridge::Receive(plan, invalid)) std::cout << "none" << std::endl;
            else if (invalid) std::cout << "invalid" << std::endl;
            else std::cout << plan.SnapshotSeq << ':' << plan.Count << ':' << plan.Orders[0].Target
                << ':' << plan.Orders[0].Generation << std::endl;
        } else if (command == "publish") {
            std::string updated;
            std::getline(std::cin >> std::ws, updated);
            std::cout << (LLMBridge::Publish(updated) ? "published" : "failed") << std::endl;
        } else return 2;
    }
    LLMBridge::Reset();
    LLMBridge::Shutdown();
    return 0;
}
