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
HANDLE DebugLog::hLogMutex = NULL;
std::string DebugLog::processName;
DWORD DebugLog::processID;
bool DebugLog::initialized = false;

void DebugLog::initialize() {
	if (!initialized) {
		processID = GetCurrentProcessId();
		
		// Create a Global Mutex to sync logging across all processes (Zoom, Teams, etc.)
		hLogMutex = CreateMutexA(NULL, FALSE, "Global\\AgileMark_WebcamDLL_LogMutex");
		
		char procName[MAX_PATH] = "<unknown>";
		HMODULE hMod;
		DWORD cbNeeded;
		if (EnumProcessModules(GetCurrentProcess(), &hMod, sizeof(hMod), &cbNeeded)) {
			GetModuleBaseNameA(GetCurrentProcess(), hMod, procName, sizeof(procName));
		}
		processName = procName;

		char path[MAX_PATH];
		bool pathFound = false;

		// Use APPDATA for Roaming profile
		if (GetEnvironmentVariableA("APPDATA", path, MAX_PATH) > 0) {
			pathFound = true;
		}
		else if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
			pathFound = true;
		}

		if (pathFound) {
			std::string baseDir = std::string(path) + "\\AgileMark";
			CreateDirectoryA(baseDir.c_str(), NULL);
			logFilePath = baseDir + "\\webcamdll.log";
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

	if (!logFilePath.empty()) {
		// Use Global Mutex to avoid log corruption and allow rotation
		if (hLogMutex) {
			DWORD waitResult = WaitForSingleObject(hLogMutex, 500); 
			if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
				// Open-Write-Close pattern: This allows AgileMark to zip/rotate the file when the mutex is free
				std::ofstream out(logFilePath, std::ios::out | std::ios::app);
				if (out.is_open()) {
					out << formatted << std::endl;
					out.close();
				}
				ReleaseMutex(hLogMutex);
			} else {
				OutputDebugStringA("[WebcamDLL][ERROR] Log Mutex Timeout.");
			}
		}
	}
}
