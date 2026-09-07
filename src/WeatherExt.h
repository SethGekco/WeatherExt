#pragma once

#include <Windows.h>

class WeatherExtDLL
{
public:
	static HANDLE hInstance;

	static constexpr size_t readLength = 2048;
	static char readBuffer[readLength];
	static wchar_t wideBuffer[readLength];

	static void ExeRun();
};
