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

## Phases

- **P0 — probe.** Global level + decay + `Weather.Delta` on warheads, value
  logged every N frames. Proves the tick seat, the detonation hook, savegame.
- **P1 — storm engine.** WeatherTypes bands, `SuperWeapons.*` auto-fire via
  `Launch`. This alone retires the clone banks + recharge/level-ladder files.
- **P2 — modifiers.** Cost / build speed / firepower / armor multipliers.
- **P3 — scoping.** Country ratio lists; PrerequisiteExt producer key;
  `SuperWeapons.LimitDelta.*`.
- **P4 — lobby diet.** Collapse the Game Options INI corpus to one small
  `[Weather]` override per dial.

## Open questions (defaults chosen, flag if wrong)

1. **Global vs per-house level** — defaulting to one shared global (matches
   "a global value"). Per-house weather is possible later but changes the
   effect plumbing.
2. **Firing house for auto-volleys** — defaulting to the Special/neutral house
   with a `SuperWeapons.Owner=` override. Matters for kill credit and Verses.
3. **RoF multiplier** — the mod's Game Options corpus has big `countriesRoF*`
   banks; adding `ROF.Multiplier=` alongside firepower is cheap if wanted.
