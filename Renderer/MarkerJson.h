#pragma once
#ifndef MARKERJSON_H
#define MARKERJSON_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Define the MarkerJson class
class MarkerJson {
public:
	bool DrawingEnabled = true;
	float Opacity = 1;
	std::string TimestampFormat = "ddMMyyyy HH:mm";
	bool GridEnabled = false;
	std::string GridShape = "Circle";
	float GridOpacity = 0.0f;
	int Cols = 0;
	int Rows = 0;
	float CellWidth = 0.0f;
	float CellHeight = 0.0f;
	int GridSpacingX = 0;
	int GridSpacingY = 0;
	int RandomOffsetDistance = 0;
	float MarginPercent = 0.0f;
	std::string GridColor1 = "#000000";
	std::string GridColor2 = "#FFFFFF";
	float GridBlurRadius = 0.0f;
	std::string GridLogoURL = "https://assets.agilemark.io/images/agilemark-logo.png";
	std::string GridLogoHash = "";
	std::string GridSecondaryLogoURL = "https://assets.agilemark.io/images/agilemark-logo.png";
	std::string GridSecondaryLogoHash = "";
	bool GridShowTimestamp = false;
	float TextOpacity = 1.0f;
	bool TextEnabled = true;
	std::string TextFormat = "{MachineName}:{UserName}";
	std::string TextCustomDateTimeFormat = "dd/MM/yyyy";
	bool TextSpacingEnabled = true;
	float TextBlurRadius = 0.0f;
	bool TextAdjustment = false;
	int TextRows = 4;
	int TextCols = 5;
	int TextSize = 30;
	int TextAngle = 45;
	int TextSpacingX = 200;
	int TextSpacingY = 200;
	std::string TextColor1 = "#000000";
	std::string TextColor2 = "#000000";
	float LogoOpacity = 1.0f;
	bool LogoEnabled = false;
	std::string LogoURL = "https://assets.agilemark.io/images/agilemark-logo.png";
	std::string LogoHash = "";
	bool LogoShowTimestamp = false;
	bool CipherTextGridEnabled = true;
	int CipherTextGridCols = 3;
	int CipherTextGridRows = 3;
	float CipherTextGridOpacity = 0.3f;
	int CipherTextGridSize = 25;
	int CipherTextGridAngle = 20;
	std::string CipherTextGridColor1 = "#000000";
	std::string CipherTextGridColor2 = "#ffffff";
	float CipherTextGridBlurRadius = 10.0f;
	std::vector<std::vector<char>> CipherTextGridTemplate = { {'e','w','r'},{'h','k','*'},{'*','*','*'} };
	int CipherTextGridSpacingX = 200;
	int CipherTextGridSpacingY = 200;
	bool CipherTextGridShowTimestamp = true;
};

// Define to_json and from_json functions for MarkerJson
void to_json(nlohmann::json& j, const MarkerJson& m);
void from_json(const nlohmann::json& j, MarkerJson& m);

#endif // MARKERJSON_H