#include "Weather.h"
#include "WeatherExt.h"

#include <CCINIClass.h>
#include <WarheadTypeClass.h>
#include <TechnoTypeClass.h>
#include <TechnoClass.h>
#include <BuildingClass.h>
// Required even though nothing here names FootClass: TechnoClass.h pulls
// Helpers/Cast.h, whose APPLY_GC_ABSTRACT_CAST(FootClass*) instantiates
// generic_cast<const FootClass*>, and MSVC needs FootClass COMPLETE somewhere
// in the TU (see ScatterExt, same trap).
#include <FootClass.h>
#include <HouseClass.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>
#include <ScenarioClass.h>
#include <MapClass.h>
#include <Fundamentals.h>
#include <GeneralDefinitions.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>   // atoi, _itoa_s
#include <cstring>   // strtok_s, strlen
#include <cmath>

Weather::DllConfig Weather::Config;

namespace
{
	const char* const CFG_SECTION = "Weather";
	const char* const REGISTRY = "WeatherSystems";
	constexpr int MAX_METERS = 256;      // registry sanity cap
	constexpr int DET_LINE_BUDGET = 20;  // per-game individual detonation lines

	// ---- firing effect (P2) ----------------------------------------------
	enum class TargetMode { Random, MapCenter };

	// One threshold-gated firing slot: fires `swName` while the meter is at or
	// above `threshold`, every `effectiveInterval()` frames.
	struct FireSlot
	{
		std::string swName;
		int swIndex = -1;         // resolved SuperWeaponType index (cached)
		int threshold = 0;
		int count = 1;
		int baseInterval = 150;
		int intervalStep = 0;     // added to the interval per overshoot step
		int intervalStepAmount = 1;
		int intervalMin = 1;      // floor -- a negative step must never reach <=0
		int timer = 0;            // runtime: frames until next volley
		int fired = 0;            // runtime: volleys fired (for log budget)
	};

	// ---- meters -----------------------------------------------------------
	struct Meter
	{
		std::string Name;
		int StartAmount = 0;
		int Min = 0;
		int Max = 100000;
		int Baseline = 0;
		int Decay = 0;        // 0 => no drift
		int DecayRate = 15;   // frames between drift ticks (>=1)
		int Amount = 0;
		int decayCounter = 0;

		// firing effect
		std::vector<FireSlot> fireSlots;
		TargetMode target = TargetMode::Random;
		std::string owner = "neutral"; // neutral | random | <country>
		bool grantToOwner = true;
	};

	std::vector<Meter> Meters;
	std::unordered_map<std::string, int> MeterIndex; // name -> Meters[] index

	// ---- contributors (stored by NAME, resolved lazily) -------------------
	// Name-based storage makes parse ORDER irrelevant: a contributor can name a
	// meter that hasn't been read yet. Finalize() resolves names to indices
	// once, after all INI passes, before the first tick/detonation.
	struct Contrib
	{
		std::string meter;
		int amount = 0;
		int idx = -1; // resolved meter index, or -1 (unknown => skipped)
	};

	std::unordered_map<WarheadTypeClass*, std::vector<Contrib>> WarheadContrib;

	struct TechnoContrib
	{
		std::vector<Contrib> list;
		int rate = 0;            // apply once per `rate` frames; 0 => never (needs a rate)
		bool requiresPower = false;
	};
	std::unordered_map<TechnoTypeClass*, TechnoContrib> TechnoContribMap;

	bool anyPassive = false;
	bool finalized = false;

	int logCounter = 0;
	int detLinesLeft = DET_LINE_BUDGET;

	// ---- small parsing helpers -------------------------------------------
	bool ReadBoolKeepCurrent(CCINIClass* pINI, const char* section, const char* key, bool current)
	{
		if (!pINI->ReadString(section, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength))
			return current;
		const char* v = WeatherExtDLL::readBuffer;
		if (!*v)
			return current;
		switch (v[0])
		{
		case '1': case 't': case 'T': case 'y': case 'Y': return true;
		case '0': case 'f': case 'F': case 'n': case 'N': return false;
		default:
			Debug::Log("[WeatherExt] [%s] unrecognised %s=%s; keeping %s.\n",
				section, key, v, current ? "yes" : "no");
			return current;
		}
	}

	// Parse "a,b,c" from an INI value into name and integer lists. Returns the
	// names; `outAmounts` gets the parallel integers (missing/short entries = 0).
	void ReadNameList(CCINIClass* pINI, const char* section, const char* key,
		std::vector<std::string>& out)
	{
		out.clear();
		if (!pINI->ReadString(section, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength))
			return;
		char* ctx = nullptr;
		for (char* tok = strtok_s(WeatherExtDLL::readBuffer, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx))
		{
			while (*tok == ' ') ++tok;
			char* end = tok + strlen(tok);
			while (end > tok && end[-1] == ' ') *--end = '\0';
			if (*tok)
				out.emplace_back(tok);
		}
	}

	void ReadIntList(CCINIClass* pINI, const char* section, const char* key,
		std::vector<int>& out)
	{
		out.clear();
		if (!pINI->ReadString(section, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength))
			return;
		char* ctx = nullptr;
		for (char* tok = strtok_s(WeatherExtDLL::readBuffer, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx))
			out.push_back(atoi(tok));
	}

	// Build the [warhead/type] contributor list from the parallel .Types /
	// .Amounts pair. Present-only: if NEITHER key is present, keep whatever was
	// parsed on an earlier pass; if either is present, this pass defines it.
	bool ReadContribPair(CCINIClass* pINI, const char* section, std::vector<Contrib>& out)
	{
		std::vector<std::string> types;
		std::vector<int> amounts;
		ReadNameList(pINI, section, "WeatherSystem.Types", types);
		ReadIntList(pINI, section, "WeatherSystem.Amounts", amounts);

		if (types.empty() && amounts.empty())
			return false; // absent this pass; caller keeps prior value

		out.clear();
		for (size_t i = 0; i < types.size(); ++i)
		{
			Contrib c;
			c.meter = types[i];
			c.amount = i < amounts.size() ? amounts[i] : 0;
			out.push_back(std::move(c));
		}
		if (amounts.size() != types.size())
			Debug::Log("[WeatherExt] [%s] WeatherSystem.Types has %zu entries but "
				".Amounts has %zu; missing amounts default to 0.\n",
				section, types.size(), amounts.size());
		return true;
	}

	// Parse the Fire.* block for one meter. Present-only: absent Fire.Types
	// keeps whatever an earlier pass parsed.
	void ReadFireSlots(CCINIClass* pINI, Meter& m)
	{
		std::vector<std::string> types;
		ReadNameList(pINI, m.Name.c_str(), "Fire.Types", types);
		if (types.empty())
		{
			// still let target/owner/grant be tuned on a later pass
			if (!m.fireSlots.empty())
			{
				if (pINI->ReadString(m.Name.c_str(), "Fire.Owner", "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength) && *WeatherExtDLL::readBuffer)
					m.owner = WeatherExtDLL::readBuffer;
				m.grantToOwner = ReadBoolKeepCurrent(pINI, m.Name.c_str(), "Fire.GrantToOwner", m.grantToOwner);
			}
			return;
		}

		std::vector<int> amounts, counts, intervals, steps, stepAmounts, mins;
		ReadIntList(pINI, m.Name.c_str(), "Fire.Amounts", amounts);
		ReadIntList(pINI, m.Name.c_str(), "Fire.Counts", counts);
		ReadIntList(pINI, m.Name.c_str(), "Fire.Intervals", intervals);
		ReadIntList(pINI, m.Name.c_str(), "Fire.IntervalStep", steps);
		ReadIntList(pINI, m.Name.c_str(), "Fire.IntervalStepAmount", stepAmounts);
		ReadIntList(pINI, m.Name.c_str(), "Fire.IntervalMin", mins);

		auto at = [](const std::vector<int>& v, size_t i, int def) {
			return i < v.size() ? v[i] : def;
		};

		m.fireSlots.clear();
		for (size_t i = 0; i < types.size(); ++i)
		{
			FireSlot s;
			s.swName = types[i];
			s.threshold = at(amounts, i, 0);
			s.count = std::max(1, at(counts, i, 1));
			s.baseInterval = std::max(1, at(intervals, i, 150));
			s.intervalStep = at(steps, i, 0);
			s.intervalStepAmount = std::max(1, at(stepAmounts, i, 1));
			// Mandatory floor: without it a negative step drives the interval to
			// <= 0 (fire every frame). Default to the base interval so an
			// unspecified min never accelerates below the author's stated rate.
			s.intervalMin = std::max(1, at(mins, i, s.baseInterval));
			m.fireSlots.push_back(std::move(s));
		}

		// target mode
		m.target = TargetMode::Random;
		if (pINI->ReadString(m.Name.c_str(), "Fire.Target", "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength) && *WeatherExtDLL::readBuffer)
		{
			if (_strcmpi(WeatherExtDLL::readBuffer, "mapcenter") == 0)
				m.target = TargetMode::MapCenter;
			else if (_strcmpi(WeatherExtDLL::readBuffer, "random") != 0)
				Debug::Log("[WeatherExt] [%s] Fire.Target=%s unrecognised "
					"(expected random|mapcenter); using random.\n",
					m.Name.c_str(), WeatherExtDLL::readBuffer);
		}

		if (pINI->ReadString(m.Name.c_str(), "Fire.Owner", "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength) && *WeatherExtDLL::readBuffer)
			m.owner = WeatherExtDLL::readBuffer;
		m.grantToOwner = ReadBoolKeepCurrent(pINI, m.Name.c_str(), "Fire.GrantToOwner", true);
	}

	Meter* FindMeter(const char* name)
	{
		auto it = MeterIndex.find(name);
		return it != MeterIndex.end() ? &Meters[it->second] : nullptr;
	}

	int ClampMeter(const Meter& m, int v)
	{
		return std::clamp(v, m.Min, m.Max);
	}

	int Scaled(int amount)
	{
		return static_cast<int>(std::lround(amount * Weather::Config.WeatherScale));
	}

	// ---- firing (P2) ------------------------------------------------------
	int fireLinesLeft = 40; // per-game individual fire-line budget

	HouseClass* ResolveFiringHouse(const std::string& owner)
	{
		if (owner.empty() || _strcmpi(owner.c_str(), "neutral") == 0)
		{
			if (auto pH = HouseClass::FindNeutral()) return pH;
			if (auto pH = HouseClass::FindSpecial()) return pH;
			return HouseClass::Array.Count ? HouseClass::Array.Items[0] : nullptr;
		}
		if (_strcmpi(owner.c_str(), "random") == 0)
		{
			// synced pick among houses still in play; iterate in index order.
			int candidates = 0;
			for (int i = 0; i < HouseClass::Array.Count; ++i)
			{
				auto pH = HouseClass::Array.Items[i];
				if (pH && !pH->Defeated && !pH->IsNeutral())
					++candidates;
			}
			if (candidates == 0)
				return HouseClass::FindNeutral();
			int pick = ScenarioClass::Instance->Random.RandomRanged(0, candidates - 1);
			for (int i = 0; i < HouseClass::Array.Count; ++i)
			{
				auto pH = HouseClass::Array.Items[i];
				if (pH && !pH->Defeated && !pH->IsNeutral() && pick-- == 0)
					return pH;
			}
			return HouseClass::FindNeutral();
		}
		return HouseClass::FindByCountryName(owner.c_str());
	}

	CellStruct PickTargetCell(TargetMode mode)
	{
		const auto& b = MapClass::Instance.MapCoordBounds;
		CellStruct cell{};
		if (mode == TargetMode::MapCenter)
		{
			cell.X = static_cast<short>((b.Left + b.Right) / 2);
			cell.Y = static_cast<short>((b.Top + b.Bottom) / 2);
		}
		else // Random -- synced RNG so every client picks the same cell
		{
			auto& rng = ScenarioClass::Instance->Random;
			cell.X = static_cast<short>(rng.RandomRanged(b.Left, b.Right));
			cell.Y = static_cast<short>(rng.RandomRanged(b.Top, b.Bottom));
		}
		return cell;
	}

	// Fire one SW of `swIndex` from `pHouse` at `cell`, through Fire_SW
	// (0x4FAE50) so Antares' AND SuperWeaponExt's inhibitor/designator vetoes
	// both apply. Returns false if the house can't field the SW.
	bool FireOneSW(HouseClass* pHouse, int swIndex, bool grant, const CellStruct& cell)
	{
		if (!pHouse || swIndex < 0)
			return false;
		int idx = pHouse->FindSuperWeaponIndex(static_cast<SuperWeaponType>(swIndex));
		if (idx < 0)
			return false;
		SuperClass* pSuper = pHouse->Supers.GetItem(idx);
		if (!pSuper)
			return false;
		if (grant)
			pSuper->Grant(false, false, false); // permanent, silent, not on-hold
		pSuper->SetReadiness(true);             // WeatherExt owns cadence
		pHouse->Fire_SW(idx, cell);             // veto layers decide from here
		return true;
	}

	// Resolve every contributor's meter name to an index, once. Unknown names
	// are logged (typo detection) and left at idx = -1 so they're skipped.
	void Finalize()
	{
		auto resolve = [](std::vector<Contrib>& list, const char* owner)
		{
			for (auto& c : list)
			{
				auto it = MeterIndex.find(c.meter);
				if (it != MeterIndex.end())
					c.idx = it->second;
				else
				{
					c.idx = -1;
					Debug::Log("[WeatherExt] %s references unknown weather system "
						"'%s'; ignored.\n", owner, c.meter.c_str());
				}
			}
		};

		for (auto& kv : WarheadContrib)
			resolve(kv.second, kv.first->ID);

		anyPassive = false;
		for (auto& kv : TechnoContribMap)
		{
			resolve(kv.second.list, kv.first->ID);
			if (kv.second.rate > 0 && !kv.second.list.empty())
				anyPassive = true;
		}

		// Resolve fire-slot SW names to SuperWeaponType indices.
		for (auto& m : Meters)
			for (auto& s : m.fireSlots)
			{
				s.swIndex = SuperWeaponTypeClass::FindIndex(s.swName.c_str());
				if (s.swIndex < 0)
					Debug::Log("[WeatherExt] meter '%s' Fire.Types names unknown "
						"superweapon '%s'; that slot is disabled.\n",
						m.Name.c_str(), s.swName.c_str());
			}

		finalized = true;
	}
}

void Weather::ReadGlobals(CCINIClass* pINI)
{
	auto& c = Config;
	const bool hadSection = pINI->GetSection(CFG_SECTION) != nullptr;

	c.Enabled = ReadBoolKeepCurrent(pINI, CFG_SECTION, "Enabled", c.Enabled);
	c.LogInterval = pINI->ReadInteger(CFG_SECTION, "LogInterval", c.LogInterval);
	c.WeatherScale = pINI->ReadDouble(CFG_SECTION, "WeatherScale", c.WeatherScale);

	// Rebuild the meter registry from [WeatherSystems] (0=,1=,...). Present-only:
	// if the registry key is absent this pass, keep the meters we already have.
	std::vector<std::string> names;
	{
		char key[16];
		for (int i = 0; i < MAX_METERS; ++i)
		{
			_itoa_s(i, key, 10);
			if (!pINI->ReadString(REGISTRY, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength))
				break;
			if (!*WeatherExtDLL::readBuffer)
				break;
			names.emplace_back(WeatherExtDLL::readBuffer);
		}
	}

	if (!names.empty())
	{
		// Preserve current Amounts across a re-parse of the same meter so a
		// later pass (gamemode/map) that only tweaks config doesn't reset play
		// state -- though in practice every pass runs pre-game.
		std::unordered_map<std::string, int> prior;
		for (auto& m : Meters)
			prior[m.Name] = m.Amount;

		Meters.clear();
		MeterIndex.clear();
		for (auto& n : names)
		{
			Meter m;
			m.Name = n;
			Meters.push_back(std::move(m));
			MeterIndex[n] = static_cast<int>(Meters.size()) - 1;
		}

		for (auto& m : Meters)
		{
			auto it = prior.find(m.Name);
			if (it != prior.end())
				m.Amount = it->second;
		}
	}

	// (Re)read each meter's own config section.
	for (auto& m : Meters)
	{
		const char* s = m.Name.c_str();
		m.StartAmount = pINI->ReadInteger(s, "StartAmount", m.StartAmount);
		m.Min = pINI->ReadInteger(s, "Min", m.Min);
		m.Max = pINI->ReadInteger(s, "Max", m.Max);
		m.Baseline = pINI->ReadInteger(s, "Baseline", m.Baseline);
		m.Decay = pINI->ReadInteger(s, "Decay", m.Decay);
		m.DecayRate = std::max(1, pINI->ReadInteger(s, "DecayRate", m.DecayRate));
		if (m.Max < m.Min)
		{
			Debug::Log("[WeatherExt] [%s] Max=%d < Min=%d; swapping.\n", s, m.Max, m.Min);
			std::swap(m.Min, m.Max);
		}
		ReadFireSlots(pINI, m);

		// Every pass runs before gameplay, so seeding the start here means the
		// game begins at StartAmount (clamped) for the last pass that ran.
		m.Amount = ClampMeter(m, m.StartAmount);
		m.decayCounter = 0;
	}

	logCounter = 0;
	detLinesLeft = DET_LINE_BUDGET;
	fireLinesLeft = 40;
	finalized = false; // force re-resolve of contributors against the new registry

	if (hadSection || c.Enabled || !Meters.empty())
	{
		Debug::Log("[WeatherExt] globals: Enabled=%s LogInterval=%d WeatherScale=%.3f "
			"meters=%zu\n", c.Enabled ? "yes" : "no", c.LogInterval, c.WeatherScale,
			Meters.size());
		for (auto& m : Meters)
			Debug::Log("[WeatherExt]   meter '%s': start=%d range=[%d,%d] "
				"baseline=%d decay=%d/%df fireSlots=%zu owner=%s\n",
				m.Name.c_str(), m.StartAmount, m.Min, m.Max, m.Baseline,
				m.Decay, m.DecayRate, m.fireSlots.size(), m.owner.c_str());
	}
}

void Weather::ReadWarhead(WarheadTypeClass* pWH, CCINIClass* pINI)
{
	if (!pWH)
		return;
	std::vector<Contrib> list;
	if (!ReadContribPair(pINI, pWH->ID, list))
		return; // absent this pass; keep any prior parse
	if (list.empty())
		WarheadContrib.erase(pWH);
	else
		WarheadContrib[pWH] = std::move(list);
	finalized = false;
}

void Weather::ReadTechnoType(TechnoTypeClass* pType, CCINIClass* pINI)
{
	if (!pType)
		return;
	std::vector<Contrib> list;
	const bool present = ReadContribPair(pINI, pType->ID, list);

	// Rate/power are only meaningful with a contributor list; read them
	// present-only alongside it.
	if (!present)
	{
		// still allow rate/power to be tuned on a later pass if the list
		// already exists
		auto it = TechnoContribMap.find(pType);
		if (it == TechnoContribMap.end())
			return;
		it->second.rate = std::max(0, pINI->ReadInteger(pType->ID, "WeatherSystem.Rate", it->second.rate));
		it->second.requiresPower = ReadBoolKeepCurrent(pINI, pType->ID, "WeatherSystem.RequiresPower", it->second.requiresPower);
		finalized = false;
		return;
	}

	if (list.empty())
	{
		TechnoContribMap.erase(pType);
		finalized = false;
		return;
	}

	TechnoContrib tc;
	tc.list = std::move(list);
	tc.rate = std::max(0, pINI->ReadInteger(pType->ID, "WeatherSystem.Rate", 0));
	tc.requiresPower = ReadBoolKeepCurrent(pINI, pType->ID, "WeatherSystem.RequiresPower", false);
	if (tc.rate <= 0)
		Debug::Log("[WeatherExt] [%s] has WeatherSystem.Types but no "
			"WeatherSystem.Rate>0; it will never contribute passively.\n", pType->ID);
	TechnoContribMap[pType] = std::move(tc);
	finalized = false;
}

void Weather::OnDetonation(WarheadTypeClass* pWH, TechnoClass* pSource, HouseClass* pHouse)
{
	if (!Config.Enabled || !pWH)
		return;
	if (!finalized)
		Finalize();

	auto it = WarheadContrib.find(pWH);
	if (it == WarheadContrib.end())
		return;

	for (const auto& c : it->second)
	{
		if (c.idx < 0)
			continue;
		Meter& m = Meters[c.idx];
		const int delta = Scaled(c.amount);
		if (delta == 0)
			continue;
		const int before = m.Amount;
		m.Amount = ClampMeter(m, m.Amount + delta);

		if (detLinesLeft > 0 && before != m.Amount)
		{
			--detLinesLeft;
			auto pOwner = pHouse ? pHouse : (pSource ? pSource->Owner : nullptr);
			Debug::Log("[WeatherExt] frame %d: %s -> '%s' %+d (house %s): %d -> %d\n",
				Unsorted::CurrentFrame, pWH->ID, m.Name.c_str(), delta,
				pOwner ? pOwner->PlainName : "<none>", before, m.Amount);
		}
	}
}

void Weather::FrameTick()
{
	if (!Config.Enabled)
		return;
	if (!finalized)
		Finalize();

	// --- per-meter drift toward baseline ---
	for (auto& m : Meters)
	{
		if (m.Decay <= 0)
			continue;
		if (++m.decayCounter < m.DecayRate)
			continue;
		m.decayCounter = 0;
		if (m.Amount > m.Baseline)
			m.Amount = ClampMeter(m, std::max(m.Baseline, m.Amount - m.Decay));
		else if (m.Amount < m.Baseline)
			m.Amount = ClampMeter(m, std::min(m.Baseline, m.Amount + m.Decay));
	}

	// --- passive techno contributors ---
	// Walk the array in index order (lockstep-safe). Cadence is stateless:
	// a type with rate R contributes on frames where CurrentFrame % R == 0,
	// so no per-object counters (which would be a save/load + desync burden).
	if (anyPassive)
	{
		const int frame = Unsorted::CurrentFrame;
		auto& arr = TechnoClass::Array;
		for (int i = 0; i < arr.Count; ++i)
		{
			TechnoClass* pT = arr.Items[i];
			if (!pT || !pT->IsAlive || pT->InLimbo)
				continue;
			TechnoTypeClass* pType = pT->GetTechnoType();
			if (!pType)
				continue;
			auto it = TechnoContribMap.find(pType);
			if (it == TechnoContribMap.end())
				continue;
			const TechnoContrib& tc = it->second;
			if (tc.rate <= 0 || (frame % tc.rate) != 0)
				continue;
			if (tc.requiresPower && pT->WhatAmI() == AbstractType::Building
				&& !static_cast<BuildingClass*>(pT)->HasPower)
				continue;

			for (const auto& c : tc.list)
			{
				if (c.idx < 0)
					continue;
				Meter& m = Meters[c.idx];
				const int delta = Scaled(c.amount);
				if (delta != 0)
					m.Amount = ClampMeter(m, m.Amount + delta);
			}
		}
	}

	// --- Effect 1: threshold-gated superweapon firing ---
	// Deterministic: meters in registry order, slots in list order, timers
	// driven by synced Amount, targets/owners drawn from ScenarioClass::Random.
	for (auto& m : Meters)
	{
		for (auto& s : m.fireSlots)
		{
			if (s.swIndex < 0)
				continue;
			if (m.Amount < s.threshold)
			{
				s.timer = 0; // re-arm: fires immediately when it next crosses
				continue;
			}
			if (s.timer > 0)
			{
				--s.timer;
				continue;
			}

			// effective interval shrinks (or grows) with overshoot, floored.
			const int over = m.Amount - s.threshold;
			const int steps = over / s.intervalStepAmount;
			int interval = s.baseInterval + s.intervalStep * steps;
			interval = std::max(interval, s.intervalMin);

			HouseClass* pHouse = ResolveFiringHouse(m.owner);
			int launched = 0;
			for (int k = 0; k < s.count; ++k)
			{
				const CellStruct cell = PickTargetCell(m.target);
				if (FireOneSW(pHouse, s.swIndex, m.grantToOwner, cell))
					++launched;
			}
			s.timer = interval;
			++s.fired;

			if (fireLinesLeft > 0)
			{
				--fireLinesLeft;
				Debug::Log("[WeatherExt] frame %d: '%s'=%d fired %s x%d "
					"(owner %s, next in %df)\n", Unsorted::CurrentFrame,
					m.Name.c_str(), m.Amount, s.swName.c_str(), launched,
					pHouse ? pHouse->PlainName : "<none>", interval);
			}
		}
	}

	// --- periodic logging ---
	if (Config.LogInterval > 0 && ++logCounter >= Config.LogInterval)
	{
		logCounter = 0;
		for (auto& m : Meters)
			Debug::Log("[WeatherExt] frame %d: '%s' = %d\n",
				Unsorted::CurrentFrame, m.Name.c_str(), m.Amount);
	}
}

int Weather::GetAmount(const char* meterName)
{
	Meter* m = FindMeter(meterName);
	return m ? m->Amount : 0;
}

// ---------------------------------------------------------------------------
// Hooks. Seats verified against the encyclopedia + framework source (registers
// and return values included); every co-hooked handler at a shared address
// returns 0, so the Syringe chain always reaches us.
// ---------------------------------------------------------------------------

// RulesClass::Read_File tail. ESI = the CCINIClass just read; fires once per
// INI pass (rules, gamemode, map). Phobos stacks two hooks here, both return 0.
DEFINE_HOOK(0x668F6A, RulesClass_ReadFile_WeatherExt, 0x5)
{
	GET(CCINIClass*, pINI, ESI);
	Weather::ReadGlobals(pINI);
	return 0;
}

// WarheadTypeClass::LoadFromINI. ESI = warhead, [esp+0x150] = INI. Same layout
// as the Ares/Antares/Phobos hooks at this exact address (all size 0x5, all
// return 0).
DEFINE_HOOK(0x75DEA0, WarheadTypeClass_LoadFromINI_WeatherExt, 0x5)
{
	GET(WarheadTypeClass*, pItem, ESI);
	GET_STACK(CCINIClass*, pINI, 0x150);
	Weather::ReadWarhead(pItem, pINI);
	return 0;
}

// TechnoTypeClass::LoadFromINI, the single funnel for building/infantry/unit/
// aircraft types. EBP = type, [esp+0x380] = INI. Same layout as the
// Antares/Phobos hooks at this address (size 0x5, return 0).
DEFINE_HOOK(0x716123, TechnoTypeClass_LoadFromINI_WeatherExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, EBP);
	GET_STACK(CCINIClass*, pINI, 0x380);
	Weather::ReadTechnoType(pItem, pINI);
	return 0;
}

// MapClass::DamageArea, the universal area-damage funnel. EBP frame: warhead at
// +0xC, source techno at +0x8, house at +0x14. Same offsets as Phobos's
// MapClass_DamageArea hook at this address (size 0x6, returns 0; its BeforeAll
// twin also returns 0). Kratos's entry at 0x489280 is disjoint (and unloaded).
DEFINE_HOOK(0x489286, MapClass_DamageArea_WeatherExt, 0x6)
{
	GET_BASE(WarheadTypeClass*, pWH, 0xC);
	GET_BASE(TechnoClass*, pSource, 0x8);
	GET_BASE(HouseClass*, pHouse, 0x14);
	Weather::OnDetonation(pWH, pSource, pHouse);
	return 0;
}

// LogicClass::AI just after the per-object update loop -- the uncontended
// once-per-frame seat. ONE hook only: the whole 5-byte range is ours, so every
// per-frame consumer is called from inside it, never a second adjacent hook.
DEFINE_HOOK(0x55B6B3, LogicClass_AI_WeatherExtTick, 0x5)
{
	Weather::FrameTick();
	return 0;
}
