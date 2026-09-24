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

	// ---- warhead-vs-armor effect (P3) ------------------------------------
	// A weather-driven MULTIPLIER on a warhead's damage, keyed by the meter
	// amount and (optionally) the victim's armor. Applied at GetTotalDamage,
	// so it multiplies on TOP of the warhead's base Verses row.
	struct VersusCurve
	{
		std::string whName;
		WarheadTypeClass* wh = nullptr; // resolved in Finalize

		// Tier A: one uniform scalar for the whole warhead.
		int perAmountA = 0;          // 0 => Tier A off
		double multA = 0.0;          // factor += multA per step
		double multMaxA = 1e9;

		// Tier C: keyframed per-armor multiplier rows (positional, Verses order).
		std::vector<int> keyframes;              // ascending meter amounts
		std::vector<std::vector<double>> rows;   // rows[k][armorIndex] multiplier
		bool interpStep = false;                 // false = linear, true = hold
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

		// warhead-vs-armor effect
		std::vector<VersusCurve> versus;
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

	// SW-fired contributor (P2b): START delta on fire, optional FINISH delta
	// scheduled `interval` frames later.
	struct SWContrib
	{
		std::vector<Contrib> start;   // .Types + .Amounts
		std::vector<int> finish;      // .Amounts.Finish, parallel to start (empty => none)
		int interval = 0;             // .Interval frames; <=0 => finish applied with start
	};
	std::unordered_map<SuperWeaponTypeClass*, SWContrib> SWContribMap;

	// The synced pending-finish queue: each entry fires its delta at finishFrame.
	struct PendingFinish { int finishFrame; int meterIdx; int amount; };
	std::vector<PendingFinish> pendingFinish;

	// Fast per-warhead lookup for the damage hook, built in Finalize. Points
	// into Meters[].versus (stable between Finalize and the next re-parse).
	struct ResolvedVersus { int meterIdx; const VersusCurve* curve; };
	std::unordered_map<WarheadTypeClass*, std::vector<ResolvedVersus>> WarheadVersusMap;

	bool anyPassive = false;
	bool anySWContrib = false;
	bool anyVersus = false;
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

	// Double list; a trailing '%' means "divide by 100" so both 2.0 and 200%
	// read as x2 multipliers.
	void ReadDoubleList(CCINIClass* pINI, const char* section, const char* key,
		std::vector<double>& out)
	{
		out.clear();
		if (!pINI->ReadString(section, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength))
			return;
		char* ctx = nullptr;
		for (char* tok = strtok_s(WeatherExtDLL::readBuffer, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx))
		{
			double v = atof(tok);
			if (strchr(tok, '%'))
				v /= 100.0;
			out.push_back(v);
		}
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

	// Parse the Versus.* block for one meter. Present-only: absent
	// Versus.Warheads keeps whatever an earlier pass parsed.
	void ReadMeterVersus(CCINIClass* pINI, Meter& m)
	{
		std::vector<std::string> whs;
		ReadNameList(pINI, m.Name.c_str(), "Versus.Warheads", whs);
		if (whs.empty())
			return;

		m.versus.clear();
		char key[128];
		for (auto& wh : whs)
		{
			VersusCurve c;
			c.whName = wh;

			// Tier A
			_snprintf_s(key, sizeof(key), "Versus.%s.PerAmount", wh.c_str());
			c.perAmountA = pINI->ReadInteger(m.Name.c_str(), key, 0);
			_snprintf_s(key, sizeof(key), "Versus.%s.Mult", wh.c_str());
			c.multA = pINI->ReadDouble(m.Name.c_str(), key, 0.0);
			_snprintf_s(key, sizeof(key), "Versus.%s.MultMax", wh.c_str());
			c.multMaxA = pINI->ReadDouble(m.Name.c_str(), key, 1e9);

			// Tier C keyframes + rows
			_snprintf_s(key, sizeof(key), "Versus.%s.Keyframes", wh.c_str());
			ReadIntList(pINI, m.Name.c_str(), key, c.keyframes);
			for (int kf : c.keyframes)
			{
				_snprintf_s(key, sizeof(key), "Versus.%s.Row.%d", wh.c_str(), kf);
				std::vector<double> row;
				ReadDoubleList(pINI, m.Name.c_str(), key, row);
				c.rows.push_back(std::move(row));
			}
			_snprintf_s(key, sizeof(key), "Versus.%s.Interp", wh.c_str());
			if (pINI->ReadString(m.Name.c_str(), key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength)
				&& _strcmpi(WeatherExtDLL::readBuffer, "step") == 0)
				c.interpStep = true;

			m.versus.push_back(std::move(c));
		}
	}

	// Evaluate a curve's damage multiplier at the given meter amount + armor.
	double EvalVersus(const VersusCurve& c, int amount, int armor)
	{
		double factor = 1.0;

		// Tier A: uniform scalar.
		if (c.perAmountA > 0 && amount > 0)
		{
			const int steps = amount / c.perAmountA;
			double fA = 1.0 + c.multA * steps;
			if (fA > c.multMaxA) fA = c.multMaxA;
			if (fA < 0.0) fA = 0.0;
			factor *= fA;
		}

		// Tier C: interpolate the per-armor multiplier row at `amount`.
		if (!c.keyframes.empty())
		{
			auto rowVal = [&](size_t k) -> double {
				const auto& row = c.rows[k];
				return (armor >= 0 && armor < (int)row.size()) ? row[armor] : 1.0;
			};
			double fC;
			if (amount <= c.keyframes.front())
				fC = rowVal(0);
			else if (amount >= c.keyframes.back())
				fC = rowVal(c.keyframes.size() - 1);
			else
			{
				size_t k = 0;
				while (k + 1 < c.keyframes.size() && c.keyframes[k + 1] <= amount)
					++k;
				if (c.interpStep)
					fC = rowVal(k);
				else
				{
					const int lo = c.keyframes[k], hi = c.keyframes[k + 1];
					const double t = hi > lo ? double(amount - lo) / double(hi - lo) : 0.0;
					fC = rowVal(k) + (rowVal(k + 1) - rowVal(k)) * t;
				}
			}
			factor *= fC;
		}

		return factor;
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

		anySWContrib = false;
		for (auto& kv : SWContribMap)
		{
			resolve(kv.second.start, kv.first->ID);
			if (!kv.second.start.empty())
				anySWContrib = true;
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

		// Build the per-warhead versus lookup (resolve warhead names).
		WarheadVersusMap.clear();
		anyVersus = false;
		for (int mi = 0; mi < (int)Meters.size(); ++mi)
			for (auto& c : Meters[mi].versus)
			{
				c.wh = WarheadTypeClass::Find(c.whName.c_str());
				if (!c.wh)
				{
					Debug::Log("[WeatherExt] meter '%s' Versus.Warheads names "
						"unknown warhead '%s'; ignored.\n",
						Meters[mi].Name.c_str(), c.whName.c_str());
					continue;
				}
				WarheadVersusMap[c.wh].push_back({ mi, &c });
				anyVersus = true;
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
		ReadMeterVersus(pINI, m);

		// Every pass runs before gameplay, so seeding the start here means the
		// game begins at StartAmount (clamped) for the last pass that ran.
		m.Amount = ClampMeter(m, m.StartAmount);
		m.decayCounter = 0;
	}

	logCounter = 0;
	detLinesLeft = DET_LINE_BUDGET;
	fireLinesLeft = 40;
	pendingFinish.clear(); // new game / re-parse: drop any scheduled pulses
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

void Weather::ReadSuperWeaponType(SuperWeaponTypeClass* pSW, CCINIClass* pINI)
{
	if (!pSW)
		return;
	std::vector<Contrib> start;
	if (!ReadContribPair(pINI, pSW->ID, start))
		return; // absent this pass; keep any prior parse
	if (start.empty())
	{
		SWContribMap.erase(pSW);
		finalized = false;
		return;
	}

	SWContrib sc;
	sc.start = std::move(start);

	std::vector<int> finish;
	ReadIntList(pINI, pSW->ID, "WeatherSystem.Amounts.Finish", finish);
	if (!finish.empty())
	{
		sc.finish = std::move(finish);
		if (sc.finish.size() != sc.start.size())
			Debug::Log("[WeatherExt] [%s] WeatherSystem.Amounts.Finish has %zu "
				"entries but .Types has %zu; missing finishes default to 0.\n",
				pSW->ID, sc.finish.size(), sc.start.size());
	}

	// Interval: numeric frames. "auto" (SW's own duration) is designed but not
	// yet wired -- treat it as 0 for now and say so.
	if (pINI->ReadString(pSW->ID, "WeatherSystem.Interval", "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength) && *WeatherExtDLL::readBuffer)
	{
		if (_strcmpi(WeatherExtDLL::readBuffer, "auto") == 0)
		{
			Debug::Log("[WeatherExt] [%s] WeatherSystem.Interval=auto not yet "
				"implemented; using 0 (finish applied with start).\n", pSW->ID);
			sc.interval = 0;
		}
		else
			sc.interval = std::max(0, atoi(WeatherExtDLL::readBuffer));
	}

	SWContribMap[pSW] = std::move(sc);
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

void Weather::OnSuperWeaponFired(HouseClass* pHouse, int swSlotIndex)
{
	if (!Config.Enabled || !pHouse || !anySWContrib)
		return;
	if (!finalized)
		Finalize();

	SuperClass* pSuper = pHouse->Supers.GetItemOrDefault(swSlotIndex);
	if (!pSuper || !pSuper->Type)
		return;
	// Only count a launch that is actually ready to go -- filters spurious
	// Fire_SW calls. (This runs post-SuperWeaponExt-veto by chain order.)
	if (!pSuper->IsReady)
		return;

	auto it = SWContribMap.find(pSuper->Type);
	if (it == SWContribMap.end())
		return;
	const SWContrib& sc = it->second;

	for (size_t i = 0; i < sc.start.size(); ++i)
	{
		const int idx = sc.start[i].idx;
		if (idx < 0)
			continue;
		Meter& m = Meters[idx];

		// START delta, now.
		const int startDelta = Scaled(sc.start[i].amount);
		if (startDelta != 0)
			m.Amount = ClampMeter(m, m.Amount + startDelta);

		// FINISH delta: scaled at SCHEDULE time (a later WeatherScale change
		// can't retroactively desync a pending finish).
		if (i < sc.finish.size() && sc.finish[i] != 0)
		{
			const int finishDelta = Scaled(sc.finish[i]);
			if (sc.interval > 0)
				pendingFinish.push_back({ Unsorted::CurrentFrame + sc.interval, idx, finishDelta });
			else
				m.Amount = ClampMeter(m, m.Amount + finishDelta); // same-frame
		}
	}

	if (fireLinesLeft > 0)
	{
		--fireLinesLeft;
		Debug::Log("[WeatherExt] frame %d: SW %s fired by %s -> contributed "
			"(interval %d)\n", Unsorted::CurrentFrame, pSuper->Type->ID,
			pHouse->PlainName, sc.interval);
	}
}

int Weather::AdjustDamage(int damage, WarheadTypeClass* pWH, int armor)
{
	if (!Config.Enabled || !anyVersus || !pWH || damage == 0)
		return damage;
	if (!finalized)
		Finalize();

	auto it = WarheadVersusMap.find(pWH);
	if (it == WarheadVersusMap.end())
		return damage;

	double factor = 1.0;
	for (const auto& rv : it->second)
		factor *= EvalVersus(*rv.curve, Meters[rv.meterIdx].Amount, armor);

	if (factor == 1.0)
		return damage;
	return static_cast<int>(std::lround(damage * factor));
}

void Weather::FrameTick()
{
	if (!Config.Enabled)
		return;
	if (!finalized)
		Finalize();

	// --- drain due pending-finish pulses (fixed order, integer math) ---
	if (!pendingFinish.empty())
	{
		const int now = Unsorted::CurrentFrame;
		size_t w = 0;
		for (size_t r = 0; r < pendingFinish.size(); ++r)
		{
			const PendingFinish& p = pendingFinish[r];
			if (p.finishFrame <= now)
			{
				if (p.meterIdx >= 0 && p.meterIdx < (int)Meters.size() && p.amount != 0)
				{
					Meter& m = Meters[p.meterIdx];
					m.Amount = ClampMeter(m, m.Amount + p.amount);
				}
			}
			else
			{
				pendingFinish[w++] = p; // keep not-yet-due entries, order preserved
			}
		}
		pendingFinish.resize(w);
	}

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

// SuperWeaponTypeClass::LoadFromINI, the single SW-type parse funnel. EBP =
// type, [esp+0x3FC] = INI. Same layout/size as the Antares+Phobos hooks at this
// address (0xA, return 0); Antares' extra DEFINE_HOOK_AGAIN at 0x6CEE50 is
// disjoint from this 0xA range.
DEFINE_HOOK(0x6CEE43, SuperWeaponTypeClass_LoadFromINI_WeatherExt, 0xA)
{
	GET(SuperWeaponTypeClass*, pItem, EBP);
	GET_STACK(CCINIClass*, pINI, 0x3FC);
	Weather::ReadSuperWeaponType(pItem, pINI);
	return 0;
}

// HouseClass::Fire_SW entry (0x4FAE50) -- the universal launch funnel and the
// encyclopedia's designated seat for *recording* a launch. SuperWeaponExt's
// constraint veto also sits here and is injected earlier, so it runs first: if
// it denies (returns 0x4FAEF3, non-zero) the chain stops and we never record,
// giving "measure low" for our own inhibitor/designator layer for free. ECX =
// house, [esp+4] = SW slot index. Stolen 7 (push ebx; mov ebx,ecx; mov
// ecx,[esp+8]) -- matches SuperWeaponExt's size at this address. We return 0 so
// the launch (and Antares' downstream +0x22 checks) proceed normally.
DEFINE_HOOK(0x4FAE50, HouseClass_Fire_SW_WeatherExt, 0x7)
{
	GET(HouseClass*, pHouse, ECX);
	GET_STACK(int, idxSW, 0x4);
	Weather::OnSuperWeaponFired(pHouse, idxSW);
	return 0;
}

// MapClass::GetTotalDamage entry (0x489180) -- the universal damage-vs-armor
// funnel: __fastcall(int damage ECX, WarheadTypeClass* EDX, Armor [esp+4],
// int distance [esp+8]). Unhooked by frameworks at the entry (Phobos starts at
// +0x2F, Antares' Verses multiply at +0xB5), so we scale the INPUT damage and
// let Antares apply base Verses downstream -- our factor multiplies on top.
// Stolen 6 (sub esp,0xc; push esi; mov esi,ecx); the stub's `mov esi,ecx`
// re-reads our modified ECX. Hot path: AdjustDamage fast-returns unless a
// versus curve is configured for this warhead.
DEFINE_HOOK(0x489180, MapClass_GetTotalDamage_WeatherExt, 0x6)
{
	GET(int, damage, ECX);
	GET(WarheadTypeClass*, pWH, EDX);
	GET_STACK(int, armor, 0x4);
	const int adjusted = Weather::AdjustDamage(damage, pWH, armor);
	if (adjusted != damage)
		R->ECX(adjusted);
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
