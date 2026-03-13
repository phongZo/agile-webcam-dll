#include "pch.h"
#include "MarkerJson.h"
#include <nlohmann/json.hpp>
// Define to_json function
void to_json(nlohmann::json& j, const MarkerJson& m) {
	j = nlohmann::json{
		{"DrawingEnabled", m.DrawingEnabled},
		{"Opacity", m.Opacity},
		{"TimestampFormat", m.TimestampFormat},
		{"GridEnabled", m.GridEnabled},
		{"GridShape", m.GridShape},
		{"GridOpacity", m.GridOpacity},
		{"Cols", m.Cols},
		{"Rows", m.Rows},
		{"CellWidth", m.CellWidth},
		{"CellHeight", m.CellHeight},
		{"GridSpacingX", m.GridSpacingX},
		{"GridSpacingY", m.GridSpacingY},
		{"RandomOffsetDistance", m.RandomOffsetDistance},
		{"MarginPercent", m.MarginPercent},
		{"GridColor1", m.GridColor1},
		{"GridColor2", m.GridColor2},
		{"GridBlurRadius", m.GridBlurRadius},
		{"GridLogoURL", m.GridLogoURL},
		{"GridLogoHash", m.GridLogoHash},
		{"GridSecondaryLogoURL", m.GridSecondaryLogoURL},
		{"GridSecondaryLogoHash", m.GridSecondaryLogoHash},
		{"GridShowTimestamp", m.GridShowTimestamp},
		{"TextOpacity", m.TextOpacity},
		{"TextEnabled", m.TextEnabled},
		{"TextFormat", m.TextFormat},
		{"TextCustomDateTimeFormat", m.TextCustomDateTimeFormat},
		{"TextSpacingEnabled", m.TextSpacingEnabled},
		{"TextBlurRadius", m.TextBlurRadius},
		{"TextAdjustment", m.TextAdjustment},
		{"TextRows", m.TextRows},
		{"TextCols", m.TextCols},
		{"TextSize", m.TextSize},
		{"TextAngle", m.TextAngle},
		{"TextSpacingX", m.TextSpacingX},
		{"TextSpacingY", m.TextSpacingY},
		{"TextColor1", m.TextColor1},
		{"TextColor2", m.TextColor2},
		{"LogoOpacity", m.LogoOpacity},
		{"LogoEnabled", m.LogoEnabled},
		{"LogoURL", m.LogoURL},
		{"LogoHash", m.LogoHash},
		{"LogoShowTimestamp", m.LogoShowTimestamp},
		{"CipherTextGridEnabled", m.CipherTextGridEnabled},
		{"CipherTextGridCols", m.CipherTextGridCols},
		{"CipherTextGridRows", m.CipherTextGridRows},
		{"CipherTextGridOpacity", m.CipherTextGridOpacity},
		{"CipherTextGridSize", m.CipherTextGridSize},
		{"CipherTextGridAngle", m.CipherTextGridAngle},
		{"CipherTextGridColor1", m.CipherTextGridColor1},
		{"CipherTextGridColor2", m.CipherTextGridColor2},
		{"CipherTextGridBlurRadius", m.CipherTextGridBlurRadius},
		{"CipherTextGridSpacingX", m.CipherTextGridSpacingX},
		{"CipherTextGridSpacingY", m.CipherTextGridSpacingY},
		{"CipherTextGridShowTimestamp", m.CipherTextGridShowTimestamp},
		{"CipherTextGridTemplate", m.CipherTextGridTemplate}


	};
}

// Define from_json function
void from_json(const nlohmann::json& j, MarkerJson& m) {
	j.at("DrawingEnabled").get_to(m.DrawingEnabled);
	j.at("Opacity").get_to(m.Opacity);
	j.at("TimestampFormat").get_to(m.TimestampFormat);
	j.at("GridEnabled").get_to(m.GridEnabled);
	j.at("GridShape").get_to(m.GridShape);
	j.at("GridOpacity").get_to(m.GridOpacity);
	j.at("Cols").get_to(m.Cols);
	j.at("Rows").get_to(m.Rows);
	j.at("CellWidth").get_to(m.CellWidth);
	j.at("CellHeight").get_to(m.CellHeight);
	j.at("GridSpacingX").get_to(m.GridSpacingX);
	j.at("GridSpacingY").get_to(m.GridSpacingY);
	j.at("RandomOffsetDistance").get_to(m.RandomOffsetDistance);
	j.at("MarginPercent").get_to(m.MarginPercent);
	j.at("GridColor1").get_to(m.GridColor1);
	j.at("GridColor2").get_to(m.GridColor2);
	j.at("GridBlurRadius").get_to(m.GridBlurRadius);
	j.at("GridLogoURL").get_to(m.GridLogoURL);
	j.at("GridLogoHash").get_to(m.GridLogoHash);
	j.at("GridSecondaryLogoURL").get_to(m.GridSecondaryLogoURL);
	j.at("GridSecondaryLogoHash").get_to(m.GridSecondaryLogoHash);
	j.at("GridShowTimestamp").get_to(m.GridShowTimestamp);
	j.at("TextOpacity").get_to(m.TextOpacity);
	j.at("TextEnabled").get_to(m.TextEnabled);
	j.at("TextFormat").get_to(m.TextFormat);
	j.at("TextCustomDateTimeFormat").get_to(m.TextCustomDateTimeFormat);
	j.at("TextSpacingEnabled").get_to(m.TextSpacingEnabled);
	j.at("TextBlurRadius").get_to(m.TextBlurRadius);
	j.at("TextAdjustment").get_to(m.TextAdjustment);
	j.at("TextRows").get_to(m.TextRows);
	j.at("TextCols").get_to(m.TextCols);
	j.at("TextSize").get_to(m.TextSize);
	j.at("TextAngle").get_to(m.TextAngle);
	j.at("TextSpacingX").get_to(m.TextSpacingX);
	j.at("TextSpacingY").get_to(m.TextSpacingY);
	j.at("TextColor1").get_to(m.TextColor1);
	j.at("TextColor2").get_to(m.TextColor2);
	j.at("LogoOpacity").get_to(m.LogoOpacity);
	j.at("LogoEnabled").get_to(m.LogoEnabled);
	j.at("LogoURL").get_to(m.LogoURL);
	j.at("LogoHash").get_to(m.LogoHash);
	j.at("LogoShowTimestamp").get_to(m.LogoShowTimestamp);
	j.at("CipherTextGridEnabled").get_to(m.CipherTextGridEnabled);
	j.at("CipherTextGridCols").get_to(m.CipherTextGridCols);
	j.at("CipherTextGridRows").get_to(m.CipherTextGridRows);
	j.at("CipherTextGridOpacity").get_to(m.CipherTextGridOpacity);
	j.at("CipherTextGridSize").get_to(m.CipherTextGridSize);
	j.at("CipherTextGridAngle").get_to(m.CipherTextGridAngle);
	j.at("CipherTextGridColor1").get_to(m.CipherTextGridColor1);
	j.at("CipherTextGridColor2").get_to(m.CipherTextGridColor2);
	j.at("CipherTextGridBlurRadius").get_to(m.CipherTextGridBlurRadius);
	j.at("CipherTextGridSpacingX").get_to(m.CipherTextGridSpacingX);
	j.at("CipherTextGridSpacingY").get_to(m.CipherTextGridSpacingY);
	j.at("CipherTextGridShowTimestamp").get_to(m.CipherTextGridShowTimestamp);
	j.at("CipherTextGridTemplate").get_to(m.CipherTextGridTemplate);
}