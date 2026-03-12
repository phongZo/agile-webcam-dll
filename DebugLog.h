#ifndef LOGGER_H
#define LOGGER_H

#include <fstream>
#include <string>
#include <windows.h>

class DebugLog {
public:
	static void log(const std::string& message);
	static void initialize();

private:
	static std::string getRoamingFolderPath();
	static std::string getProcessName();
	static DWORD getProcessID();
	static std::string getCurrentTimestamp();

	static std::string logFilePath;
	static std::ofstream logFile;
	static std::string processName;
	static DWORD processID;

	static bool initialized;
};

#endif // LOGGER_H
