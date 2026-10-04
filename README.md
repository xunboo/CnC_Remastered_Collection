# AI-Boost 4.0 for Red Alert

**Source update:** 4 October 2026

**Base mod:** AI-Boost 3.2b, released 3 November 2022

This repository contains the AI-Boost Red Alert mod, its configuration and Workshop assets, and the accompanying Command & Conquer Remastered game and map-editor source. The latest code update improves skirmish AI decisions, repairs a terrain-rendering startup crash, and adds reproducible build and regression scripts.

## Main updates in AI-Boost 4.0

This summary is based on [64c7037](https://github.com/xunboo/CnC_Remastered_Collection/commit/64c70377b7311bc00eb4aee4d8453d8f94aed361), compared with its parent [f1f0d42](https://github.com/xunboo/CnC_Remastered_Collection/commit/f1f0d42bc2dcd06d5d1df943c6150ab34bf307ae). [View the complete code difference](https://github.com/xunboo/CnC_Remastered_Collection/compare/f1f0d42bc2dcd06d5d1df943c6150ab34bf307ae...64c70377b7311bc00eb4aee4d8453d8f94aed361).

| Area | Main changes |
| --- | --- |
| Adaptive unit production | Production responds to enemy armor, infantry, aircraft, and defensive buildings. Anti-air and siege units receive appropriate weighting, while support and explosive vehicles remain a limited part of the fighting force. |
| Economy and base recovery | Replacing harvesters and recovering construction capability takes priority over the mod's combat-unit cap. Essential power, the first refinery, and initial anti-air defenses can recover before optional base-size limits block them. Air-rush bases can build the helipads they need. |
| Strategy selection | A finite list of feasible strategies replaces a selection loop that could stall. Naval access, available technology, and ground forces affect the choices. In automatic mode, infantry, air, and naval rushes can transition to a mixed strategy when opponents develop counters. |
| Enemy information and production limits | Threat snapshots refresh aircraft, ground, and naval information from living opponents, excluding allies and neutral houses. Players with units but no buildings still count. Infantry budgets stay local to each AI house, naval multipliers and bonuses apply to fleet limits, and disabled production categories keep their zero caps. Mechanic and Shock Trooper counts use their actual infantry types. |
| Attack waves and Chrono Tanks | Attack waves preserve harvesting, MCV expansion, existing teams, aircraft rearming, and ongoing attacks. Chrono Tanks choose living enemy buildings by strategic value and distance, check legal landing cells, and restore attack orders after teleporting. |
| Shared target selection | Nearby threat scanning retains the best score across all four scan edges, so a weaker later candidate cannot replace a stronger target. This shared targeting correction also affects human-owned units. |
| Terrain-rendering startup repair | Fourteen ghost, fading, predator, and combined drawing routines now subtract immediate code addresses in MASM. The previous operands read instruction bytes and could jump into the middle of an instruction, causing illegal-instruction or invalid-access crashes during map rendering. |
| Build and regression tooling | The Red Alert build uses the v143 toolset and an isolated output folder. Scripts discover the installed tools, normalize Windows environment-variable names, and run AI and actual-assembly renderer regressions. |

The AI changes are implemented in [HOUSE.CPP](REDALERT/HOUSE.CPP) and [AISTRATEGY.H](REDALERT/AISTRATEGY.H). Shared targeting is in [TECHNO.CPP](REDALERT/TECHNO.CPP), and the renderer repair is in [KEYFBUFF.ASM](REDALERT/KEYFBUFF.ASM).

### AI-Boost content included in this import

The same commit also brings the existing AI-Boost 3.2b mod into this repository:

- Configurable production and income boosts, attack timing, primary-factory production, naval/air strategies, MCV base expansion, and AI special-ability handling.
- Attack Move, configurable instant Engineer capture, harvester memory/optimization, and optional CFE veterancy. The included mod configuration sets `EnableVeterancy=0`.
- The mod configuration, metadata, graphics/XML/audio assets, and supporting ground, naval, and production test maps under [WorkshopContent](WorkshopContent) and [Other/test-maps](Other/test-maps).

## Build the updated RedAlert DLL

The validated target is **Release / Win32 (x86)**. Install:

- Visual Studio C++ build tools with the **v143** toolset.
- Windows SDK **10.0.26100.0**, which is selected by [RedAlert.vcxproj](REDALERT/RedAlert.vcxproj). If using another SDK version, retarget the project before building.
- **PowerShell 7** to run the build script.
- **Python 3** to run the regression scripts.

From the repository root, run:

```powershell
./SCRIPTS/Build-RedAlert.ps1
```

The script writes `RedAlert.dll`, matching debug symbols, intermediate objects, and `build.log` under `build/redalert/`. It builds the Red Alert project directly.

## Run the regression checks

```powershell
python SCRIPTS/test_redalert_ai.py
python SCRIPTS/test_redalert_renderer.py
```

After building, you can also test the renderer object actually linked into that DLL:

```powershell
python SCRIPTS/test_redalert_renderer.py --object build/redalert/obj/KEYFBUFF.obj
```

Validation recorded for the code update:

- **Release/Win32 build succeeded** with the v143 toolset and configured Windows SDK.
- **60 AI scenarios passed:** 29 production-policy scenarios, 8 target-selection scenarios, and 23 HouseClass decision scenarios. The fixtures compile selected game functions directly with controlled world data.
- **16,164 renderer scenarios passed:** the actual Win32 assembly is compared with expected pixels across sprite-width remainders, drawing flags, clipping, pitch, transparency, shadow/fading effects, and destination memory guards. The final DLL's linked renderer object passed the same checks.

The AI checks passed before the assembly repair; their source was unchanged by that repair. These checks validate the tested decision and rendering behavior. Full in-game startup, human-opponent win rates, campaign progression, save/load, and multiplayer synchronization still require playtesting.

## Install and configure the current build

1. Close Red Alert and its instance-server process.
2. Back up the active AIBoost mod's `Data/RedAlert.dll`.
3. Copy `build/redalert/RedAlert.dll` into that `Data` folder as `RedAlert.dll`. Copy the repository's `WorkshopContent/AIBoost/ccmod.json` to the active mod root to display the AI-Boost 4.0 name and description.
4. Restart the game, enable AIBoost, and test a stock skirmish map. Use Normal game speed for the initial startup check.

The local mod package is [WorkshopContent/AIBoost](WorkshopContent/AIBoost). For the original subscribed mod, the usual installation path is:

```text
C:\Program Files (x86)\Steam\steamapps\workshop\content\1213210\2221741447\AIBoost
```

AI settings are in [CCDATA/AIBOOST.INI](WorkshopContent/AIBoost/CCDATA/AIBOOST.INI) inside the active mod. The included configuration uses `AIStrategyMode=1`, `AIAttackFirstTime=5`, and `AIAttackInterval=5`.

- `AIStrategyMode=0` enables automatic strategy selection and counter-driven transitions.
- `AIStrategyMode=2` allows proactive naval production where the base has legal naval placement. An infeasible requested strategy falls back to the mixed dynamic mode.
- Attack timing depends on the simulation and game speed. The remaining strategy modes and limits are documented in the INI comments.

Build and install this repository's DLL to use these source changes. The historical Workshop update instructions below describe the original 2022 release.

## Repository contents and exclusions

The repository includes source, tests, configuration, and mod assets. [The ignore rules](.gitignore) exclude `bin`, `build`, `review`, `Changelogs`, generated Visual Studio output/debug files, and Python caches. Local build logs, review backups, and generated symbols stay outside commits.

See [LICENSE.md](LICENSE.md) and [License.txt](License.txt) for the GPLv3 license and additional terms.

## Original AI-Boost 3.2b notes - November 2022

The release notes and acknowledgements below describe the base mod imported by the code update.

Please follow the steps below to update and reactivate the mod:
- Disable the mod in the mods Menu
- Unsubscribe from the mod in the in-game Workshop mods menu
- Quit and restart the game
- Re-subscribe to the mod via the Workshop mods menu
- Activate the mod and restart the game as prompted
- The updated mod should then work as intended

---

Latest changes:
3.2b:
Fix:
- Added missing content for veterancy

3.2:
New:
- AI no longer builds with all factories at the same time. It's using primary factory now and can switch between them. (Like Humans)
  (This also gives a huge performance boost for the engine that fixes the cannot build issue for human in late games!!!)
- New strategy mode, default dynamic where AI don't wait human to build naval

Fixed/Changed/Improved:
- Fixed hack for more aggressive AI(human had it too). Now it's just the AI
- There was a bug in the dynamic strategy mode, sometimes it selected an strategy thats not possible on the map. Should be fixed now.
- Sometimes AI don't build barracks or base defenses, should be fixed now
- Fixed dynamic calculations. Sometimes, AI didn't see the right amount of bases

---

Included mod's:
- CFE Patch Redux 1.8 mod(Veterancy mod(default disabled), vanilla/multiplayer/performance fix)
- The Rampastring Quality of Live mod
- Engineer instant capture
- Additional Zoom Levels+++
- RA Immersive Heli's mod (And all Air units have a sight) (Use 'L' to land)
- Attack-Move mod (Use Shift)
- Aftermath fast build mod (Use .ini to enable/disable, default disabled)
- ChronoKillCargo set to False. Chronoshifting APCs or transports will not kill any passengers or cargo
- Added the option to remove shroud from the map

Own mod's:
- Added some performance features, the game runs smoother now
- Tech stealing by capturing a building with an Engineer (Allies vs Soviets)
- Sight range of the MCV is increased, this was needed to get the AI to find a deploy spot
- Now it's possible to add more starting cash and units! Editable by the .ini file
- You can now remove toggle with space between old/new graphic in gameconstant.xml
- Added extra zoom levels to the mod: "Additional Zoom Levels"
- Harvester memory option and made editable by .ini file

AI changes:
- AI handles his cash flow and building priorities/limits a lot smarter
- AI customizable limits for number of AI buildings, tanks, etc. using the .ini file
- AI customizable aggressivness using the .ini file
- AI scatters all units on new attack launch to unlock stuck/blocked units
- AI understands importance of buildings & units a lot better and uses it during attack
- AI attacks interval uses different ways (Hunt, Closest enemy, etc.)
- AI Harvester prio switcher. There is a 5% change of AI goes wild on harvester hunting. The rest of the time it gives harvesters lower prio
- AI Air units no longer just attack Refineries, it selects random between base-defense, factories, power, buildings, naval units or all threats
- AI knows how to use Thief's, Spies, MAD Tanks, Demo truck and chrono shifting with a Chrono Tank
- AI knows how to use Naval stuff
- AI Naval war detection. The AI scans for human Naval objects, if he detects Naval stuff, it will upscale his Naval and Air limits
- AI lowers tank production if human goes naval and doesn't produce a lot of tanks (Still builds Chronotanks because they can Chronoshift)
- AI Engineers automatically choose new target after capturing or loosing focus on previous selected target
- AI Engineers no longer walk in groups. They choose their own targets
- AI Medics and Mechanics now only repair friendly units, not enemy units
- AI doesn't wait for the player to let his base grow and/or tech up to the highest level (Radar, Air units, etc.)
- AI build MCV's and deploys them, AI limitations for buildings, etc
- AI have a limit of construction yard/war factory/vessel carrier according to fast build on/off. It's more realist against a human even with a massive base
- AI monitors what you are doing and uses this on how to react while upgrading / expanding his base. If you have more of building x, he builds more of x too. It's totally dynamic. :-)
- AIBoostLevel is dynamic by the number of enemies/friendlies he personally sees, to re-balance unfair groups (Can be enabled/disabled/tweaked using the .ini file.)
- You can now set the boost for your allies and your enemies independently (when dynamic AI boost is disabled)
- Added an option to allow the AI to make extra base using MCV's (Will use chronoshift for map with islands)
- AI generates a team to proctect MCVs while going to extra base location
- AI know how to use Chronosphere (Will teleport MCV and Cruiser (Also Missile Sub if tech stolen))
- AI Know how to use Iron CUrtain (Will protect MCV/Mad-Tank and Yard/Chronosphere/Iron Curtain/Weap/Ref/Adv Power if attacked and in yellow condition)
- Production hack is now customizable
- Building limit for AI is increased by x2 when last construction yard is built or captured
- Free-For-All detection, it automatically disables Dynamic AI in these cases because there are no teams so nothing to re-balance
- Harvester prio switcher. There is a 5% change of AI goes wild on harvester hunting. The rest of the time it gives harvesters lower prio

---

Note:
- The more Tiberium is available on the map, the harder it is to beat the AI. You might need a faster PC because of the number of units etc.  :-)
- AI Boost level and almost all other settings are editable using the .ini file. It's located in: C:\Program Files (x86)\Steam\steamapps\workshop\content\1213210\2221741447\AIBoost\CCDATA\AIBOOST.INI

---

To do / wishes:
- AI Minelayer control in Skirmish
- In skirmish and not in FFA-games, the AI should be enabled to ally or declare war to change his friends and enemies
- The AI should know how to send a MCV (Or other unit) into a transport and send it over water to a better location
- Like in RA2 Yuri's Revenge: Units should be able to scan the area and attack a specified object while avoiding all other dangers. (Attack object from other side by driving around units and defenses)

Known issues:
- Tech stealing:
The human player must re-deploy the Construction Yard to MCV and back to unlock all stolen tech. Also, for the War Factory. (Or you must build an extra one)
It also changes the voices for some countries. Currently, we don't know how to correct this

Source code: (If you know how to program C++, we always can use some help with our To-do's and wishes)
https://github.com/Bast75/CnC_Remastered_Collection-AI-Boost2


Greetings, have fun and don't forget to rate this mod
Bast75
xXMini FrankiXx
