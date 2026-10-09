// Copyright 2026. Distributed under the project's GPLv3 license.
#ifndef LLMBRIDGE_H
#define LLMBRIDGE_H

#include "LLMPROTOCOL.H"

namespace LLMBridge {
void Set_Local_Game(bool allowed);
bool Can_Control(int house);
void Reset();
// Invoke outside DllMain, before unloading the DLL.
void Shutdown();
void New_Match(unsigned int & low, unsigned int & high);
void Object_Created(unsigned int target);
unsigned int Object_Generation(unsigned int target);
// Wall time is independent of the simulation speed; unsigned subtraction wraps safely.
unsigned int Milliseconds();
bool Publish(std::string const & snapshot);
// A new malformed packet is consumed and returned with invalid=true.
bool Receive(LLM::Plan & plan, bool & invalid);
}

#endif
