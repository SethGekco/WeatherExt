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
#include <cctype>    // tolower, isdigit
#include <climits>   // INT_MIN
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

	// A simple linear response curve: factor = clamp(1 + mult*(amount/perAmount),
	// 0, multMax). perAmount <= 0 disables it (factor stays 1).
	struct ScalarCurve
	{
		int perAmount = 0;
		double mult = 0.0;
		double multMax = 1e9;
	};

	double EvalScalar(const ScalarCurve& c, int amount)
	{
		if (c.perAmount <= 0 || amount <= 0)
			return 1.0;
		double f = 1.0 + c.mult * (double(amount) / c.perAmount);
		if (f > c.multMax) f = c.multMax;
		if (f < 0.0) f = 0.0;
		return f;
	}

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

		// global combat multipliers (P4): applied to ALL damage at the
		// GetTotalDamage seat. Firepower multiplies damage up; Armor divides it
		// (tankier world). Net global factor = firepower / armor.
		ScalarCurve firepower;
		ScalarCurve armor;
		bool hasGlobalDamage = false;

		// global cost multiplier (P4b): scales every house's build cost. >1 =
		// pricier war economy. Applied by re-driving HouseClass's own cost
		// recompute each frame and layering this factor on top.
		ScalarCurve cost;
		bool hasCost = false;

		// motion (rev 4): the climb/decline ladder. Direction +1 climb,
		// -1 decline, 0 stable; StartMotion 2 = random (rolled at start).
		struct MotionDir
		{
			int step = 10;
			int every = 900;          // frames per step
			int spreadLo = 0, spreadHi = 0;
			bool hasTo = false;       // false => edge is Max (climb) / Min (decline)
			int to = 0;
			int atEnd = 0;            // 0 reverse, 1 hold, 2 wrap, 3 stop
			int pause = 0;
		};
		int StartMotion = 0;
		int MotionDelay = 0;
		MotionDir climb, decline;
		int dir = 0;                  // runtime
		int motionTimer = 0;
		bool motionStarted = false;
		bool edgePending = false;
	};

	std::vector<Meter> Meters;
	std::unordered_map<std::string, int> MeterIndex; // name -> Meters[] index

	// Armor name -> Verses column index, mirroring the engine's order: the 11
	// built-ins (0-10) then [ArmorTypes] keys in file order. Lets the modder
	// write Versus rows by armor NAME; we resolve to the positional row the
	// damage hook actually indexes. Names are stored lower-cased (engine armor
	// lookups are case-insensitive). Accumulated across INI passes, never wiped.
	std::unordered_map<std::string, int> ArmorNameIndex;

	std::string Lower(const char* s)
	{
		std::string r(s ? s : "");
		for (auto& ch : r) ch = (char)tolower((unsigned char)ch);
		return r;
	}

	void BuildArmorNames(CCINIClass* pINI)
	{
		if (ArmorNameIndex.empty())
		{
			const char* base[] = { "none","flak","plate","light","medium","heavy",
				"wood","steel","concrete","special_1","special_2" };
			for (int i = 0; i < 11; ++i)
				ArmorNameIndex[base[i]] = i;
		}
		// Append custom [ArmorTypes] keys in file order (FindOrAllocate-style:
		// a key already known keeps its index).
		const int n = pINI->GetKeyCount("ArmorTypes");
		for (int i = 0; i < n; ++i)
		{
			const char* key = pINI->GetKeyName("ArmorTypes", i);
			if (!key || !*key)
				continue;
			std::string k = Lower(key);
			if (ArmorNameIndex.find(k) == ArmorNameIndex.end())
			{
				const int next = (int)ArmorNameIndex.size(); // read before insert
				ArmorNameIndex[k] = next;
			}
		}
	}

	// ---- contributors (stored by NAME, resolved lazily) -------------------
	// Name-based storage makes parse ORDER irrelevant: a contributor can name a
	// meter that hasn't been read yet. Finalize() resolves names to indices
	// once, after all INI passes, before the first tick/detonation.
	struct Contrib
	{
		std::string meter;
		int amount = 0;
		int motion = 0; // MotionCmd (0 = keep); steers the meter's motion
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
	bool anyGlobalDamage = false;
	bool anyCostEffect = false;
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

	// ---- time values (rev 4) ----------------------------------------------
	// A bare number is frames; a suffix converts: s = 15 frames, m = 900 frames
	// (the engine's RechargeTime scale), f = frames. Returned in centi-frames
	// (frames x 100) so small per-Weight shaves accumulate exactly.
	// `unitHint` is the multiplier to use when the value carries no suffix.
	long long ParseTimeCenti(const char* s, double unitHint = 1.0, double* unitOut = nullptr)
	{
		if (!s)
			return 0;
		while (*s == ' ') ++s;
		char* end = nullptr;
		const double v = strtod(s, &end);
		while (end && *end == ' ') ++end;
		double unit = unitHint;
		if (end && *end)
		{
			switch (*end)
			{
			case 'm': case 'M': unit = 900.0; break;
			case 's': case 'S': unit = 15.0; break;
			case 'f': case 'F': unit = 1.0; break;
			default: break;
			}
		}
		if (unitOut)
			*unitOut = unit;
		return std::llround(v * unit * 100.0);
	}

	int CentiToFrames(long long c)
	{
		return static_cast<int>(c >= 0 ? (c + 50) / 100 : -((-c + 50) / 100));
	}

	int ParseTimeFrames(const char* s)
	{
		return CentiToFrames(ParseTimeCenti(s));
	}

	// Present-only time key in frames.
	int ReadTime(CCINIClass* pINI, const char* section, const char* key, int current)
	{
		if (!pINI->ReadString(section, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength)
			|| !*WeatherExtDLL::readBuffer)
			return current;
		return ParseTimeFrames(WeatherExtDLL::readBuffer);
	}

	void ReadTimeList(CCINIClass* pINI, const char* section, const char* key,
		std::vector<int>& out)
	{
		out.clear();
		if (!pINI->ReadString(section, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength))
			return;
		char* ctx = nullptr;
		for (char* tok = strtok_s(WeatherExtDLL::readBuffer, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx))
			out.push_back(ParseTimeFrames(tok));
	}

	// "lo,hi" time pair. A unit on the LAST value only applies to both, so
	// "0.5,2s" means 0.5s..2s. Returns frames; hi is forced >= lo.
	void ParseTimePair(const char* s, int& lo, int& hi)
	{
		lo = hi = 0;
		if (!s || !*s)
			return;
		std::string str(s);
		const size_t comma = str.find(',');
		if (comma == std::string::npos)
		{
			lo = 0;
			hi = ParseTimeFrames(str.c_str());
		}
		else
		{
			double unit = 1.0;
			const long long h = ParseTimeCenti(str.c_str() + comma + 1, 1.0, &unit);
			const std::string first = str.substr(0, comma);
			// does the first value carry its own suffix?
			bool ownUnit = false;
			for (char ch : first)
				if (ch == 'm' || ch == 'M' || ch == 's' || ch == 'S' || ch == 'f' || ch == 'F')
					ownUnit = true;
			lo = CentiToFrames(ParseTimeCenti(first.c_str(), ownUnit ? 1.0 : unit));
			hi = CentiToFrames(h);
		}
		if (hi < lo)
			std::swap(lo, hi);
	}

	// ---- motion (rev 4) ---------------------------------------------------
	// Contributor command that steers a meter's climb/decline motion.
	enum class MotionCmd { Keep, Climb, Decline, Stable, Reverse };

	MotionCmd ParseMotionCmd(const char* v)
	{
		if (!v) return MotionCmd::Keep;
		if (_strcmpi(v, "climb") == 0) return MotionCmd::Climb;
		if (_strcmpi(v, "decline") == 0) return MotionCmd::Decline;
		if (_strcmpi(v, "stable") == 0) return MotionCmd::Stable;
		if (_strcmpi(v, "reverse") == 0) return MotionCmd::Reverse;
		return MotionCmd::Keep;
	}

	// Build the [warhead/type] contributor list from the parallel .Types /
	// .Amounts pair. Present-only: if NEITHER key is present, keep whatever was
	// parsed on an earlier pass; if either is present, this pass defines it.
	bool ReadContribPair(CCINIClass* pINI, const char* section, std::vector<Contrib>& out)
	{
		std::vector<std::string> types;
		std::vector<int> amounts;
		std::vector<std::string> motions;
		ReadNameList(pINI, section, "WeatherSystem.Types", types);
		ReadIntList(pINI, section, "WeatherSystem.Amounts", amounts);
		ReadNameList(pINI, section, "WeatherSystem.Motion", motions);

		if (types.empty() && amounts.empty())
			return false; // absent this pass; caller keeps prior value

		out.clear();
		for (size_t i = 0; i < types.size(); ++i)
		{
			Contrib c;
			c.meter = types[i];
			c.amount = i < amounts.size() ? amounts[i] : 0;
			c.motion = i < motions.size() ? (int)ParseMotionCmd(motions[i].c_str()) : 0;
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
		ReadTimeList(pINI, m.Name.c_str(), "Fire.Intervals", intervals);
		ReadTimeList(pINI, m.Name.c_str(), "Fire.IntervalStep", steps);
		ReadIntList(pINI, m.Name.c_str(), "Fire.IntervalStepAmount", stepAmounts);
		ReadTimeList(pINI, m.Name.c_str(), "Fire.IntervalMin", mins);

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

			// Tier C keyframes + rows. Rows may be authored BY NAME: if
			// Versus.<WH>.Armors= lists armor names, each Row.<kf> is parallel
			// to that list (sparse -- unlisted armors stay at x1). Without
			// Armors=, a Row is a full positional row in Verses order.
			_snprintf_s(key, sizeof(key), "Versus.%s.Keyframes", wh.c_str());
			ReadIntList(pINI, m.Name.c_str(), key, c.keyframes);

			std::vector<int> cols; // armor index per row column; empty => positional
			_snprintf_s(key, sizeof(key), "Versus.%s.Armors", wh.c_str());
			std::vector<std::string> armorNames;
			ReadNameList(pINI, m.Name.c_str(), key, armorNames);
			for (auto& an : armorNames)
			{
				auto ai = ArmorNameIndex.find(Lower(an.c_str()));
				if (ai == ArmorNameIndex.end())
				{
					Debug::Log("[WeatherExt] [%s] Versus.%s.Armors: unknown armor "
						"'%s'; that column is ignored.\n", m.Name.c_str(), wh.c_str(), an.c_str());
					cols.push_back(-1);
				}
				else
					cols.push_back(ai->second);
			}

			for (int kf : c.keyframes)
			{
				_snprintf_s(key, sizeof(key), "Versus.%s.Row.%d", wh.c_str(), kf);
				std::vector<double> raw;
				ReadDoubleList(pINI, m.Name.c_str(), key, raw);

				if (cols.empty())
				{
					// positional: the row IS the full armor-indexed row
					c.rows.push_back(std::move(raw));
				}
				else
				{
					// by-name: scatter raw[j] into a positional row at cols[j],
					// defaulting every other armor to x1.
					int maxIdx = 10; // always cover the 11 built-ins
					for (int ci : cols) if (ci > maxIdx) maxIdx = ci;
					std::vector<double> row(maxIdx + 1, 1.0);
					for (size_t j = 0; j < cols.size() && j < raw.size(); ++j)
						if (cols[j] >= 0)
							row[cols[j]] = raw[j];
					c.rows.push_back(std::move(row));
				}
			}
			_snprintf_s(key, sizeof(key), "Versus.%s.Interp", wh.c_str());
			if (pINI->ReadString(m.Name.c_str(), key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength)
				&& _strcmpi(WeatherExtDLL::readBuffer, "step") == 0)
				c.interpStep = true;

			m.versus.push_back(std::move(c));
		}
	}

	// Parse the global combat multipliers for one meter.
	void ReadMeterModifiers(CCINIClass* pINI, Meter& m)
	{
		const char* s = m.Name.c_str();
		m.firepower.perAmount = pINI->ReadInteger(s, "Firepower.PerAmount", m.firepower.perAmount);
		m.firepower.mult = pINI->ReadDouble(s, "Firepower.Mult", m.firepower.mult);
		m.firepower.multMax = pINI->ReadDouble(s, "Firepower.MultMax", m.firepower.multMax);
		m.armor.perAmount = pINI->ReadInteger(s, "Armor.PerAmount", m.armor.perAmount);
		m.armor.mult = pINI->ReadDouble(s, "Armor.Mult", m.armor.mult);
		m.armor.multMax = pINI->ReadDouble(s, "Armor.MultMax", m.armor.multMax);
		m.hasGlobalDamage = (m.firepower.perAmount > 0 && m.firepower.mult != 0.0)
			|| (m.armor.perAmount > 0 && m.armor.mult != 0.0);

		m.cost.perAmount = pINI->ReadInteger(s, "Cost.PerAmount", m.cost.perAmount);
		m.cost.mult = pINI->ReadDouble(s, "Cost.Mult", m.cost.mult);
		m.cost.multMax = pINI->ReadDouble(s, "Cost.MultMax", m.cost.multMax);
		m.hasCost = (m.cost.perAmount > 0 && m.cost.mult != 0.0);
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
	int versusLinesLeft = 20; // per-game damage-adjust log budget

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
		if (!pHouse || swIndex < 0 || swIndex >= pHouse->Supers.Count)
			return false;
		// A house holds one SuperClass per SuperWeaponTypeClass, in type-array
		// order, so the TYPE index is the Supers index. (FindSuperWeaponIndex is
		// NOT that lookup: it matches the built-in kind, Type->Type, so a custom
		// SW's array index resolved to nothing or to another SW of that kind.)
		const int idx = swIndex;
		SuperClass* pSuper = pHouse->Supers.GetItem(idx);
		if (!pSuper || pSuper->Type != SuperWeaponTypeClass::Array.GetItem(swIndex))
			return false;
		if (grant)
			pSuper->Grant(false, false, false); // permanent, silent, not on-hold
		pSuper->SetReadiness(true);             // WeatherExt owns cadence
		pHouse->Fire_SW(idx, cell);             // veto layers decide from here
		return true;
	}

	// ---- meter motion (rev 4) ---------------------------------------------
	int MotionLinesLeft = 40;

	int StepTime(const Meter& m, int dir)
	{
		const auto& d = dir > 0 ? m.climb : m.decline;
		int t = d.every;
		if (d.spreadHi > d.spreadLo)
			t += ScenarioClass::Instance->Random.RandomRanged(d.spreadLo, d.spreadHi);
		else
			t += d.spreadLo;
		return std::max(1, t);
	}

	void ApplyMotionCmd(Meter& m, int cmd)
	{
		int nd = m.dir;
		switch ((MotionCmd)cmd)
		{
		case MotionCmd::Climb: nd = 1; break;
		case MotionCmd::Decline: nd = -1; break;
		case MotionCmd::Stable: nd = 0; break;
		case MotionCmd::Reverse:
			if (m.dir == 0)
				return; // nothing to reverse; keep any scheduled start
			nd = -m.dir;
			break;
		default:
			return;
		}
		if (nd == m.dir && m.motionStarted)
			return;
		m.dir = nd;
		m.motionStarted = true;
		m.edgePending = false;
		if (nd)
			m.motionTimer = StepTime(m, nd);
		if (MotionLinesLeft > 0)
		{
			--MotionLinesLeft;
			Debug::Log("[WeatherExt] frame %d: '%s' motion set to %s by a contributor\n",
				Unsorted::CurrentFrame, m.Name.c_str(), nd > 0 ? "climb" : nd < 0 ? "decline" : "stable");
		}
	}

	void TickMotion(Meter& m)
	{
		if (!m.motionStarted)
		{
			if (m.StartMotion == 0 || Unsorted::CurrentFrame < m.MotionDelay)
				return;
			m.motionStarted = true;
			m.dir = m.StartMotion == 2
				? (ScenarioClass::Instance->Random.RandomRanged(0, 1) ? 1 : -1)
				: m.StartMotion;
			m.motionTimer = StepTime(m, m.dir);
			return;
		}
		if (m.dir == 0 || --m.motionTimer > 0)
			return;

		const auto& d = m.dir > 0 ? m.climb : m.decline;
		const int edge = d.hasTo ? d.to : (m.dir > 0 ? m.Max : m.Min);
		auto atEdge = [&]() { return m.dir > 0 ? m.Amount >= edge : m.Amount <= edge; };

		if (m.edgePending)
		{
			m.edgePending = false;
			const char* what = "hold";
			switch (d.atEnd)
			{
			case 0: m.dir = -m.dir; what = "reverse"; break;
			case 2:
			{
				const auto& o = m.dir > 0 ? m.decline : m.climb;
				const int other = o.hasTo ? o.to : (m.dir > 0 ? m.Min : m.Max);
				m.Amount = ClampMeter(m, other);
				what = "wrap";
				break;
			}
			case 3: m.dir = 0; what = "stop"; break;
			default: break;
			}
			if (MotionLinesLeft > 0 && d.atEnd != 1)
			{
				--MotionLinesLeft;
				Debug::Log("[WeatherExt] frame %d: '%s' reached %d -> %s (now %s, amount %d)\n",
					Unsorted::CurrentFrame, m.Name.c_str(), edge, what,
					m.dir > 0 ? "climb" : m.dir < 0 ? "decline" : "stable", m.Amount);
			}
			if (m.dir)
				m.motionTimer = StepTime(m, m.dir);
			return;
		}

		if (!atEdge())
		{
			int next = m.Amount + m.dir * d.step;
			next = m.dir > 0 ? std::min(next, edge) : std::max(next, edge);
			m.Amount = ClampMeter(m, next);
		}
		if (atEdge())
		{
			m.edgePending = true;
			m.motionTimer = std::max(1, d.pause);
			return;
		}
		m.motionTimer = StepTime(m, m.dir);
	}

	// ---- strike pools (rev 4) ---------------------------------------------
	const char* const POOL_REGISTRY = "WeatherStrikes";
	constexpr int MAX_POOLS = 64;
	constexpr int MAX_POOL_SLOTS = 2000;

	enum class Pattern { Global, Even, Climb, Decline };
	enum class PatternSwitch { Never, Every, Random, Motion, Saturated };
	const char* PatternName(Pattern p)
	{
		switch (p)
		{
		case Pattern::Even: return "even";
		case Pattern::Climb: return "climb";
		case Pattern::Decline: return "decline";
		default: return "global";
		}
	}

	// One complete parameter set. A pool holds one per Weight band.
	struct PoolParams
	{
		int perWeight = 10;
		int slotsMin = 0;
		int slotsMax = 500;
		int delay = 0;
		bool newReady = true;
		bool removeNewest = true;

		long long rMaxC = 900LL * 10 * 100; // 10m, centi-frames
		long long rMinC = 0;                // defaults to rMaxC unless set
		long long shaveC = 0;               // centi-frames per shavePer Weight
		int shavePer = 1;
		int shaveFrom = 0;
		int spreadLo = 0, spreadHi = 0;     // frames
		std::vector<Pattern> patterns{ Pattern::Global };
		PatternSwitch patternSwitch = PatternSwitch::Never;
		int patternEvery = 900 * 5;
		bool live = false;

		std::vector<std::string> swNames;
		std::vector<int> swIdx;
		std::vector<int> chances;
		int count = 1;
		int countAdd = 0, countAddPer = 1;
		int countMax = 0;                   // 0 => no cap (hard ceiling 100)
		int gap = 0;
		int perFrame = 2;
		TargetMode target = TargetMode::Random;
		std::string owner = "neutral";
		bool grant = true;
	};

	// Lower-cased key -> value, merged across INI passes (later pass wins), so
	// every pool key is present-only and a lobby file needs only what it changes.
	using RawMap = std::unordered_map<std::string, std::string>;

	struct Pool
	{
		std::string Name;
		RawMap raw;
		std::vector<std::string> sysNames;
		std::vector<double> factors;
		std::vector<int> sysIdx;
		std::vector<int> floors;            // ascending; floors[0] = INT_MIN
		std::vector<PoolParams> bands;      // parallel to floors

		// runtime
		std::vector<int> timers;            // frames until each slot fires
		std::vector<int> totals;            // each slot's current base reload (Live)
		int patternPos = 0;
		int patternTimer = 0;
		bool wasSaturated = false;
		int gapTimer = 0;
		int cursor = 0;
		int shots = 0;
		int weight = 0;
		int band = 0;
	};
	std::vector<Pool> Pools;
	bool anyPools = false;

	std::vector<std::string> SplitList(const std::string& s)
	{
		std::vector<std::string> out;
		size_t i = 0;
		while (i <= s.size())
		{
			size_t j = s.find(',', i);
			if (j == std::string::npos) j = s.size();
			size_t a = i, b = j;
			while (a < b && s[a] == ' ') ++a;
			while (b > a && s[b - 1] == ' ') --b;
			out.push_back(s.substr(a, b - a));
			i = j + 1;
		}
		return out;
	}

	bool ParseBool(const std::string& v, bool current)
	{
		if (v.empty()) return current;
		switch (v[0])
		{
		case '1': case 't': case 'T': case 'y': case 'Y': return true;
		case '0': case 'f': case 'F': case 'n': case 'N': return false;
		default: return current;
		}
	}

	// Numeric last segment ("strike.sw.3000") -> base + floor. Returns false
	// for plain keys.
	bool SplitBand(const std::string& key, std::string& base, int& floor)
	{
		const size_t dot = key.rfind('.');
		if (dot == std::string::npos || dot + 1 >= key.size())
			return false;
		size_t k = dot + 1;
		if (key[k] == '-') ++k;
		if (k >= key.size())
			return false;
		for (size_t q = k; q < key.size(); ++q)
			if (!isdigit((unsigned char)key[q]))
				return false;
		base = key.substr(0, dot);
		floor = atoi(key.c_str() + dot + 1);
		return true;
	}

	// Apply every pool key found under `suffix` ("" = plain, ".3000" = band).
	void ApplyPoolKeys(PoolParams& p, bool& minSet, const RawMap& raw,
		const std::string& suffix, const char* pool)
	{
		auto get = [&](const char* base) -> const std::string* {
			auto it = raw.find(Lower(base) + suffix);
			return (it != raw.end() && !it->second.empty()) ? &it->second : nullptr;
		};
		const std::string* v;

		if ((v = get("Slots.PerWeight"))) p.perWeight = std::max(1, atoi(v->c_str()));
		if ((v = get("Slots.Min"))) p.slotsMin = std::max(0, atoi(v->c_str()));
		if ((v = get("Slots.Max"))) p.slotsMax = std::clamp(atoi(v->c_str()), 0, MAX_POOL_SLOTS);
		if ((v = get("Slots.Delay"))) p.delay = ParseTimeFrames(v->c_str());
		if ((v = get("Slots.NewReady"))) p.newReady = ParseBool(*v, p.newReady);
		if ((v = get("Slots.Remove"))) p.removeNewest = _strcmpi(v->c_str(), "oldest") != 0;

		if ((v = get("Recharge.Max"))) p.rMaxC = std::max(100LL, ParseTimeCenti(v->c_str()));
		if ((v = get("Recharge.Min"))) { p.rMinC = std::max(100LL, ParseTimeCenti(v->c_str())); minSet = true; }
		if ((v = get("Recharge.Shave")))
		{
			auto parts = SplitList(*v);
			p.shaveC = std::max(0LL, ParseTimeCenti(parts[0].c_str()));
			p.shavePer = parts.size() > 1 ? std::max(1, atoi(parts[1].c_str())) : 1;
		}
		if ((v = get("Recharge.ShaveFrom"))) p.shaveFrom = atoi(v->c_str());
		if ((v = get("Recharge.Spread"))) ParseTimePair(v->c_str(), p.spreadLo, p.spreadHi);
		if ((v = get("Recharge.Pattern")))
		{
			p.patterns.clear();
			for (auto& n : SplitList(*v))
			{
				if (_strcmpi(n.c_str(), "global") == 0) p.patterns.push_back(Pattern::Global);
				else if (_strcmpi(n.c_str(), "even") == 0) p.patterns.push_back(Pattern::Even);
				else if (_strcmpi(n.c_str(), "climb") == 0) p.patterns.push_back(Pattern::Climb);
				else if (_strcmpi(n.c_str(), "decline") == 0) p.patterns.push_back(Pattern::Decline);
				else if (!n.empty())
					Debug::Log("[WeatherExt] pool '%s': unknown Recharge.Pattern '%s' "
						"(global|even|climb|decline); skipped.\n", pool, n.c_str());
			}
			if (p.patterns.empty())
				p.patterns.push_back(Pattern::Global);
		}
		if ((v = get("Recharge.Pattern.Switch")))
		{
			const char* s = v->c_str();
			if (_strcmpi(s, "every") == 0) p.patternSwitch = PatternSwitch::Every;
			else if (_strcmpi(s, "random") == 0) p.patternSwitch = PatternSwitch::Random;
			else if (_strcmpi(s, "motion") == 0) p.patternSwitch = PatternSwitch::Motion;
			else if (_strcmpi(s, "saturated") == 0) p.patternSwitch = PatternSwitch::Saturated;
			else p.patternSwitch = PatternSwitch::Never;
		}
		if ((v = get("Recharge.Pattern.Every"))) p.patternEvery = std::max(1, ParseTimeFrames(v->c_str()));
		if ((v = get("Recharge.Live"))) p.live = ParseBool(*v, p.live);

		if ((v = get("Strike.SW")))
		{
			p.swNames.clear();
			for (auto& n : SplitList(*v))
				if (!n.empty())
					p.swNames.push_back(n);
		}
		if ((v = get("Strike.SW.Chances")))
		{
			p.chances.clear();
			for (auto& n : SplitList(*v))
				p.chances.push_back(std::max(0, atoi(n.c_str())));
		}
		if ((v = get("Strike.Count"))) p.count = std::max(0, atoi(v->c_str()));
		if ((v = get("Strike.Count.Add")))
		{
			auto parts = SplitList(*v);
			p.countAdd = atoi(parts[0].c_str());
			p.countAddPer = parts.size() > 1 ? std::max(1, atoi(parts[1].c_str())) : 1;
		}
		if ((v = get("Strike.Count.Max"))) p.countMax = std::max(0, atoi(v->c_str()));
		if ((v = get("Strike.Gap"))) p.gap = std::max(0, ParseTimeFrames(v->c_str()));
		if ((v = get("Strike.PerFrame"))) p.perFrame = std::max(1, atoi(v->c_str()));
		if ((v = get("Strike.Target")))
			p.target = _strcmpi(v->c_str(), "mapcenter") == 0 ? TargetMode::MapCenter : TargetMode::Random;
		if ((v = get("Strike.Owner"))) p.owner = *v;
		if ((v = get("Strike.Grant"))) p.grant = ParseBool(*v, p.grant);
	}

	// Rebuild a pool's band table from its merged raw keys.
	void BuildPoolBands(Pool& pool)
	{
		std::vector<int> floors;
		for (auto& kv : pool.raw)
		{
			std::string base;
			int f;
			if (SplitBand(kv.first, base, f))
				floors.push_back(f);
		}
		std::sort(floors.begin(), floors.end());
		floors.erase(std::unique(floors.begin(), floors.end()), floors.end());

		pool.sysNames.clear();
		pool.factors.clear();
		auto it = pool.raw.find("weight.systems");
		if (it != pool.raw.end())
			for (auto& n : SplitList(it->second))
				if (!n.empty())
					pool.sysNames.push_back(n);
		it = pool.raw.find("weight.factors");
		if (it != pool.raw.end())
			for (auto& n : SplitList(it->second))
				pool.factors.push_back(n.empty() ? 1.0 : atof(n.c_str()));
		pool.factors.resize(pool.sysNames.size(), 1.0);

		auto fixed = [&](PoolParams p, bool minSet) {
			if (!minSet)
				p.rMinC = p.rMaxC;
			if (p.rMinC > p.rMaxC)
			{
				Debug::Log("[WeatherExt] pool '%s': Recharge.Min > Recharge.Max; "
					"using Max for both.\n", pool.Name.c_str());
				p.rMinC = p.rMaxC;
			}
			if (p.slotsMax < p.slotsMin)
				p.slotsMax = p.slotsMin;
			return p;
		};

		PoolParams cur;
		bool minSet = false;
		ApplyPoolKeys(cur, minSet, pool.raw, "", pool.Name.c_str());
		pool.floors = { INT_MIN };
		pool.bands = { fixed(cur, minSet) };
		for (int f : floors)
		{
			ApplyPoolKeys(cur, minSet, pool.raw, "." + std::to_string(f), pool.Name.c_str());
			pool.floors.push_back(f);
			pool.bands.push_back(fixed(cur, minSet));
		}
	}

	void ResetPoolRuntime(Pool& p)
	{
		p.timers.clear();
		p.totals.clear();
		p.patternPos = 0;
		p.patternTimer = 0;
		p.wasSaturated = false;
		p.gapTimer = 0;
		p.cursor = 0;
		p.shots = 0;
		p.weight = 0;
		p.band = 0;
	}

	int PoolWeight(const Pool& p)
	{
		double w = 0.0;
		for (size_t i = 0; i < p.sysIdx.size(); ++i)
			if (p.sysIdx[i] >= 0)
				w += p.factors[i] * Meters[p.sysIdx[i]].Amount;
		return static_cast<int>(std::lround(w));
	}

	long long ShaveBudget(const PoolParams& P, int w)
	{
		if (P.shaveC <= 0 || w <= P.shaveFrom)
			return 0;
		return P.shaveC * (long long)(w - P.shaveFrom) / P.shavePer;
	}

	bool PatternSaturated(const PoolParams& P, Pattern pat, int w, int n)
	{
		const long long R = P.rMaxC - P.rMinC;
		if (R <= 0 || n <= 0)
			return true;
		const long long B = ShaveBudget(P, w);
		switch (pat)
		{
		case Pattern::Global: return B >= R;
		case Pattern::Even: return B / n >= R;
		default: return B >= R * n;
		}
	}

	// Base reload (frames, no spread) for slot i of n under `pat`.
	int SlotBase(const PoolParams& P, Pattern pat, int w, int i, int n)
	{
		const long long R = std::max(0LL, P.rMaxC - P.rMinC);
		const long long B = ShaveBudget(P, w);
		long long s = 0;
		switch (pat)
		{
		case Pattern::Global: s = std::min(B, R); break;
		case Pattern::Even: s = n > 0 ? std::min(B / n, R) : 0; break;
		case Pattern::Climb: s = std::clamp(B - (long long)i * R, 0LL, R); break;
		case Pattern::Decline: s = std::clamp(B - (long long)(n - 1 - i) * R, 0LL, R); break;
		}
		return std::max(1, CentiToFrames(P.rMaxC - s));
	}

	int RollSpread(const PoolParams& P)
	{
		if (P.spreadHi > P.spreadLo)
			return ScenarioClass::Instance->Random.RandomRanged(P.spreadLo, P.spreadHi);
		return P.spreadLo;
	}

	Pattern ActivePattern(Pool& pool, const PoolParams& P, int w, int n)
	{
		const int np = (int)P.patterns.size();
		if (np == 0)
			return Pattern::Global;
		switch (P.patternSwitch)
		{
		case PatternSwitch::Every:
			if (++pool.patternTimer >= P.patternEvery)
			{
				pool.patternTimer = 0;
				pool.patternPos = (pool.patternPos + 1) % np;
			}
			break;
		case PatternSwitch::Random:
			if (++pool.patternTimer >= P.patternEvery)
			{
				pool.patternTimer = 0;
				pool.patternPos = ScenarioClass::Instance->Random.RandomRanged(0, np - 1);
			}
			break;
		case PatternSwitch::Motion:
		{
			const int d = (!pool.sysIdx.empty() && pool.sysIdx[0] >= 0) ? Meters[pool.sysIdx[0]].dir : 0;
			return d > 0 ? P.patterns[0] : d < 0 ? P.patterns[1 % np] : (np >= 3 ? P.patterns[2] : P.patterns[0]);
		}
		case PatternSwitch::Saturated:
		{
			const bool sat = PatternSaturated(P, P.patterns[pool.patternPos % np], w, n);
			if (sat && !pool.wasSaturated)
				pool.patternPos = (pool.patternPos + 1) % np;
			pool.wasSaturated = sat;
			break;
		}
		default:
			return P.patterns[0];
		}
		return P.patterns[pool.patternPos % np];
	}

	int PickPoolSW(const PoolParams& P)
	{
		const int n = (int)P.swIdx.size();
		if (n == 0)
			return -1;
		if (n == 1)
			return P.swIdx[0];
		int total = 0;
		for (int i = 0; i < n; ++i)
			total += i < (int)P.chances.size() ? P.chances[i] : 1;
		if (total <= 0)
			return P.swIdx[0];
		int roll = ScenarioClass::Instance->Random.RandomRanged(0, total - 1);
		for (int i = 0; i < n; ++i)
		{
			roll -= i < (int)P.chances.size() ? P.chances[i] : 1;
			if (roll < 0)
				return P.swIdx[i];
		}
		return P.swIdx[n - 1];
	}

	// Fire one shot (Strike.Count SWs) for the pool. Returns SWs launched.
	int FirePoolShot(const PoolParams& P, int w)
	{
		long long count = P.count;
		if (P.countAdd != 0 && w > 0)
			count += (long long)(w / P.countAddPer) * P.countAdd;
		count = std::clamp(count, 0LL, (long long)(P.countMax > 0 ? P.countMax : 100));

		const bool eachHuman = _strcmpi(P.owner.c_str(), "each-human") == 0;
		const bool eachPlayer = _strcmpi(P.owner.c_str(), "each-player") == 0;
		int launched = 0;
		for (int k = 0; k < (int)count; ++k)
		{
			const int sw = PickPoolSW(P);
			if (sw < 0)
				continue;
			if (eachHuman || eachPlayer)
			{
				for (int i = 0; i < HouseClass::Array.Count; ++i)
				{
					HouseClass* pH = HouseClass::Array.Items[i];
					if (!pH || pH->Defeated || pH->IsNeutral() || pH->IsObserver())
						continue;
					if (eachHuman && !pH->IsHumanPlayer)
						continue;
					if (FireOneSW(pH, sw, P.grant, PickTargetCell(P.target)))
						++launched;
				}
			}
			else if (FireOneSW(ResolveFiringHouse(P.owner), sw, P.grant, PickTargetCell(P.target)))
				++launched;
		}
		return launched;
	}

	void TickPool(Pool& pool)
	{
		const int w = PoolWeight(pool);
		pool.weight = w;
		int b = 0;
		while (b + 1 < (int)pool.floors.size() && pool.floors[b + 1] <= w)
			++b;
		pool.band = b;
		const PoolParams& P = pool.bands[b];
		if (Unsorted::CurrentFrame < P.delay)
			return;

		int n = w > 0 ? w / P.perWeight : 0;
		n = std::clamp(n, P.slotsMin, P.slotsMax);
		const Pattern pat = ActivePattern(pool, P, w, n);

		// resize: grow with fresh slots, shrink from the newest (or oldest) end
		while ((int)pool.timers.size() > n)
		{
			if (P.removeNewest)
			{
				pool.timers.pop_back();
				pool.totals.pop_back();
			}
			else
			{
				pool.timers.erase(pool.timers.begin());
				pool.totals.erase(pool.totals.begin());
			}
		}
		while ((int)pool.timers.size() < n)
		{
			const int i = (int)pool.timers.size();
			const int base = SlotBase(P, pat, w, i, n);
			pool.totals.push_back(base);
			pool.timers.push_back(P.newReady ? 0 : base + RollSpread(P));
		}
		if (n == 0)
			return;
		if (pool.cursor >= n)
			pool.cursor = 0;

		// Live: running reloads rescale to the new target
		if (P.live)
			for (int i = 0; i < n; ++i)
			{
				const int nb = SlotBase(P, pat, w, i, n);
				if (pool.totals[i] > 0 && nb != pool.totals[i] && pool.timers[i] > 0)
					pool.timers[i] = (int)((long long)pool.timers[i] * nb / pool.totals[i]);
				pool.totals[i] = nb;
			}

		for (int& t : pool.timers)
			if (t > 0)
				--t;
		if (pool.gapTimer > 0)
			--pool.gapTimer;

		int shotsNow = 0;
		int last = -1;
		for (int k = 0; k < n; ++k)
		{
			if (shotsNow >= P.perFrame || pool.gapTimer > 0)
				break;
			const int i = (pool.cursor + k) % n;
			if (pool.timers[i] > 0)
				continue;
			const int launched = FirePoolShot(P, w);
			const int base = SlotBase(P, pat, w, i, n);
			pool.totals[i] = base;
			pool.timers[i] = base + RollSpread(P);
			pool.gapTimer = P.gap;
			++shotsNow;
			++pool.shots;
			last = i;
			if (fireLinesLeft > 0)
			{
				--fireLinesLeft;
				Debug::Log("[WeatherExt] frame %d: pool '%s' weight=%d slot %d/%d fired x%d "
					"(%s, next in %df)\n", Unsorted::CurrentFrame, pool.Name.c_str(), w,
					i + 1, n, launched, PatternName(pat), pool.timers[i]);
			}
		}
		if (last >= 0)
			pool.cursor = (last + 1) % n;
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

		// Resolve strike pools: meter names per pool, SW names per band.
		anyPools = false;
		for (auto& pool : Pools)
		{
			pool.sysIdx.clear();
			for (auto& n : pool.sysNames)
			{
				auto it = MeterIndex.find(n);
				pool.sysIdx.push_back(it != MeterIndex.end() ? it->second : -1);
				if (it == MeterIndex.end())
					Debug::Log("[WeatherExt] pool '%s' Weight.Systems names unknown "
						"weather system '%s'; ignored.\n", pool.Name.c_str(), n.c_str());
			}
			bool anySW = false;
			for (auto& P : pool.bands)
			{
				P.swIdx.clear();
				for (auto& n : P.swNames)
				{
					const int idx = SuperWeaponTypeClass::FindIndex(n.c_str());
					if (idx < 0)
						Debug::Log("[WeatherExt] pool '%s' Strike.SW names unknown "
							"superweapon '%s'; skipped.\n", pool.Name.c_str(), n.c_str());
					else
					{
						P.swIdx.push_back(idx);
						anySW = true;
					}
				}
			}
			if (pool.sysNames.empty())
				Debug::Log("[WeatherExt] pool '%s' has no Weight.Systems; it never fires.\n",
					pool.Name.c_str());
			if (anySW && !pool.sysNames.empty())
				anyPools = true;
		}

		anyGlobalDamage = false;
		anyCostEffect = false;
		for (auto& m : Meters)
		{
			if (m.hasGlobalDamage)
				anyGlobalDamage = true;
			if (m.hasCost)
				anyCostEffect = true;
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

	// Build the armor-name -> index map first so Versus rows can resolve names.
	BuildArmorNames(pINI);

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
		m.DecayRate = std::max(1, ReadTime(pINI, s, "DecayRate", m.DecayRate));
		if (m.Max < m.Min)
		{
			Debug::Log("[WeatherExt] [%s] Max=%d < Min=%d; swapping.\n", s, m.Max, m.Min);
			std::swap(m.Min, m.Max);
		}
		ReadFireSlots(pINI, m);
		ReadMeterVersus(pINI, m);
		ReadMeterModifiers(pINI, m);

		// motion (rev 4), present-only
		if (pINI->ReadString(s, "Motion", "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength) && *WeatherExtDLL::readBuffer)
		{
			const char* v = WeatherExtDLL::readBuffer;
			if (_strcmpi(v, "climb") == 0) m.StartMotion = 1;
			else if (_strcmpi(v, "decline") == 0) m.StartMotion = -1;
			else if (_strcmpi(v, "random") == 0) m.StartMotion = 2;
			else m.StartMotion = 0;
		}
		m.MotionDelay = std::max(0, ReadTime(pINI, s, "Motion.Delay", m.MotionDelay));
		auto readDir = [&](const char* prefix, Meter::MotionDir& d)
		{
			char key[64];
			_snprintf_s(key, sizeof(key), "%s.Step", prefix);
			d.step = std::max(0, pINI->ReadInteger(s, key, d.step));
			_snprintf_s(key, sizeof(key), "%s.Every", prefix);
			d.every = std::max(1, ReadTime(pINI, s, key, d.every));
			_snprintf_s(key, sizeof(key), "%s.Spread", prefix);
			if (pINI->ReadString(s, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength) && *WeatherExtDLL::readBuffer)
				ParseTimePair(WeatherExtDLL::readBuffer, d.spreadLo, d.spreadHi);
			_snprintf_s(key, sizeof(key), "%s.To", prefix);
			if (pINI->ReadString(s, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength) && *WeatherExtDLL::readBuffer)
			{
				d.hasTo = true;
				d.to = atoi(WeatherExtDLL::readBuffer);
			}
			_snprintf_s(key, sizeof(key), "%s.AtEnd", prefix);
			if (pINI->ReadString(s, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength) && *WeatherExtDLL::readBuffer)
			{
				const char* v = WeatherExtDLL::readBuffer;
				d.atEnd = _strcmpi(v, "hold") == 0 ? 1 : _strcmpi(v, "wrap") == 0 ? 2
					: _strcmpi(v, "stop") == 0 ? 3 : 0;
			}
			_snprintf_s(key, sizeof(key), "%s.Pause", prefix);
			d.pause = std::max(0, ReadTime(pINI, s, key, d.pause));
		};
		readDir("Climb", m.climb);
		readDir("Decline", m.decline);

		// Every pass runs before gameplay, so seeding the start here means the
		// game begins at StartAmount (clamped) for the last pass that ran.
		m.Amount = ClampMeter(m, m.StartAmount);
		m.decayCounter = 0;
		m.dir = 0;
		m.motionTimer = 0;
		m.motionStarted = false;
		m.edgePending = false;
	}

	// Strike pools (rev 4): registry present-only, each pool's keys merged
	// into its raw map so later passes (gamemode, map, lobby files) only need
	// the keys they change.
	{
		std::vector<std::string> poolNames;
		char key[16];
		for (int i = 0; i < MAX_POOLS; ++i)
		{
			_itoa_s(i, key, 10);
			if (!pINI->ReadString(POOL_REGISTRY, key, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength)
				|| !*WeatherExtDLL::readBuffer)
				break;
			poolNames.emplace_back(WeatherExtDLL::readBuffer);
		}
		if (!poolNames.empty())
		{
			std::unordered_map<std::string, RawMap> prior;
			for (auto& p : Pools)
				prior[p.Name] = std::move(p.raw);
			Pools.clear();
			for (auto& n : poolNames)
			{
				Pool p;
				p.Name = n;
				auto it = prior.find(n);
				if (it != prior.end())
					p.raw = std::move(it->second);
				Pools.push_back(std::move(p));
			}
		}
		for (auto& p : Pools)
		{
			const int nk = pINI->GetKeyCount(p.Name.c_str());
			for (int i = 0; i < nk; ++i)
			{
				const char* k = pINI->GetKeyName(p.Name.c_str(), i);
				if (!k || !*k)
					continue;
				std::string lk = Lower(k);
				pINI->ReadString(p.Name.c_str(), k, "", WeatherExtDLL::readBuffer, WeatherExtDLL::readLength);
				// normalise a band suffix ("x.03000" -> "x.3000") so lookups match
				std::string base;
				int f;
				if (SplitBand(lk, base, f))
					lk = base + "." + std::to_string(f);
				p.raw[lk] = WeatherExtDLL::readBuffer;
			}
			BuildPoolBands(p);
			ResetPoolRuntime(p);
		}
	}
	MotionLinesLeft = 40;

	logCounter = 0;
	detLinesLeft = DET_LINE_BUDGET;
	fireLinesLeft = 40;
	versusLinesLeft = 20;
	pendingFinish.clear(); // new game / re-parse: drop any scheduled pulses
	finalized = false; // force re-resolve of contributors against the new registry

	if (hadSection || c.Enabled || !Meters.empty())
	{
		Debug::Log("[WeatherExt] globals: Enabled=%s LogInterval=%d WeatherScale=%.3f "
			"meters=%zu\n", c.Enabled ? "yes" : "no", c.LogInterval, c.WeatherScale,
			Meters.size());
		for (auto& m : Meters)
			Debug::Log("[WeatherExt]   meter '%s': start=%d range=[%d,%d] "
				"baseline=%d decay=%d/%df fireSlots=%zu owner=%s motion=%d "
				"(delay %df, climb %d/%df, decline %d/%df)\n",
				m.Name.c_str(), m.StartAmount, m.Min, m.Max, m.Baseline,
				m.Decay, m.DecayRate, m.fireSlots.size(), m.owner.c_str(),
				m.StartMotion, m.MotionDelay, m.climb.step, m.climb.every,
				m.decline.step, m.decline.every);
		for (auto& p : Pools)
		{
			const PoolParams& P = p.bands.front();
			Debug::Log("[WeatherExt]   pool '%s': systems=%zu bands=%zu perWeight=%d "
				"slots=[%d,%d] recharge=[%d,%d]f shave=%lld/%d spread=[%d,%d]f "
				"patterns=%zu sw=%zu owner=%s\n", p.Name.c_str(), p.sysNames.size(),
				p.bands.size(), P.perWeight, P.slotsMin, P.slotsMax,
				CentiToFrames(P.rMinC), CentiToFrames(P.rMaxC), P.shaveC, P.shavePer,
				P.spreadLo, P.spreadHi, P.patterns.size(), P.swNames.size(), P.owner.c_str());
		}
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
		it->second.rate = std::max(0, ReadTime(pINI, pType->ID, "WeatherSystem.Rate", it->second.rate));
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
	tc.rate = std::max(0, ReadTime(pINI, pType->ID, "WeatherSystem.Rate", 0));
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
			sc.interval = std::max(0, ParseTimeFrames(WeatherExtDLL::readBuffer));
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
		if (c.motion)
			ApplyMotionCmd(m, c.motion);
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
		if (sc.start[i].motion)
			ApplyMotionCmd(m, sc.start[i].motion);

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
	if (!Config.Enabled || damage == 0 || (!anyVersus && !anyGlobalDamage))
		return damage;
	if (!finalized)
		Finalize();

	double factor = 1.0;

	// Global combat multipliers (every damage event): firepower up, armor down.
	if (anyGlobalDamage)
		for (const auto& m : Meters)
		{
			if (!m.hasGlobalDamage)
				continue;
			factor *= EvalScalar(m.firepower, m.Amount);
			const double a = EvalScalar(m.armor, m.Amount);
			if (a > 0.0)
				factor /= a;
		}

	// Per-warhead Versus curves.
	if (pWH)
	{
		auto it = WarheadVersusMap.find(pWH);
		if (it != WarheadVersusMap.end())
			for (const auto& rv : it->second)
				factor *= EvalVersus(*rv.curve, Meters[rv.meterIdx].Amount, armor);
	}

	if (factor == 1.0)
		return damage;
	const int adjusted = static_cast<int>(std::lround(damage * factor));
	if (versusLinesLeft > 0)
	{
		--versusLinesLeft;
		Debug::Log("[WeatherExt] frame %d: %s vs armor %d damage %d -> %d (x%.2f)\n",
			Unsorted::CurrentFrame, pWH->ID, armor, damage, adjusted, factor);
	}
	return adjusted;
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
				if (c.motion)
					ApplyMotionCmd(m, c.motion);
				const int delta = Scaled(c.amount);
				if (delta != 0)
					m.Amount = ClampMeter(m, m.Amount + delta);
			}
		}
	}

	// --- meter motion (rev 4): the climb/decline ladder ---
	for (auto& m : Meters)
		TickMotion(m);

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

	// --- Effect: strike pools (rev 4) ---
	if (anyPools)
		for (auto& pool : Pools)
			TickPool(pool);

	// --- Effect: global cost multiplier (house-field approach) ---
	// Re-drive each house's own cost recompute (HouseClass::CalculateCostMultipliers,
	// 0x50BF60 -- which the game only runs on factory-plant events) so we get the
	// fresh FactoryPlant base, then layer the weather factor on top. Idempotent
	// and self-reverting: when the meters relax, W -> 1 and cost returns to base.
	if (anyCostEffect)
	{
		double W = 1.0;
		for (const auto& m : Meters)
			if (m.hasCost)
				W *= EvalScalar(m.cost, m.Amount);

		const float fw = (float)W;
		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			HouseClass* pH = HouseClass::Array.Items[i];
			if (!pH)
				continue;
			pH->CalculateCostMultipliers(); // reset to the FactoryPlant base
			pH->CostInfantryMult *= fw;
			pH->CostUnitsMult *= fw;
			pH->CostAircraftMult *= fw;
			pH->CostBuildingsMult *= fw;
			pH->CostDefensesMult *= fw;
		}
	}

	// --- periodic logging ---
	if (Config.LogInterval > 0 && ++logCounter >= Config.LogInterval)
	{
		logCounter = 0;
		for (auto& m : Meters)
			Debug::Log("[WeatherExt] frame %d: '%s' = %d (%s)\n",
				Unsorted::CurrentFrame, m.Name.c_str(), m.Amount,
				m.dir > 0 ? "climb" : m.dir < 0 ? "decline" : "stable");
		for (auto& p : Pools)
			Debug::Log("[WeatherExt] frame %d: pool '%s' weight=%d band=%d slots=%zu shots=%d\n",
				Unsorted::CurrentFrame, p.Name.c_str(), p.weight, p.band,
				p.timers.size(), p.shots);
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
