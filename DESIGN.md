# WeatherExt — a real weather system for Yuri's Revenge

Standalone Syringe DLL (co-loads with Antares, same toolchain as the other Ext
DLLs). One synced **global Weather Level** plus a modder-defined **WeatherTypes**
list replaces the current INI-explosion approach (~1,600 lobby-option files of
SW clone banks, LimboDelivery level ladders, and per-dial RechargeTime patches
in `INI/Game Options/`).

## What exists today (the system being absorbed)

The World Powers mod fakes an escalating storm with:

- **500 `MaxLevelX.ini` / 500 `LowestLevelX.ini`** — a "weather level" ladder
  built from Phobos `LimboDelivery` chains of dummy buildings
  (`WeatherStormStep001…`), one file per lobby dial position.
- **`WeatherStrikeSW_RechargeX.ini` (75 files)** — banks of cloned
  `[WeatherStrikeSW001…N]` sections whose `RechargeTime` is the storm frequency.
- **`LightningMaxX` / `LightningMinX` / `lightningstormsX` /
  `gradualLightningspaceX`** — which clones are enabled, initial-ready flags,
  and recharge for the `SKIRMRULESSPAWN3*` bank.
- **`StormDamageX.ini` (33 files)** — `[STRMTWRWH]` Verses variants.

All of that is one integer, three timers, and a handful of multipliers once a
DLL owns the state.

## Core model

**One global Weather Level** (signed int, synced sim state, saved/loaded).
Everything else reads or writes it:

```
contributors ──► Weather Level ──► active WeatherType(s) ──► effects
```

- Contributors push the level up or down (warhead hits, SW launches, passive
  emitters).
- A configurable per-frame **decay** drifts the level back toward a baseline.
- Each **WeatherType** declares a level band. When the level enters the band,
  that type becomes active and its effects run; leaving the band deactivates it.

Default is one shared global value. A per-house variant is noted under Open
Questions but is *not* Phase 1 — every effect listed below works off the global.

## INI schema (draft)

### Globals — `[Weather]` in rulesmd

```ini
[Weather]
Enabled=yes
Initial=0                 ; starting level
Baseline=0                ; decay drifts toward this
Decay=1                   ; level units removed per DecayRate frames
DecayRate=15              ; frames between decay ticks
Min=0
Max=10000
```

Lobby dials become *one small section override* (e.g. a Game Options file that
patches only `Initial=`, `Decay=`, or a `Sensitivity=` scalar) instead of 500
ladder files. Remember: set test flags in rulesmd, **not** spawn.ini — the
client regenerates spawn.ini at launch.

### Contributors — tags on existing sections

```ini
[NUKEWH]                  ; any warhead
Weather.Delta=150         ; added to the level on every detonation

[NukeSpecial]             ; any superweapon
Weather.Delta=800         ; added when the SW actually fires

[GAWEAT]                  ; any TechnoType (building/vehicle/etc.)
Weather.Rate=5            ; added every Weather.RateDelay frames while alive
Weather.RateDelay=60
Weather.Rate.RequiresPower=yes   ; buildings only: no contribution while offline
```

Demo trucks, nuclear reactors, weather generators all fall out of these three
tags — the demo truck contributes through its warhead, the reactor through a
passive `Weather.Rate`, the weatherstorm generator through either.

### WeatherTypes

```ini
[WeatherTypes]
0=ThunderSeason
1=Drought

[ThunderSeason]
Range=2000,6000           ; active while Min <= level <= Max

; --- auto-firing superweapons (replaces the WeatherStrikeSW clone banks) ---
SuperWeapons=WeatherStormSpecial
SuperWeapons.Count=3      ; launches per volley ("quantity fired at a time")
SuperWeapons.Frequency=450   ; frames between volleys
SuperWeapons.Limit=-1     ; max volleys per activation, -1 = unlimited
SuperWeapons.LimitDelta.Warheads=SOMEWH   ; "terms to alter the limit":
SuperWeapons.LimitDelta.Amounts=1         ;  these warheads add volleys back
SuperWeapons.Owner=neutral               ; neutral|random|<country> (fired-by house)

; --- global modifiers while active ---
Cost.Multiplier=1.25
BuildSpeed.Multiplier=0.8
Firepower.Multiplier=1.1
Armor.Multiplier=0.9

; --- per-country ratios (parallel lists, ratio scales the modifiers' distance from 1.0) ---
Countries=Russians,Alliance
Countries.Ratio=0.5,2.0   ; Russians feel half the effect, Alliance double

; --- prerequisite gating (WeatherExt = producer, PrerequisiteExt = consumer) ---
Prerequisites.Grant=WEATHERLAB   ; virtual prereq satisfied while active
Prerequisites.Deny=GAWEAP        ; these prereqs test false while active
```

Bands may overlap; every active type's effects apply (multipliers compose by
multiplication). Volley limits reset on re-entry into the band.

## Hook map

Consulted the YR Hook Encyclopedia (standing rule); findings go back into it as
each hook is claimed.

| Purpose | Seat | Status |
|---|---|---|
| Per-frame engine (decay, band transitions, volley timers, passive emitters) | `0x55B6B3` — LogicClass::AI just after the object loop, **uncontended** (only unmerged Phobos PR #352) | verified in encyclopedia |
| SW-fired contribution | `0x4FAE50` — Fire_SW entry, unhooked by frameworks; SuperWeaponExt sits at the same address → same-address chaining is safe (run the hook-overlap CI check anyway) | verified in encyclopedia |
| Warhead detonation contribution | DamageArea funnel, `0x4892xx` region (PDB-named `DamageArea_*` landmarks) | RE-VERIFY exact entry + register state |
| Auto-firing storms | call `SuperClass::Launch` (`0x6CC390`) directly, N times per volley — goes through Antares' dispatch, so Antares custom SW types fire correctly too | verified in encyclopedia |
| Build speed multiplier | GetBuildTime `0x6F47A0` is **fully Antares-replaced**; chain after Antares, and any epilogue jump must return `0x6F494D` explicitly (the `0x6F4955` ret sits in a nop bed) | known trap, documented |
| Cost multiplier | production cost getter funnel | RE-VERIFY (encyclopedia Production-Queues page first, then PDB) |
| Firepower / armor multipliers | per-shot damage & ReceiveDamage funnels | RE-VERIFY (prefer convergence points over Antares-owned sites — don't concede framework hooks) |
| Prereq gating | no gamemd hook: register as a PrerequisiteExt Requirement test key, exactly like SpawnExt's producer role | design-time integration |

Passive `Weather.Rate` emitters need **no per-object hook** — the `0x55B6B3`
pass walks the techno array on its own cadence.

Not touching: `LightningStorm_Start/Strike/Update` (`0x539EB0` etc.) — Ares and
Antares both hook them; we never need to, since volleys go through `Launch`.

## Sync & persistence rules

- The level is sim state: mutate it **only** from synced events (detonations,
  launches, the logic-frame tick). Any randomness uses `ScenarioClass::Random`.
- Save/load the level + per-type timers/volley counters via the savegame stream
  (encyclopedia `Savegame-Stream.md`).
- Echo all parsed `[Weather]` globals in the DLL log at rules load (INI
  last-line rule: never let a weather tag be the final line of a file).

## Phases (rev 2)

- **P0 — probe.** ✅ built + deployed. One global meter + decay +
  `Weather.Delta` on warheads, logged every N frames. Proved the tick seat, the
  detonation hook, INI parse. (The single global becomes the "default meter".)
- **P1 — named meters + contributors.** `[WeatherSystems]` registry; the
  `WeatherSystem.Types/.Amounts/.Rate/.RequiresPower` contributor pair on SWs,
  warheads, and technos. Multiple meters logged. Retires
  `StartingLevel`/`LowestLevel`/`MaxLevel` and the LimboDelivery ladder.
- **P2 — Effect 1: threshold SW firing.** `Fire.*` slots via `SuperClass::Launch`
  (inhibitors/designators free). Retires the recharge banks +
  `lightningstorms`/`gradualLightningspace`/`LightningMax`/`LightningMin`.
- **P3 — Effect 2: warhead vs armor.** `Versus.*` curve at the damage-apply
  seat, armor-by-name via `ArmorType_FindIndex`. Retires `StormDamage`.
- **P4 — broad modifiers.** Cost / build speed / firepower / armor multipliers
  as meter effects; country ratios; PrerequisiteExt producer key.
- **P5 — lobby diet.** Collapse the Game Options corpus to a few small keyed
  overrides per dial.

## Open questions (defaults chosen, flag if wrong)

1. **Global vs per-house level** — defaulting to one shared global (matches
   "a global value"). Per-house weather is possible later but changes the
   effect plumbing.
2. **Firing house for auto-volleys** — defaulting to the Special/neutral house
   with a `SuperWeapons.Owner=` override. Matters for kill credit and Verses.
3. **RoF multiplier** — the mod's Game Options corpus has big `countriesRoF*`
   banks; adding `ROF.Multiplier=` alongside firepower is cheap if wanted.

---

# Design rev 2 — named meters (supersedes the single-global model above)

The sections above describe **one** global level with reactive bands. Testing
Phase 0 and the SW/warhead discussion promoted this to the real model: **many
independently-named meters**, each with its own contributors and its own
threshold-tiered effects. Phase 0's single global is just the degenerate
one-meter case, so the P0 code still stands; this generalizes around it.

## The core reframing: a WeatherSystem is a named *meter*

A **WeatherSystem** is nothing but a named signed integer (`Amount`) that lives
in synced sim state, with:

- **contributors** that add to it (superweapons on fire, warheads on detonate,
  technos passively over time), and
- **effects** that switch on when the `Amount` crosses per-slot thresholds
  (fire superweapons, modulate warhead-vs-armor, and later: cost / build /
  firepower / prereqs).

That single abstraction is what deletes the 1,770 files. The old system faked a
"level" with **500** `LimboDelivery` ladder files spawning dummy step-buildings
(`WeatherStormStep001…`), plus 75+75 SW recharge banks, 500 `StartingLevel`
files, 500 `LowestLevel`, 500 `MaxLevel`, and 33 `StormDamage` Verses variants.
All of that is: one integer, one threshold table, and one Verses multiplier —
expressed once, driven by lobby overrides that patch a handful of keys.

**It is deliberately not weather-specific.** A meter that fires SWs and scales
damage can equally drive prices, spawn zombies (a spawn is just an SW), or grant
upgrades (via other Ext DLLs). "Weather" is the flagship use of a general
**global-meter** primitive. WeatherExt implements the meter plus the effects it
can reach; anything expressible as "fire this SW when the meter is high enough"
comes free.

## Meter declaration

```ini
[WeatherSystems]                 ; the registry (like [ArmorTypes], [Warheads])
0=StormCloudWeather
1=RadStorm
2=NuclearWinter

[StormCloudWeather]
StartAmount=0
Min=0
Max=100000
Baseline=0                       ; the meter drifts toward this...
Decay=1                          ; ...by this many units...
DecayRate=15                     ; ...every this many frames (0 = no decay)
```

## Contributors — one key pair, cadence set by context

Every contributor uses the **same two parallel lists**: which meters, and how
much. The difference between a superweapon and a reactor is *when* the delta is
applied, and that is implied by the section type — not by a different key.

```ini
; --- superweapon: delta applied once, each time it actually FIRES ---
[StormCloudSpecial]
WeatherSystem.Types=StormCloudWeather,RadStorm,HeatWave,CruiseStorm,MeteorFall
WeatherSystem.Amounts=1,24,-2,100,5      ; parallel to .Types

; --- warhead: delta applied once, each time it DETONATES ---
[NukeWH]
WeatherSystem.Types=NuclearWinter,RadStorm
WeatherSystem.Amounts=150,40

; --- techno (building/vehicle/infantry/aircraft): applied passively over time ---
[NANRCT]                          ; Soviet reactor
WeatherSystem.Types=NuclearWinter,RadRain,GlobalWarming,WorldPeace
WeatherSystem.Amounts=10,100,5,-1000
WeatherSystem.Rate=60             ; apply the Amounts once per 60 frames while alive
WeatherSystem.RequiresPower=yes   ; buildings: contribute only while powered/online
```

Design decisions baked in here, and why:

- **`.Amounts` everywhere** (renamed from the drafts' mixed `.Add` / `.Amount`).
  One key name across all three contributor kinds keeps the mental model flat.
- **Cadence is contextual, not a key.** SW = on-fire, warhead = on-detonate,
  techno = per `WeatherSystem.Rate` frames. A demo truck therefore contributes
  through *its warhead* automatically; a reactor through its *techno* rate; a
  weather generator can do either or both. No "mode" tag needed.
- **Passive technos need no per-object hook** — the once-per-frame pass at
  `0x55B6B3` walks `TechnoClass::Array` in index order (lockstep-safe) and bills
  each contributor on its own cadence.

## Effect 1 — threshold-gated superweapon firing

Cleaned up from the draft (fixed the `Modifer` typo, unified singular/plural,
and added the guardrails a stepping interval *must* have). Each column `i` is an
independent firing **slot**: it activates when `Amount ≥ Fire.Amounts[i]` and
then fires `Fire.Types[i]` every `Fire.Intervals[i]` frames, with the interval
shrinking as the meter climbs further past the threshold.

```ini
[StormCloudWeather]
Fire.Types=WeatherStrike,WeatherStrike,LightningStormSpecial
Fire.Amounts=15,25,200            ; activation threshold per slot
Fire.Counts=1,1,3                 ; SWs launched per volley per slot (default 1)
Fire.Intervals=5,100,500          ; base frames between volleys; OVERRIDES the SW's own RechargeTime
Fire.IntervalStep=5,-5,-10        ; add this to the interval per step of overshoot (draft: Interval.Modifer)
Fire.IntervalStepAmount=10,25,50  ; one step per this many Amount above the threshold (draft: Interval.Rate)
Fire.IntervalMin=1,15,30          ; REQUIRED floor — without it a negative step drives the interval to <=0 => fire-every-frame / div-by-zero
Fire.Owner=neutral                ; neutral|random|<country>; kill credit + Verses source
```

`effective_interval(i) = clamp(Fire.Intervals[i]
    + Fire.IntervalStep[i] * floor((Amount - Fire.Amounts[i]) / Fire.IntervalStepAmount[i]),
    Fire.IntervalMin[i], +inf)`

Two things I'm making non-optional because the draft would misbehave without
them:

1. **`Fire.IntervalMin` is mandatory.** The draft's `Interval.Modifer=-10`
   marches the interval toward zero and past it; a zero/negative interval means
   "fire every frame" (or a divide-by-zero in the cadence math). The floor is
   the safety rail. If omitted, WeatherExt defaults it to `max(1, RechargeTime)`
   and logs that it did.
2. **Firing goes through the real launch** (`SuperClass::Launch`, `0x6CC390`),
   *not* a raw effect. That is what makes **inhibitors and designators honored
   for free**: Antares owns `SW_Inhibitors` / `SW_Designators` /
   `SW_AnyInhibitor` / `SW_AnyDesignator` (verified in `Ext/SWType/Body.h`) and
   checks them on the firing path. A weather-fired `LightningStormSpecial` is
   suppressed by an enemy inhibitor in range and requires a designator in range,
   exactly as a player-fired one would — because it *is* one.

## Effect 2 — warhead vs armor, driven by the meter

The big realization from digging through Antares: **custom armor types already
exist.** Antares stores each warhead's `Verses` as a growable
`std::vector<VersesData>` indexed by a dynamic `ArmorType` registry
(`[ArmorTypes]`), resolved by name via `ArmorType_FindIndex` (`0x4753F0`). So
your `special_1 … special_22` are already real the moment they're listed in
Antares' `[ArmorTypes]`. WeatherExt must **not** invent an armor system; it
scales the damage the game already computes against those armors.

**Where it applies.** The base multiply `damage * GetVerses(armor)` happens at
`GetTotalDamage_Verses` (`0x489235`) — but Antares already owns those 8 bytes.
Rather than fight for that seat (see *don't concede framework hooks*), WeatherExt
applies its weather factor at the **per-object damage-apply seat** (the
`DamageArea` / `ReceiveDamage` convergence), where all three inputs are in hand:
the warhead, the victim (hence its `TechnoType->Armor` index), and the
already-computed damage. One multiply there = "the storm makes lightning bite
harder," with optional per-armor granularity.

Schema — put the modulation on the **meter**, not scattered across warheads, and
reference warheads and armors **by name** (positional lists are exactly the
unreadable trap we're escaping):

```ini
[StormCloudWeather]
Versus.Warheads=NukeWH,HE          ; which warheads this meter modulates

; -- Tier A (simple): one scalar for the whole warhead, grown by the meter --
Versus.NukeWH.PerAmount=100        ; one "step" per 100 Amount
Versus.NukeWH.Mult=0.10            ; +0.10x to ALL versus per step (linear, additive steps)
Versus.NukeWH.MultMax=3.0          ; clamp

; -- Tier B (opt-in, per armor): only for warheads that need surgical control --
Versus.HE.PerAmount=50
Versus.HE.Armors=none,light,heavy,special_1   ; names from Antares [ArmorTypes]
Versus.HE.Mult=0.0,0.05,0.20,0.50             ; per-armor multiplier added per step
Versus.HE.Add=0%,0%,10%,25%                    ; per-armor flat Verses added per step
Versus.HE.Clamp=0%,500%                         ; final per-armor floor/ceiling
```

Decisions and rationale:

- **Linear/additive steps, not compounding.** `factor = 1 + Mult * steps`
  (clamped), where `steps = floor((Amount - onset) / PerAmount)`. Multiplicative
  `Mult^steps` explodes non-intuitively; additive is what a designer can predict
  from the INI. Same stepping primitive as the SW interval curve — **one
  response-curve concept reused everywhere** (contributor → meter → interval →
  versus), which is the through-line that keeps the whole system learnable.
- **Reference by name.** `Versus.<WH>.Armors=` uses armor *names*; adding an
  armor type to `[ArmorTypes]` never silently shifts a column. This is the fix
  for the draft's `[0]`/positional forms.
- **Tier A covers the common case** (whole-warhead scaling); Tier B is the
  power-user escape hatch. A modder never has to touch per-armor lists to get
  "storm boosts nukes."
- This **subsumes the 33 `StormDamage` Verses files**: those were 33 frozen
  snapshots of one warhead's Verses row; here it's one curve the meter drives
  continuously.

## What each old file family becomes

| Old family (file count) | Replaced by |
|---|---|
| `StartingLevel*` (500) | `StartAmount=` (one key, lobby-overridable) |
| `LowestLevel*` (500) | `Min=` |
| `MaxLevel*` (500) | `Max=` |
| `lightningstorms*`, `gradualLightningspace*` (37) | `Fire.Intervals` / `Fire.IntervalStep*` |
| `LightningMax*` / `LightningMin*` (50) | `Fire.Amounts` slots (which tiers are active) |
| `DecimationSW_Recharge*`, `DeliverySW_Recharge*` (150) | `Fire.Intervals` (meter overrides SW recharge) |
| `StormDamage*` (33) | `Versus.*` curve on the meter |
| the `WeatherStormStep*` dummy buildings + LimboDelivery ladders | **deleted** — the meter is a real integer, not a stack of dummy buildings |

## Additional hooks this rev needs

Extends the Hook map above. Encyclopedia-first, findings returned after.

| Purpose | Seat | Status |
|---|---|---|
| SW-fired contribution (per-meter) | `0x4FAE50` Fire_SW entry, or the `SuperClass::Launch` `0x6CC390` path shared with the auto-fire — pick the one seat both use | RE-VERIFY which carries the firing house + SW type cleanly |
| Warhead-vs-armor weather factor | per-object damage-apply seat (`DamageArea`/`ReceiveDamage` convergence), **not** `0x489235` (Antares owns it) | RE-VERIFY exact seat + that victim `TechnoType->Armor` is readable there |
| Armor name → index | `ArmorType_FindIndex` `0x4753F0` (call it; don't reimplement the registry) | verified in PDB |

## Sync & persistence (rev 2)

- Every meter's `Amount`, plus per-slot fire timers and volley counters, is
  synced sim state — mutate only from synced events + the frame tick, save/load
  all of it via the savegame stream.
- Meters iterate in **registry order**; contributors iterate `TechnoClass::Array`
  in index order. No pointer-ordered iteration anywhere (desync trap).

## Open questions (rev 2)

1. **Fire cadence base unit** — `Fire.Intervals` in frames (engine-native, what
   the SW recharge really is) vs minutes (what the lobby files used). Leaning
   frames for precision, with a `Fire.IntervalsAreMinutes=yes` convenience flag
   for porting the old dials. Your call.
2. **Overshoot stepping direction** — should a *falling* meter also lengthen the
   interval back symmetrically (it does under the formula) or latch at the
   fastest reached until the slot deactivates? Symmetric is simpler and I'd
   default to it.
3. **Tier A vs Tier B precedence** — if both are declared for a warhead, apply
   Tier A then Tier B (broad then surgical), or make them mutually exclusive?
   Recommend: apply both, Tier A first.
4. **Meter visibility** — do players need to *see* the meter (a UI number / bar),
   or is it a felt, behind-the-scenes force? Affects whether P-later needs any
   render work.
