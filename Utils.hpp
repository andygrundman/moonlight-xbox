#pragma once
#include "pch.h"

namespace moonlight_xbox_dx {
	namespace Utils {
		extern std::vector<std::wstring> logLines;
		extern bool showLogs;
		extern bool showStats;
		extern std::mutex logMutex;

		Platform::String^ StringPrintf(const char* fmt, ...);

		void Log(const char* msg);
		void Log(const std::string_view& msg);
		void Logf(const char* msg, ...);

		// Mirror every Log() line to a file (flushed per line) until
		// EndLogMirror. For capturing diagnostics that must survive without a
		// debugger attached - the file can be pulled from LocalState via
		// Device Portal. Keep the mirrored window short: the per-line flush is
		// console storage I/O and would stall a streaming session.
		void BeginLogMirror(const char* path);
		void EndLogMirror();

		std::vector<std::wstring> GetLogLines();
		Platform::String^ StringFromChars(const char* chars);
		Platform::String^ StringFromStdString(std::string st);
		std::string PlatformStringToStdString(Platform::String^ input);
		std::string WideToNarrowString(const std::wstring_view& str);
		std::wstring NarrowToWideString(const std::string_view& str);	}
}
