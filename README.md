# WeatherExt

Standalone Syringe DLL for Yuri's Revenge: one synced global **Weather Level**
plus a modder-defined **WeatherTypes** system. Co-loads with Antares + Phobos.
See [DESIGN.md](DESIGN.md) for the full picture.

## Status: Phase 0 (probe)

What works:

- `[Weather]` globals in rulesmd (present-only-update, so gamemode/map INIs
  override individual keys): `Enabled`, `Initial`, `Baseline`, `Decay`,
  `DecayRate`, `Min`, `Max`, `LogInterval`.
- `Weather.Delta=` on any warhead adds to the level on every detonation
  (through the universal `MapClass::DamageArea` funnel).
- Per-frame decay toward `Baseline`, periodic level lines in `debug.log`,
  plus individual lines for the first 20 delta-carrying detonations.

No `[Weather]` section (or `Enabled=no`) leaves the DLL inert.

Known Phase 0 limitations (deliberate):

- No savegame persistence: loading a save does not restore the level.
- No WeatherTypes yet (that's Phase 1), no SW/passive contributors.

## Hooks

| Address | Size | Seat |
|---|---|---|
| `0x7CD810` / `0x7CD81E` | 0x9 / 0x6 | ExeRun (contested + fallback pair, idempotent) |
| `0x668F6A` | 0x5 | RulesClass::Read_File tail — parse `[Weather]`, reset level |
| `0x75DEA0` | 0x5 | WarheadTypeClass::LoadFromINI — parse `Weather.Delta` |
| `0x489286` | 0x6 | MapClass::DamageArea — apply detonation delta |
| `0x55B6B3` | 0x5 | LogicClass::AI post-loop — decay + logging (the ONE per-frame seat; add consumers inside it, never a second hook) |

All shared addresses were checked against the framework sources: every
co-hooked handler returns 0, so the Syringe chain always reaches us.

## Building

CI builds on push (`.github/workflows/build.yml`, DevBuild config, v142/x86).
Submodules are pinned to the same YRpp/Phobos commits as ScatterExt.

Remember: the DLL does nothing until it's in the Syringe `-i=` list in
`wine-game.sh` **and** ClientDefinitions.ini.
