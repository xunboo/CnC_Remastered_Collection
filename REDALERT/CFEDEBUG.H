#pragma once

#include <cstdio>
#include "FUNCTION.H"

//Prints to steamapps\common\CnCRemastered\log\CnCDLL_Log_16000.txt
template <typename... ARGS>
void CFE_Debug_Printf(const char* const format, const ARGS&... args) {
	const int bufferSize = snprintf(nullptr, 0, format, args...);
	char* printed = (char*)malloc(bufferSize + 1);
	snprintf(printed, bufferSize + 1, format, args...);

	GlyphX_Debug_Print(printed);
	delete printed;
}
