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
		if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
			// Change log name for webcam dll
			logFilePath = std::string(path) + "\\AgileMark\\webcam_dll.txt";

			logFile.open(logFilePath, std::ios::out | std::ios::app);
			if (!logFile.is_open()) {
				logFile.open(logFilePath, std::ios::out);
				if (logFile.is_open()) {
					logFile.close();
					logFile.open(logFilePath, std::ios::out | std::ios::app);
				}
			}
		}
		initialized = true;
	}
}

void DebugLog::log(const std::string& message) {
	if (!initialized) {
		initialize();
	}

	if (logFile.is_open()) {
		auto now = std::chrono::system_clock::now();
		auto in_time_t = std::chrono::system_clock::to_time_t(now);
		struct tm timeinfo;
		localtime_s(&timeinfo, &in_time_t);
		std::stringstream ss;
		ss << std::put_time(&timeinfo, "%Y-%m-%d %X");

		logFile << "[" << ss.str() << "] "
			<< "[" << processName << " " << processID << "] "
			<< message << std::endl;
	}
}
