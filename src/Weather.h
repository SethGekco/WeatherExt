#pragma once

class CCINIClass;
class WarheadTypeClass;
class TechnoTypeClass;
class TechnoClass;
class HouseClass;

// P1: named meters + contributors.
//
// A "weather system" is a named signed-integer meter in synced sim state. Many
// meters coexist (the [WeatherSystems] registry). Contributors add to named
// meters: warheads on detonation, technos passively over time. (SW-fired
// contributions land in P2, alongside the SW-firing effect, because both share
// the Fire_SW/Launch veto-seat design.)
//
// Determinism: meters mutate ONLY from synced events (detonations) and the
// synced per-frame tick. Iteration is array-index order, math is integer.
namespace Weather
{
	struct DllConfig
	{
		bool Enabled = false;     // no [Weather] section => DLL stays inert
		int LogInterval = 150;    // frames between periodic log lines (0 = off)
		double WeatherScale = 1.0; // global multiplier on every contributor amount
	};

	extern DllConfig Config;

	// Present-only-update everywhere: absent keys keep current values, so the
	// rules / gamemode / map passes (and spawn.ini lobby overrides) layer.
	void ReadGlobals(CCINIClass* pINI);       // [Weather] + [WeatherSystems] + per-meter sections
	void ReadWarhead(WarheadTypeClass* pWH, CCINIClass* pINI);
	void ReadTechnoType(TechnoTypeClass* pType, CCINIClass* pINI);

	void OnDetonation(WarheadTypeClass* pWH, TechnoClass* pSource, HouseClass* pHouse);
	void FrameTick();

	// Read-only accessor for later effects / tests. Unknown name => 0.
	int GetAmount(const char* meterName);
}
