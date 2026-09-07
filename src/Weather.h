#pragma once

class CCINIClass;
class WarheadTypeClass;
class TechnoClass;
class HouseClass;

// Phase 0: one synced global weather level.
//
// Mutation rules (multiplayer determinism): the level changes ONLY from
// synced sim events -- warhead detonations and the per-frame decay tick.
// Logging never feeds back into sim state.
namespace Weather
{
	struct GlobalConfig
	{
		bool Enabled = false;   // no [Weather] section => DLL stays inert
		int Initial = 0;
		int Baseline = 0;
		int Decay = 1;          // level units removed per DecayRate frames
		int DecayRate = 15;     // frames between decay ticks
		int MinLevel = 0;
		int MaxLevel = 10000;
		int LogInterval = 150;  // frames between periodic log lines (0 = off)
	};

	extern GlobalConfig Config;
	extern int Level;

	// Present-only-update: absent keys keep their current values, so the
	// rules / gamemode / map INI passes layer exactly like vanilla tags do.
	void ReadGlobals(CCINIClass* pINI);

	void ReadWarhead(WarheadTypeClass* pWH, CCINIClass* pINI);

	void OnDetonation(WarheadTypeClass* pWH, TechnoClass* pSource, HouseClass* pHouse);

	void FrameTick();
}
