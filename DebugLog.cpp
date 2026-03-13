#include "pch.h"
#include "DebugLog.h"
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <processthreadsapi.h>
#include <Psapi.h>
#include <ShlObj_core.h>
#include <sstream>
#include <string>
#include <Windows.h>

std::string DebugLog::logFilePath;
std::ofstream DebugLog::logFile;
std::string DebugLog::processName;
DWORD DebugLog::processID;
bool DebugLog::initialized = false;

void DebugLog::initialize() {
	if (!initialized) {
		processID = GetCurrentProcessId();
		
		char procName[MAX_PATH] = "<unknown>";
		HMODULE hMod;
		DWORD cbNeeded;
		if (EnumProcessModules(GetCurrentProcess(), &hMod, sizeof(hMod), &cbNeeded)) {
			GetModuleBaseNameA(GetCurrentProcess(), hMod, procName, sizeof(procName));
		}
		processName = procName;

		char path[MAX_PATH];
		bool pathFound = false;

		// Try environment variable first (more robust in services)
		if (GetEnvironmentVariableA("ProgramData", path, MAX_PATH) > 0) {
			pathFound = true;
		}
		else if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, path))) {
			pathFound = true;
		}

		if (pathFound) {
			std::string baseDir = std::string(path) + "\\AgileMark";
			CreateDirectoryA(baseDir.c_str(), NULL);
			
			logFilePath = baseDir + "\\webcam_dll.txt";

			logFile.open(logFilePath, std::ios::out | std::ios::app);
			if (!logFile.is_open()) {
				// Fallback to a simple name in C:\ if possible or just use OutputDebugString
				OutputDebugStringA("[WebcamDLL] Failed to open log file in ProgramData.");
			}
		}
		
		std::string startMsg = "[WebcamDLL] Initialized in " + processName + " (PID: " + std::to_string(processID) + ")";
		OutputDebugStringA(startMsg.c_str());
		
		initialized = true;
	}
}

void DebugLog::log(const std::string& message) {
	if (!initialized) {
		initialize();
	}

	auto now = std::chrono::system_clock::now();
	auto in_time_t = std::chrono::system_clock::to_time_t(now);
	struct tm timeinfo;
	localtime_s(&timeinfo, &in_time_t);
	std::stringstream ss;
	ss << std::put_time(&timeinfo, "%Y-%m-%d %X");

	std::string formatted = "[" + ss.str() + "] [" + processName + " " + std::to_string(processID) + "] " + message;
	
	// Always output to debugger (can be seen with DebugView)
	OutputDebugStringA(formatted.c_str());

	if (logFile.is_open()) {
		logFile << formatted << std::endl;
		logFile.flush(); // Force write to disk
	}
}
