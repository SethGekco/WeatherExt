#include "WeatherExt.h"

#include <Phobos.h>
#include <Syringe.h>
#include <Utilities/Patch.h>
#include <Utilities/Macro.h>

HANDLE WeatherExtDLL::hInstance = nullptr;

char WeatherExtDLL::readBuffer[WeatherExtDLL::readLength];
wchar_t WeatherExtDLL::wideBuffer[WeatherExtDLL::readLength];

namespace
{
	bool patchesApplied = false;
}

void WeatherExtDLL::ExeRun()
{
	if (patchesApplied)
		return;
	patchesApplied = true;
	Patch::ApplyStatic();
}

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		WeatherExtDLL::hInstance = hInstance;
		Phobos::hInstance = hInstance; // needed by Patch::ApplyStatic
	}
	return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
	pInfo->Message = const_cast<char*>("WeatherExt");
	return S_OK;
}

// Init seats, both taken (lesson from ScatterExt): 0x7CD810 is contested by
// five frameworks and a non-zero return upstream would starve us; 0x7CD81E is
// uncontested (`mov eax, fs:0` -- one whole instruction, no relative branch,
// safe to re-execute from the trampoline). ExeRun() is idempotent, so both
// firing is fine and either alone is enough.
//
// Do NOT log from these: WinMain hasn't parsed the command line yet, so
// debug.log doesn't exist and Debug::Log writes nowhere.
DEFINE_HOOK(0x7CD810, WeatherExt_ExeRun, 0x9)
{
	WeatherExtDLL::ExeRun();
	return 0;
}

DEFINE_HOOK(0x7CD81E, WeatherExt_ExeRunAlt, 0x6)
{
	WeatherExtDLL::ExeRun();
	return 0;
}
