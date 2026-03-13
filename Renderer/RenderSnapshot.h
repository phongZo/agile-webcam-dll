#pragma once
#include <cmath>
#include <functional>
#include <string>
#include <vector>

struct RenderSnapshot
{
	// ====== Global ======
	bool  DrawingEnabled = true;
	float Opacity = 1.0f;

	// ====== Timestamp ======
	std::wstring TimestampFormat = L"HH:mm:ss dd/MM/yyyy";

	// =========================================================================
	// TEXT LAYER
	// =========================================================================
	bool         TextEnabled = false;
	std::wstring TextFormat = L"{machinename} | {shortdate} {shorttime}";
	float        TextSize = 28.0f;
	float        TextOpacity = 0.25f;
	float        TextAngleDeg = -20.0f;
	bool         TextSpacingEnabled = true;
	float        TextSpacingX = 320.0f;
	float        TextSpacingY = 160.0f;
	int          TextCols = 4;
	int          TextRows = 3;
	float        TextBlurRadius = 0.0f;
	bool         TextAdjustment = false;
	std::wstring TextColor1 = L"#000000";
	std::wstring TextColor2 = L"#FFFFFF";

	// =========================================================================
	// GRID
	// =========================================================================
	bool         GridEnabled = false;
	std::wstring GridShape = L"Square";
	float        GridBlurRadius = 0.0f;
	float        GridOpacity = 0.0f;
	int          GridRows = 5;
	int          GridCols = 5;
	int          Rows = 3;
	int          Cols = 6;
	float        CellWidth = 10.0f;
	float        CellHeight = 10.0f;
	int          GridSpacingX = 450;
	int          GridSpacingY = 200;
	int          RandomOffset = 0;
	float        MarginPercent = 0.0f;
	bool         GridSpacingEnabled = true;
	std::wstring GridColor1 = L"#000000";
	std::wstring GridColor2 = L"#FFFFFF";
	bool         GridShowTimestamp = false;

	// Logo
	bool         LogoEnabled = false;
	float        LogoOpacity = 0.0f;
	std::wstring LogoURL;
	std::wstring LogoHash;
	bool         LogoShowTimestamp = false;
	std::vector<bool>      LogoMatrix;
	std::wstring GridLogoURL;
	std::wstring GridLogoHash;
	std::wstring GridSecondaryLogoURL;
	std::wstring GridSecondaryLogoHash;

	// Cipher TEXT Grid
	bool         CipherTextGridEnabled = false;
	int          CipherTextGridCols = 3;
	int          CipherTextGridRows = 3;
	float        CipherTextGridOpacity = 0.5f;
	int          CipherTextGridSize = 25;
	int          CipherTextGridAngle = 20;
	std::wstring CipherTextGridColor1 = L"#000000";
	std::wstring CipherTextGridColor2 = L"#333333";
	float        CipherTextGridBlurRadius = 25.0f;
	int          CipherTextGridSpacingX = 200;
	int          CipherTextGridSpacingY = 200;
	bool         CipherTextGridShowTimestamp = false;
	bool         CipherTextSpacingEnabled = true;
	int          CipherTextRows = 5;
	int          CipherTextCols = 5;
	std::vector<wchar_t> CipherTextGridTemplate;

	// CIPHER SHAPE GRID
	bool         CipherShapeGridEnabled = false;
	float        CipherShapeStrokeThickness = 0.0f;
	int          CipherShapeGridCols = 3;
	int          CipherShapeGridRows = 3;
	float        CipherShapeGridOpacity = 1.0f;
	int          CipherShapeGridSize = 25;
	int          CipherShapeGridAngle = 20;
	std::wstring CipherShapeGridColor1 = L"#000000";
	std::wstring CipherShapeGridColor2 = L"#333333";
	float        CipherShapeGridBlurRadius = 10.0f;
	int          CipherShapeGridSpacingX = 200;
	int          CipherShapeGridSpacingY = 200;
	bool         CipherShapeGridShowTimestamp = false;
	bool         CipherShapeSpacingEnabled = true;
	int          CipherShapeRows = 5;
	int          CipherShapeCols = 5;
	std::vector<wchar_t> CipherShapeGridTemplate;

	static float NormalizeAngleDeg(float a) {
		int ai = (int)a % 360;
		if (ai < 0) ai += 360;
		if (ai >= 180) ai -= 180 * 2;
		return (float)ai;
	}

	// ======================
	// Signature & equality
	// ======================
	size_t Signature() const noexcept {
		std::size_t seed = 0;

		// helpers
		auto hash_combine = [](std::size_t& s, std::size_t v) {
			s ^= v + 0x9e3779b97f4a7c15ull + (s << 6) + (s >> 2);
			};
		auto hash_w = [&](const std::wstring& ws) {
			std::size_t s = 0;
			for (wchar_t c : ws) hash_combine(s, std::hash<wchar_t>{}(c));
			return s;
			};
		auto hash_floatq = [&](float v, float eps = 1e-3f) {
			if (std::isnan(v)) return std::size_t{ 0x7ff1u };
			if (std::isinf(v)) return std::size_t{ 0x7ff2u };
			long long q = llroundf(v / eps);
			return std::hash<long long>{}(q);
			};

		// Global
		hash_combine(seed, std::hash<bool>{}(DrawingEnabled));
		hash_combine(seed, hash_floatq(Opacity));
		hash_combine(seed, hash_w(TimestampFormat));

		// Text
		hash_combine(seed, std::hash<bool>{}(TextEnabled));
		hash_combine(seed, hash_w(TextFormat));
		hash_combine(seed, hash_floatq(TextSize));
		hash_combine(seed, hash_floatq(TextOpacity));
		hash_combine(seed, hash_floatq(TextBlurRadius));
		hash_combine(seed, std::hash<bool>{}(TextAdjustment));
		hash_combine(seed, std::hash<bool>{}(TextSpacingEnabled));
		hash_combine(seed, hash_floatq(TextSpacingX));
		hash_combine(seed, hash_floatq(TextSpacingY));
		hash_combine(seed, std::hash<int>{}(TextCols));
		hash_combine(seed, std::hash<int>{}(TextRows));
		hash_combine(seed, hash_w(TextColor1));
		hash_combine(seed, hash_w(TextColor2));
		hash_combine(seed, hash_floatq(TextAngleDeg));

		// Grid
		hash_combine(seed, std::hash<bool>{}(GridEnabled));
		hash_combine(seed, hash_w(GridShape));
		hash_combine(seed, hash_floatq(GridBlurRadius));
		hash_combine(seed, hash_floatq(GridOpacity));
		hash_combine(seed, std::hash<int>{}(GridRows));
		hash_combine(seed, std::hash<int>{}(GridCols));
		hash_combine(seed, std::hash<int>{}(Rows));
		hash_combine(seed, std::hash<int>{}(Cols));
		hash_combine(seed, hash_floatq(CellWidth));
		hash_combine(seed, hash_floatq(CellHeight));
		hash_combine(seed, std::hash<int>{}(GridSpacingX));
		hash_combine(seed, std::hash<int>{}(GridSpacingY));
		hash_combine(seed, std::hash<int>{}(RandomOffset));
		hash_combine(seed, hash_floatq(MarginPercent));
		hash_combine(seed, std::hash<bool>{}(GridSpacingEnabled));
		hash_combine(seed, hash_w(GridColor1));
		hash_combine(seed, hash_w(GridColor2));
		hash_combine(seed, std::hash<bool>{}(GridShowTimestamp));

		// Logo
		hash_combine(seed, std::hash<bool>{}(LogoEnabled));
		hash_combine(seed, hash_floatq(LogoOpacity));
		hash_combine(seed, hash_w(LogoURL));
		hash_combine(seed, hash_w(LogoHash));
		hash_combine(seed, std::hash<bool>{}(LogoShowTimestamp));
		for (bool b : LogoMatrix) hash_combine(seed, std::hash<bool>{}(b));
		hash_combine(seed, hash_w(GridLogoURL));
		hash_combine(seed, hash_w(GridLogoHash));
		hash_combine(seed, hash_w(GridSecondaryLogoURL));
		hash_combine(seed, hash_w(GridSecondaryLogoHash));

		// Cipher TEXT Grid
		hash_combine(seed, std::hash<bool>{}(CipherTextGridEnabled));
		hash_combine(seed, std::hash<int>{}(CipherTextGridCols));
		hash_combine(seed, std::hash<int>{}(CipherTextGridRows));
		hash_combine(seed, hash_floatq(CipherTextGridOpacity));
		hash_combine(seed, std::hash<int>{}(CipherTextGridSize));
		hash_combine(seed, std::hash<int>{}(CipherTextGridAngle));
		hash_combine(seed, hash_w(CipherTextGridColor1));
		hash_combine(seed, hash_w(CipherTextGridColor2));
		hash_combine(seed, hash_floatq(CipherTextGridBlurRadius));
		hash_combine(seed, std::hash<int>{}(CipherTextGridSpacingX));
		hash_combine(seed, std::hash<int>{}(CipherTextGridSpacingY));
		hash_combine(seed, std::hash<bool>{}(CipherTextGridShowTimestamp));
		hash_combine(seed, std::hash<bool>{}(CipherTextSpacingEnabled));
		hash_combine(seed, std::hash<int>{}(CipherTextRows));
		hash_combine(seed, std::hash<int>{}(CipherTextCols));
		for (wchar_t ch : CipherTextGridTemplate) hash_combine(seed, std::hash<wchar_t>{}(ch));

		// Cipher SHAPE Grid
		hash_combine(seed, std::hash<bool>{}(CipherShapeGridEnabled));
		hash_combine(seed, hash_floatq(CipherShapeStrokeThickness));
		hash_combine(seed, std::hash<int>{}(CipherShapeGridCols));
		hash_combine(seed, std::hash<int>{}(CipherShapeGridRows));
		hash_combine(seed, hash_floatq(CipherShapeGridOpacity));
		hash_combine(seed, std::hash<int>{}(CipherShapeGridSize));
		hash_combine(seed, std::hash<int>{}(CipherShapeGridAngle));
		hash_combine(seed, hash_w(CipherShapeGridColor1));
		hash_combine(seed, hash_w(CipherShapeGridColor2));
		hash_combine(seed, hash_floatq(CipherShapeGridBlurRadius));
		hash_combine(seed, std::hash<int>{}(CipherShapeGridSpacingX));
		hash_combine(seed, std::hash<int>{}(CipherShapeGridSpacingY));
		hash_combine(seed, std::hash<bool>{}(CipherShapeGridShowTimestamp));
		hash_combine(seed, std::hash<bool>{}(CipherShapeSpacingEnabled));
		hash_combine(seed, std::hash<int>{}(CipherShapeRows));
		hash_combine(seed, std::hash<int>{}(CipherShapeCols));
		for (wchar_t ch : CipherShapeGridTemplate) hash_combine(seed, std::hash<wchar_t>{}(ch));

		return seed;
	}

	bool RenderEquals(const RenderSnapshot& other) const noexcept {
		return Signature() == other.Signature();
	}
};
