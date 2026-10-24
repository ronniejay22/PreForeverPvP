# Rebrand inventory findings (2026-10-23)

From `tools/rebrand/scan.py` on CENTURION `0d282d54bc`. These become the rules
in `rules.toml`. Policy: **rename everything a player or admin can see; keep
code identifiers.** Identifiers are invisible, several are fixed by the prebuilt
`dinput8.dll`, and renaming the ones in `patch-Y` would force a 1.39 GB
re-download for every player.

## Rename

| where | what |
|---|---|
| `worldserver.conf(.dist)`, `playerbots.conf(.dist)`, C++ | config prefix `Centurion.` + capital → `<ServerKey>.` (about 230 keys; also in log and GM messages that quote them) |
| `worldserver.conf` | `Motd` (conf:1294), `PlayerStart.String` (conf:3413) |
| world DB | creature 900200 "Centurion Guard"; items 201448 and 203608-203624 (marks of honor); quests 26035, 26036, 60002 (`QuestDescription`); npc_text 16777218, 16777220; comments in `disables`, `conditions`, `areatrigger_teleport`, `item_loot_template` |
| auth DB | `realmlist.name` (**only at cutover**) |
| C++ log text | `custom_bounty.cpp`: "Centurion Guard(s)" in two log lines |
| `patch-enUS-A` | `GameTips.dbc` ×7 tips; `Spell.dbc` ×3 descriptions (server copy ×4); `CenturionNameTags.lua` panel name `"Centurion"` and `"Options for the Centurion client."`; realm check `CenturionWhisper.lua:17` (**cutover**) |
| `patch-enUS-6` | realm checks `CharacterSelect.lua:578`, `CharacterCreate.lua:1001` (literals `"centurion"`, `"centuriondev"`; **cutover**); text at `CharacterCreate.lua:1237`; the 3 logo BLPs |
| `addons.zip` | 20 `CENTURION_*` folders and their `.toc` file names; `.toc` Title, Notes and Author lines; string references to the folder names (e.g. `Interface\\AddOns\\CENTURION_Rift\\vo\\…`); `"Centurion Summoner's Rift"`; `"initializing default centurion keybinds"` |
| `clientedits/addons` | the same as `addons.zip` for the 8 folders with source here (plus the 2 GM-only ones) |
| launcher | see the plan, Phase 8 |
| docs | `README.md`, `AGENTS.md`, `doc/Stockades/AGENTS.md` (keep credits) |

Owner's decisions (2026-10-23): creatures 110206-110210 "Centurion Ogre Mage/Enforcer/Warlock/Mauler/Brute" **keep** their names ("Centurion" read as a rank); invisible code identifiers **keep** their names.

## Keep

- **Owner's choice:** creatures 110206-110210 ("Centurion Ogre Mage", "Centurion Enforcer", "Centurion Warlock", "Centurion Mauler", "Centurion Brute").
- **Blizzard content:** creatures 1036, 4301, 7346, 16319, 17465, 19510, 19792, 20048, 20140, 20583, 21522, 23337, 24411, 27563; item 10740 "Centurion Legplates"; quest, locale, `smart_scripts`, `creature_text` and `page_text` mentions; `Achievement.dbc` "Centurion" and `CharTitles.dbc` "Centurion %s" (Horde PvP rank); `mod_pvp_titles.cpp` "Centurion".
- **Code identifiers:** `CONFIG_CENTURION_*`, Lua globals (`CENTURION_WSG_AUTH`, `CenturionCTF_*`, `CenturionPVPToggles`, …), frame names, slash-command and binding identifiers, SavedVariables names.
- **Coupled to the prebuilt `dinput8.dll`:** `CenturionUnitBarsScan`, `CenturionUnitBar`, `CenturionTargetGUID`, `CenturionGlueRequest`, CVar `centurionNameTags`, the attest tag.
- **Must match C++ registrations:** ScriptNames `npc_centurion_guard`, `go_centurion_bounty_board`, `AddSC_centurion_*`.
- **Database table names:** `centurion_weekly_npc`, `centurion_pvp_match_duration`, `centurion_bounty_board_*`, …
- **Asset paths:** 472 files under `World\Centurion\…`, `World\wmo\Centurion\…`, `Sound\Music\Centurion\…`, and the `Interface\FrameXML\Centurion*.lua` file names.
- **History:** `sql/old` (stock TrinityCore updates), `auth.audit_dml_log`.

## Archives that change

`addons.zip`, `patch-enUS-6`, `patch-enUS-A` (and `patch-enUS-7` only if a
visible string turns up there). `patch-Y`, `patch-Z`, `patch-4`, `patch-N`,
`patch-dungeon-maps` and `client-tweaks` keep upstream's `.version`, so
players coming from Centurion don't download them again.
