#include "Weather.h"
#include "WeatherExt.h"

#include <CCINIClass.h>
#include <WarheadTypeClass.h>
#include <TechnoClass.h>
// Required even though nothing here names FootClass: TechnoClass.h pulls
// Helpers/Cast.h, whose APPLY_GC_ABSTRACT_CAST(FootClass*) instantiates
// generic_cast<const FootClass*>, and MSVC needs FootClass COMPLETE somewhere
// in the TU (see ScatterExt, same trap).
#include <FootClass.h>
#include <HouseClass.h>
#include <Fundamentals.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <unordered_map>
#include <algorithm>

Weather::GlobalConfig Weather::Config;
int Weather::Level = 0;

namespace
{
	const char* const SECTION = "Weather";

	// Keyed by type pointer; only looked up by pointer, never iterated, so
	// map order cannot desync anything. Entries from a previous game can
	// dangle harmlessly: a reused allocation always passes through
	// LoadFromINI again before it can detonate, which overwrites the slot.
	std::unordered_map<WarheadTypeClass*, int> WarheadDeltas;

	int decayCounter = 0;
	int logCounter = 0;

	// Detonation stats between periodic log lines, plus a first-N budget of
	// individual lines so early testing shows each hit without a storm of
	// deltas flooding debug.log later.
	int detApplied = 0;
	int detSum = 0;
	int detLinesLeft = 20;

	int Clamp(int v)
	{
		return std::clamp(v, Weather::Config.MinLevel, Weather::Config.MaxLevel);
	}

	// CCINIClass::ReadBool treats an absent key as the fallback already, but
	// we route through ReadString so a typo'd value gets logged instead of
	// silently reading as "no".
	bool ReadBoolKeepCurrent(CCINIClass* pINI, const char* key, bool current)
	{
		if (!pINI->ReadString(SECTION, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength))
			return current;

		const char* v = WeatherExtDLL::readBuffer;
		if (!*v)
			return current;

		switch (v[0])
		{
		case '1': case 't': case 'T': case 'y': case 'Y':
			return true;
		case '0': case 'f': case 'F': case 'n': case 'N':
			return false;
		default:
			Debug::Log("[WeatherExt] [%s] unrecognised %s=%s; keeping %s.\n",
				SECTION, key, v, current ? "yes" : "no");
			return current;
		}
	}
}

void Weather::ReadGlobals(CCINIClass* pINI)
{
	auto& c = Config;
	const bool hadSection = pINI->GetSection(SECTION) != nullptr;

	c.Enabled = ReadBoolKeepCurrent(pINI, "Enabled", c.Enabled);
	c.Initial = pINI->ReadInteger(SECTION, "Initial", c.Initial);
	c.Baseline = pINI->ReadInteger(SECTION, "Baseline", c.Baseline);
	c.Decay = pINI->ReadInteger(SECTION, "Decay", c.Decay);
	c.DecayRate = std::max(1, pINI->ReadInteger(SECTION, "DecayRate", c.DecayRate));
	c.MinLevel = pINI->ReadInteger(SECTION, "Min", c.MinLevel);
	c.MaxLevel = pINI->ReadInteger(SECTION, "Max", c.MaxLevel);
	c.LogInterval = pINI->ReadInteger(SECTION, "LogInterval", c.LogInterval);

	if (c.MaxLevel < c.MinLevel)
	{
		Debug::Log("[WeatherExt] [%s] Max=%d < Min=%d; swapping.\n", SECTION, c.MaxLevel, c.MinLevel);
		std::swap(c.MinLevel, c.MaxLevel);
	}

	// This tail fires once per INI pass (rules, gamemode, map), all before
	// gameplay, so resetting here means every game starts from Initial.
	Level = Clamp(c.Initial);
	decayCounter = 0;
	logCounter = 0;
	detApplied = 0;
	detSum = 0;
	detLinesLeft = 20;

	// Echo what was actually parsed (standing rule: the INI last-line trap and
	// silent typos are only visible if the DLL says what it read).
	if (hadSection || c.Enabled)
	{
		Debug::Log("[WeatherExt] globals: Enabled=%s Initial=%d Baseline=%d "
			"Decay=%d/%df range=[%d,%d] LogInterval=%d (section present: %s)\n",
			c.Enabled ? "yes" : "no", c.Initial, c.Baseline,
			c.Decay, c.DecayRate, c.MinLevel, c.MaxLevel, c.LogInterval,
			hadSection ? "yes" : "no");
	}
}

void Weather::ReadWarhead(WarheadTypeClass* pWH, CCINIClass* pINI)
{
	if (!pWH)
		return;

	const auto it = WarheadDeltas.find(pWH);
	const int current = it != WarheadDeltas.end() ? it->second : 0;
	const int value = pINI->ReadInteger(pWH->ID, "Weather.Delta", current);

	if (value != current)
	{
		WarheadDeltas[pWH] = value;
		Debug::Log("[WeatherExt] [%s] Weather.Delta=%d\n", pWH->ID, value);
	}
}

void Weather::OnDetonation(WarheadTypeClass* pWH, TechnoClass* pSource, HouseClass* pHouse)
{
	if (!Config.Enabled || !pWH)
		return;

	const auto it = WarheadDeltas.find(pWH);
	if (it == WarheadDeltas.end() || it->second == 0)
		return;

	const int before = Level;
	Level = Clamp(Level + it->second);
	++detApplied;
	detSum += it->second;

	if (detLinesLeft > 0)
	{
		--detLinesLeft;
		const auto pOwner = pHouse ? pHouse : (pSource ? pSource->Owner : nullptr);
		Debug::Log("[WeatherExt] frame %d: %s detonated (delta %+d, house %s): level %d -> %d\n",
			Unsorted::CurrentFrame, pWH->ID, it->second,
			pOwner ? pOwner->PlainName : "<none>", before, Level);
	}
}

void Weather::FrameTick()
{
	if (!Config.Enabled)
		return;

	if (++decayCounter >= Config.DecayRate)
	{
		decayCounter = 0;
		if (Level > Config.Baseline)
			Level = Clamp(std::max(Config.Baseline, Level - Config.Decay));
		else if (Level < Config.Baseline)
			Level = Clamp(std::min(Config.Baseline, Level + Config.Decay));
	}

	if (Config.LogInterval > 0 && ++logCounter >= Config.LogInterval)
	{
		logCounter = 0;
		Debug::Log("[WeatherExt] frame %d: level=%d (%d detonations, %+d total since last line)\n",
			Unsorted::CurrentFrame, Level, detApplied, detSum);
		detApplied = 0;
		detSum = 0;
	}
}

// ---------------------------------------------------------------------------
// Hooks. All four seats verified against the encyclopedia + framework source
// before writing (registers included); all co-hooked handlers at shared
// addresses return 0, so the chain always reaches us.
// ---------------------------------------------------------------------------

// RulesClass::Read_File tail. Fires once per INI pass (rules, gamemode, map)
// with ESI = the CCINIClass just read. Phobos stacks two hooks here, both
// return 0. Present-only-update parsing makes later passes into overrides --
// which is exactly how the Game Options lobby files will drive this in P4.
DEFINE_HOOK(0x668F6A, RulesClass_ReadFile_WeatherExt, 0x5)
{
	GET(CCINIClass*, pINI, ESI);
	Weather::ReadGlobals(pINI);
	return 0;
}

// WarheadTypeClass::LoadFromINI. ESI = warhead, [esp+0x150] = INI; identical
// reads to the Ares/Antares/Phobos hooks at this exact address (all size 0x5,
// all return 0). Parsing here instead of at the rules tail avoids any
// ordering race with Phobos's deferred LoadTypesFromINI.
DEFINE_HOOK(0x75DEA0, WarheadTypeClass_LoadFromINI_WeatherExt, 0x5)
{
	GET(WarheadTypeClass*, pItem, ESI);
	GET_STACK(CCINIClass*, pINI, 0x150);
	Weather::ReadWarhead(pItem, pINI);
	return 0;
}

// MapClass::DamageArea, the universal area-damage funnel: bullets, ivan
// bombs, lightning bolts, death weapons all pass through. 0x489286 is after
// the frame setup; argument offsets lifted verbatim from Phobos's
// MapClass_DamageArea hook at this same address (size 0x6, returns 0; its
// BeforeAll twin also returns 0). Entry 0x489280 belongs to Kratos -- not
// loaded here, and staying off it keeps the ranges disjoint anyway.
DEFINE_HOOK(0x489286, MapClass_DamageArea_WeatherExt, 0x6)
{
	GET_BASE(WarheadTypeClass*, pWH, 0xC);
	GET_BASE(TechnoClass*, pSource, 0x8);
	GET_BASE(HouseClass*, pHouse, 0x14);
	Weather::OnDetonation(pWH, pSource, pHouse);
	return 0;
}

// LogicClass::AI just after the per-object update loop -- the uncontended
// once-per-frame seat (only unmerged Phobos PR #352 knows it). ONE hook only:
// the whole 5-byte range is ours, a second hook beside it would overlap the
// stolen bytes. Every future per-frame consumer gets called from inside this.
DEFINE_HOOK(0x55B6B3, LogicClass_AI_WeatherExtTick, 0x5)
{
	Weather::FrameTick();
	return 0;
}
