// Exercises the real DLL logger and transport independently of game assets.
#include "../REDALERT/AILOG.H"
#include "../REDALERT/LLMBRIDGE.H"
#include <cstdio>
#include <iostream>
#include <string>

static void BeginMatch(int frame) {
    unsigned int low, high;
    LLMBridge::New_Match(low, high);
    AILog::Prepare(low, high, frame, "fixture_match_or_load");
    AILog::Begin(frame, AILog::Fields().Text("scenario", "controlled_log_fixture"));
    char match[17]; std::snprintf(match, sizeof(match), "%08x%08x", high, low);
    std::cout << AILog::Path() << std::endl << match << std::endl;
}
int main() {
    BeginMatch(0);
    if (!AILog::Enabled()) return 1;
    LLMBridge::Set_Local_Game(true);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") break;
        if (line == "record") {
            for (int i = 0; i < 5000; ++i) AILog::Record(i, 4, "native_fixture_decision",
                AILog::Fields().Number("index", i).Text("quoted", "line\nquote\"slash\\")
                    .Text("utf8", "\xe6\xb5\x8b\xe8\xaf\x95").Text("legacy_non_ascii", "\xe9"));
            std::cout << "recorded" << std::endl;
        } else if (line == "rotate") {
            LLMBridge::Reset(); BeginMatch(0); LLMBridge::Set_Local_Game(true);
        } else if (line.compare(0, 8, "publish ") == 0) {
            AILog::Begin(901, AILog::Fields());
            std::cout << (LLMBridge::Can_Control(4) && LLMBridge::Publish(line.substr(8)) ? "published" : "failed") << std::endl;
        } else return 2;
    }
    LLMBridge::Shutdown();
    AILog::End(9999, "fixture_shutdown");
    LLMBridge::Reset();
    return 0;
}
