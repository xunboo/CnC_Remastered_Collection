# AI-Boost 4.0 for Red Alert

**Source update:** 5 October 2026

**Base mod:** AI-Boost 3.2b, released 3 November 2022

This repository contains the AI-Boost Red Alert mod, its configuration and Workshop assets, and the accompanying Command & Conquer Remastered game and map-editor source. The current local build adds coordinated attacks against reachable weak sectors, immediate nearby combat during a march, immediate full assaults with a combat-unit lead above thirty, frequent power-plant air raids, economic targeting by large tank forces, nearby defensive responses, stronger counter-production, smarter harvester ore/refinery selection, and funded expansion near safe ore with additional tank-production capacity. Developed economies also maintain two refineries, two tank factories and two combined MCV/construction-yard capabilities, with multi-silo storage expansion. It also retains the terrain-rendering startup repair and reproducible build and regression scripts.

## Main updates in AI-Boost 4.0

**Local offensive AI update:** this update is recorded in a local Git commit, relative to repository baseline `07d9ada8e7a58357e27312f659b3e034d7b6a2d3` (`Improve AI tactics, harvesting and base expansion`). This cumulative update adds numerical full assaults, faster normal attacks, parallel air harassment and large-tank economic targeting, and includes the preceding encounter, infrastructure, economy recovery and kennel fixes. Earlier tactical, harvesting and expansion work is included in that commit. The commit remains local; it has not been pushed to GitHub.

The earlier published AI and renderer changes are based on [64c7037](https://github.com/xunboo/CnC_Remastered_Collection/commit/64c70377b7311bc00eb4aee4d8453d8f94aed361), compared with its parent [f1f0d42](https://github.com/xunboo/CnC_Remastered_Collection/commit/f1f0d42bc2dcd06d5d1df943c6150ab34bf307ae). [View the complete code difference](https://github.com/xunboo/CnC_Remastered_Collection/compare/f1f0d42bc2dcd06d5d1df943c6150ab34bf307ae...64c70377b7311bc00eb4aee4d8453d8f94aed361).

| Area | Main changes |
| --- | --- |
| Concentrated attack forces | Normal waves keep approximately 20% of available power near the base when at least four fighters can still form a strike force. Attackers gather and advance through shared checkpoints. Both numerical full assaults and occasional random ground assaults can recruit this reserve. Advancing requires at least 75% of assigned units and 80% of estimated combat power to gather. Formation spacing scales with army size; narrow crossings lead to a place where the force can regroup. |
| Immediate close encounters | Ground forces and fleets check contact every three simulation ticks (0.2 simulation seconds). A nearby enemy within native firing range, or able to fire on a member, interrupts formation movement and strategic withdrawal so the group fights immediately. Each unit favors nearby targets it can damage with a valid primary or secondary weapon. Shared fire-pressure and health estimates encourage concentration while reducing overkill. Assigned defenders use the same nearby targeting; uncommitted reserves fire at enemies already in range. The original strategic objective is retained, fleeing contacts cease to divert the group once range is lost, and a marching group regathers at the front before resuming its route or choosing another weak sector. |
| Numerical full assaults | A living armed mobile-unit lead strictly greater than thirty over all active hostile houses triggers at the next three-tick combat check. Exactly thirty does not qualify. Infantry, tanks, ships and armed aircraft count; buildings, unarmed transports, harvesters and MCVs do not. Scripted/rearming military units count symmetrically, while recruitment preserves their native control. Ready compatible forces commit without the normal reserve, random roll, attack/economy timer or local power veto. Ground, fleet and air forces can launch concurrently where actual routes and weapons permit. Close encounters retain priority, new ready reinforcements join every three seconds, and loss of the lead restores normal local safety. |
| Large tank forces attack the economy | More than twenty available main battle tanks prioritize reachable power plants, advanced power plants and refineries. Target alternatives include the latest enemy positions and defenses. Ordinary local safety still applies, with another weak target as fallback when an economic sector is unreachable or too strong. The count includes usable damaged tanks and does not require the healthy-tank random-assault threshold. |
| Frequent power-plant air harassment | Ready aircraft first seek power and advanced power plants, preferring weaker anti-air coverage. A single loaded aircraft with estimated power of at least 500 can raid an exposed target; small sorties need no four-unit ground cohort. Air sorties run concurrently with ground/fleet attacks, retain rearming and full-magazine checks, and retry on an independent eight-second timer. If no safe power plant can be attacked, another compatible weak target is considered. |
| Random all-out ground assaults | At least 20 available main battle tanks at 50% health or better, with at least 10,000 combined estimated power, enable a 25% native deterministic random chance every 30 simulation seconds. A successful launch has a three-simulation-minute cooldown and can reinforce an existing wave from its current front. The recruited reachable force must satisfy the same tank thresholds. Available ground escorts join without the usual reserve, while harvesters, MCVs, special vehicles and scripted teams retain their duties. Active base raids postpone the assault. Reachable approaches, local advantage, route safety and normal cohesion still govern the launch; enemy buildings and mobile units are eligible targets. |
| Weak sectors and reachable approaches | Enemy buildings and troop positions feed target scoring. Passable terrain, buildings, walls, water and map boundaries determine reachability. Weighted routes favor flanks around concentrated defenses. Ordinary waves reject unsafe unavoidable chokepoints. Target/route scans run every two simulation seconds, alternatives use six-second score hysteresis, and defensive responses refresh every second. Explicit numerical full assaults keep real routes and cohesion while bypassing local power vetoes until their numerical lead is lost. |
| Superiority and continued pressure | Ordinary ground/naval attacks need four fighters, estimated power of 2,000, 115% of nearby reachable mobile reinforcements, and 150% of local defense. Distant enemy main forces do not veto a local strike. Normal waves can select exposed enemy units while buildings remain. New fortifications or reinforcements can redirect a wave or cause withdrawal. Successful forces continue from the front. Weapon checks include useful damage against the target armor. |
| Nearby defense and exposed ore fields | Nearby reachable units respond according to estimated travel time and required strength. Up to three raiding areas receive separate responders, including protection for exposed harvesters and travelling MCVs. Covered minor raids leave spare troops available for offense; uncovered raids hold new offensives. New towers favor threatened or likely enemy approaches while respecting construction proximity and refinery exits. |
| Heavy-weapon response and economic priorities | Massed enemy armor gives main battle tanks and heavy weapons higher production weights, reduces anti-infantry vehicle weighting, and limits infantry escort spending where tank production is available. Economic expansion requires working harvesters, sufficient power and funds, and a viable defense. Recovery of the last construction yard remains possible. Committed production and the new outpost refinery/power budget are reserved before optional spending. |
| Aircraft and naval compatibility | Ships gather as fleets and torpedo weapons receive compatible naval targets. Helicopters can gather for a strike; fully loaded fixed-wing aircraft launch a common sortie from nearby airfields. Rearming, docked aircraft, unlimited-ammo settings, and secondary anti-air weapons are handled explicitly. An opponent's remaining unarmed MCV remains targetable after its buildings are destroyed. |
| Nearby rich ore and reduced field congestion | Automatic harvesters compare reachable ground routes and existing ore destinations claimed by other trucks. Within a nearby band of two to four effective route cells, richer ore and gems receive priority. Crowded patches lose priority. Returning harvesters rescan from their current position instead of automatically revisiting a distant remembered field. |
| Reachable refineries and separate waiting positions | Selection uses the actual southern docking entrance, route length, current unloading/radio occupancy, and incoming truck reservations. Nearby free refineries generally beat busy ones; a distant free bay does not automatically override a much closer queue. Waiting trucks use separate squares two to four cells from the entrance, keep an arrived holding position, and reconsider every three simulation seconds. Small equivalent changes retain the current choice. |
| Damaged harvester self-preservation | After actual damage from an enemy, an automatic harvester carrying any ore and at or below `ConditionRed` (25% health by default) returns early, retaining its partial load for the native unloading sequence. Retreat takes precedence over chasing infantry to crush. Explicit movement, repair commands, scripted teams, accepted docking, and unloading are preserved. |
| Late-game infrastructure minimums | From five simulation minutes, free construction decisions keep checking for at least two refineries and two War Factories. An enabled category with a one-building strategy limit gains an effective minimum of two. Funded second factories no longer depend on the current army having spare unit-cap slots or on a ground-oriented strategy. Category zero, technology, available income, adequate working harvesters, reserved funds and the overall building cap still apply. Lost second buildings are reconsidered. Site searches are cached for ten simulation seconds and invalidated by relevant infrastructure inventory changes; native placement rechecks the site. |
| Two combined base capabilities | From five simulation minutes, construction yards and undeployed MCVs together have a minimum goal of two. One yard plus one MCV, or two yards, satisfies it. The effective positive base cap is at least two. The second base can use safe already-served dense ore while retaining a reachable factory route, twelve-cell yard separation and legal refinery/power follow-up plots. MCV purchase still requires two working harvesters, six equivalent defenders, technology, power and its full purchase/outpost/combat budget. Missing Repair Depots can be funded. Further expansion follows the existing distant-ore policy and caps. |
| Multiple ore silos | Storage at 90% of native ore capacity triggers a funded storage plan with at least two silos. An existing single silo also establishes the pair, so the plan can recover after state reset or a silo loss. Finishing one does not cancel the second when occupancy drops. Once established, storage grows again when free capacity falls below two silo capacities, and stops when the pair and buffer are sufficient. Placement avoids docking/factory exits and preserves current harvester routes. Unpaid production, military reserves and travelling-MCV refinery funds remain reserved; extra power is funded before a brownout. The old 99% skirmish candidate cannot bypass this planning. |
| Funded MCV expansion near safe ore | Optional MCVs require two working harvesters, adequate power, six equivalent defenders, and funds for the MCV, first outpost refinery and recovery/combat buffer after unpaid production. Safe reachable ore and legal follow-up space determine expansion; distant enemy numbers do not veto it. A missing Repair Depot is funded as an MCV prerequisite when a viable outpost exists. Dense patches are scored by richness, route, mining claims and enemy danger. |
| Safe deployment and route control | MCVs follow short checkpoints along safe ground routes, with periodic threat and placement checks and immediate checks on arrival. New threats trigger a new route or site; an MCV already inside danger can retreat toward safe home terrain. Walls, water, corner cutting, building footprints, and actual factory exits are considered. A lost first construction yard can recover without the optional expansion conditions. |
| Refineries near distant mining routes | A reachable ore route of at least 16 cells can justify a closer refinery with a docking entrance within eight route cells of the patch and an improvement of at least eight cells. A legal refinery extension takes precedence over buying an MCV; otherwise a funded resource outpost can provide construction proximity. New yards and overloaded refinery capacity also raise refinery priority. |
| Additional tank factories | Beyond the late-game two-factory minimum, funded ground-oriented armies or armies facing enemy armor can build additional War Factories when refinery/harvester capacity supports them and useful tank production remains below its cap. Placement keeps exits accessible and discourages refinery congestion. The native factory-count speed bonus applies; with Aftermath enabled and fast build disabled, investment stops at two factories. Recovery of a destroyed first factory reserves replacement-harvester funds when needed. |
| Adaptive unit production | Production responds to enemy armor, infantry, aircraft, and defensive buildings. Anti-air and siege units receive appropriate weighting, while support and explosive vehicles remain a limited part of the fighting force. |
| Frequent attack opportunities | Ready normal waves become eligible after one simulation minute. Positive AIAttackInterval values use two seconds per unit, clamped to 4-15 simulation seconds: the included value 5 gives ten seconds. Zero interval or SpeedRush uses two seconds. Opportunities refresh every two seconds and target alternatives every six, with score hysteresis to limit oscillation. An aborted wave can retry after five seconds. Air raids have a separate eight-second opportunity timer and run alongside ground and naval waves. |
| Ore shortage recovery | One truck failing to find ore no longer marks the whole skirmish house permanently short of ore. Successful searches and a periodic whole-economy scan clear stale flags and let idle trucks resume. Reachable ore counts as income independently of the dense-patch threshold for an outpost; real exhaustion requires one minute without reachable ore or loaded returning trucks. Capacity refineries can consider smaller viable patches. |
| Kennel build/sell loop | Skirmish AI builds at most one optional kennel, at low priority, after its first War Factory and only with income, spare power and funds above the economic/combat reserve. Emergency kennel sales start a two-minute rebuilding cooldown. The actual low-cash check does not sell buildings merely because of a stale shortage flag. |
| Economy and base recovery | Replacing harvesters and recovering construction capability takes priority over the mod's combat-unit cap. Essential power, the first refinery, and initial anti-air defenses can recover before optional base-size limits block them. Air-rush bases can build the helipads they need. |
| Strategy selection | A finite list of feasible strategies replaces a selection loop that could stall. Naval access, available technology, and ground forces affect the choices. In automatic mode, infantry, air, and naval rushes can transition to a mixed strategy when opponents develop counters. |
| Enemy information and production limits | Threat snapshots refresh aircraft, ground, and naval information from living opponents, excluding allies and neutral houses. Players with units but no buildings still count. Infantry budgets stay local to each AI house, naval multipliers and bonuses apply to fleet limits, and disabled production categories keep their zero caps. Mechanic and Shock Trooper counts use their actual infantry types. |
| Campaign attack behavior and Chrono Tanks | The new tactical controller applies to computer-controlled skirmish houses. Campaign orders and existing scripted teams retain their control. Chrono Tanks join the coordinated ground force in skirmish; the earlier standalone teleport-targeting repair remains in the legacy campaign attack path. |
| Shared target selection | Nearby threat scanning retains the best score across all four scan edges, so a weaker later candidate cannot replace a stronger target. This shared targeting correction also affects human-owned units. |
| Terrain-rendering startup repair | Fourteen ghost, fading, predator, and combined drawing routines now subtract immediate code addresses in MASM. The previous operands read instruction bytes and could jump into the middle of an instruction, causing illegal-instruction or invalid-access crashes during map rendering. |
| Build and regression tooling | The Red Alert build uses the v143 toolset and an isolated output folder. Scripts discover the installed tools, normalize Windows environment-variable names, and run AI and actual-assembly renderer regressions. |

The tactical controller and route policy are in [AITACTICS.CPP](REDALERT/AITACTICS.CPP) and [AITACTICS.H](REDALERT/AITACTICS.H). Harvester decisions are in [HARVESTAI.CPP](REDALERT/HARVESTAI.CPP) and [HARVESTAI.H](REDALERT/HARVESTAI.H), integrated with the native harvest/damage state machines in [UNIT.CPP](REDALERT/UNIT.CPP). Economic expansion and MCV routing are in [AIEXPANSION.CPP](REDALERT/AIEXPANSION.CPP) and [AIEXPANSION.H](REDALERT/AIEXPANSION.H). [FACTORY.H](REDALERT/FACTORY.H) exposes the existing unpaid production balance without adding object state. House integration, production, and economy decisions are in [HOUSE.CPP](REDALERT/HOUSE.CPP) and [AISTRATEGY.H](REDALERT/AISTRATEGY.H). Shared targeting is in [TECHNO.CPP](REDALERT/TECHNO.CPP), and the renderer repair is in [KEYFBUFF.ASM](REDALERT/KEYFBUFF.ASM).

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
# To run only the harvester fixture:
python SCRIPTS/test_redalert_ai.py --test harvester_test
# To run only economic expansion and MCV scenarios:
python SCRIPTS/test_redalert_ai.py --test expansion_test
python SCRIPTS/test_redalert_renderer.py
```

After building, you can also test the renderer object actually linked into that DLL:

```powershell
python SCRIPTS/test_redalert_renderer.py --object build/redalert/obj/KEYFBUFF.obj
```

Validation recorded for the code update:

- **Release/Win32 build succeeded** with the v143 toolset and configured Windows SDK, with zero warnings and zero errors.
- **535 AI checks passed:** 37 production-policy checks, 8 target-selection checks, 35 HouseClass decision checks, 182 tactical route/policy/controller checks, 80 harvester scenarios, and 193 economic expansion scenarios. The fixtures compile the actual `AITACTICS.CPP`, `HARVESTAI.CPP`, and `AIEXPANSION.CPP` against controlled worlds, plus selected native House/unit/harvest functions, MCV mission/deployment hooks, damage-response guards, the unpaid factory-balance getter, actual factory-count speed calculation, emergency-cash/sale functions, and native kennel build priorities.
- **16,164 renderer scenarios passed:** the actual Win32 assembly is compared with expected pixels across sprite-width remainders, drawing flags, clipping, pitch, transparency, shadow/fading effects, and destination memory guards. The final DLL's linked renderer object passed the same checks.

All 44 exported names and ordinals match the previous local DLL. Tactical, harvester, and expansion scheduling state is kept outside serialized game objects and reset on scenario initialization; unit-slot reuse clears harvester and MCV timing. No serialized unit, house, or factory data fields or virtual methods were added. The fixtures exercise decision logic and controlled movement outcomes; native unit collision/movement, full in-game startup, human-opponent win rates, campaign progression, save/load, and multiplayer synchronization still require playtesting.

## Install and configure the current build

Close Red Alert and its instance-server process, then run the [local installer](SCRIPTS/Install-RedAlert.ps1) from the repository root:

```powershell
./SCRIPTS/Install-RedAlert.ps1
# For a different existing AIBoost installation:
./SCRIPTS/Install-RedAlert.ps1 -ModDirectory 'D:\SteamLibrary\steamapps\workshop\content\1213210\2221741447\AIBoost'
```

The installer checks that the game is closed, backs up the active DLL, symbols, and metadata under `review/installed-backups/`, and verifies the copied files by SHA-256. It installs `RedAlert.dll` with its matching `RedAlert.pdb` and updates the local mod description. Existing gameplay configuration and mod assets are preserved. Use `-WhatIf` to inspect the planned operation while the game is closed.

For manual installation, back up the same files, copy both files from `build/redalert/` into the active mod's `Data` folder, and copy `WorkshopContent/AIBoost/ccmod.json` to the mod root. Restart the game, enable AIBoost, and test a stock skirmish map at Normal game speed.

The local mod package is [WorkshopContent/AIBoost](WorkshopContent/AIBoost). For the original subscribed mod, the usual installation path is:

```text
C:\Program Files (x86)\Steam\steamapps\workshop\content\1213210\2221741447\AIBoost
```

AI settings are in [CCDATA/AIBOOST.INI](WorkshopContent/AIBoost/CCDATA/AIBOOST.INI) inside the active mod. The included configuration uses `AIStrategyMode=1`, `AIAttackFirstTime=5`, and `AIAttackInterval=5`.

- `AIStrategyMode=0` enables automatic strategy selection and counter-driven transitions.
- `AIStrategyMode=2` allows proactive naval production where the base has legal naval placement. An infeasible requested strategy falls back to the mixed dynamic mode.
- Skirmish opportunity checks run every two simulation seconds. `AIAttackInterval=5` means a ten-simulation-second interval for ready new ground/naval waves; other positive values use two seconds per unit, clamped to 4-15 seconds. Zero interval or SpeedRush uses two seconds. Normal waves become eligible after one simulation minute; numerical full assaults bypass that opening delay and attack/economy timers. Independent air raid opportunities use eight simulation seconds. Wall-clock timing depends on game speed.
- Close encounters and combat counts check every three simulation ticks. A combat-unit lead strictly above thirty commits all ready compatible ground/naval/air forces without a reserve or random roll, retaining routes, cohesion and close battle priority. Counts combine all hostile armies. More than twenty ready tanks prioritize power/refinery sectors. Existing random ground assaults retain their separate defaults: at least twenty tanks at 50% health, usable ammunition and power 10,000, a 25% chance per thirty seconds, and a three-minute cooldown. These policies are source constants in `REDALERT/AITACTICS.H` and `REDALERT/AITACTICS.CPP`, with no new gameplay INI keys.
- Tactical launch and cohesion thresholds are in `REDALERT/AITACTICS.H` and `REDALERT/AITACTICS.CPP`. An aborted wave can reassess after five simulation seconds; blocked numerical assaults retry after three seconds. A stalled rally or checkpoint gives up after 40 simulation seconds without further gathering progress. Existing technology and production-category caps still apply. Initial skirmish timing follows the opportunity controller described above.
- Late-game minimums become active at five simulation minutes: two refineries, two War Factories and two construction yards/MCVs combined. Construction decisions keep rechecking live quantities, while costly infrastructure site searches refresh at the ten-second economic scan or inventory changes. One-building enabled strategy caps gain a minimum of two; category zero and overall building limits remain. Tank factories need funded production and at least two working harvesters.
- Ore storage is measured by native `Tiberium` and `Capacity`, rather than all available cash. At 90% occupancy, plan at least two silos; later retain free capacity equal to two silo capacities. Costs, power, legal safe space and traffic reachability govern each queued building. These and the five-minute threshold are source constants in `REDALERT/AIEXPANSION.H`; no gameplay INI values changed.
- The economic controller applies to computer-controlled skirmish houses with base building enabled. Resource/placement scans run every ten simulation seconds; travelling MCVs recheck every five seconds and at checkpoints, use destinations no more than four route steps away, and reconsider stalled movement after twenty seconds. Optional expansion decisions have a two-simulation-minute interval.
- A new ore outpost requires a dense patch worth at least twice the refinery price, a direct safe ground route from an actual War Factory exit, and legal plots for the yard, refinery, and power. Optional purchased MCVs also require technology/prerequisites, two actual working harvesters, adequate defense and power, and funds for the MCV, first refinery and reserve. A missing Repair Depot can be prioritized with a funded safe outpost; defense uses local site safety and a minimum garrison, without a global enemy-strength veto. First-yard recovery is handled separately.
- Production budgets subtract unpaid balances and new requests across buildings, vehicles, infantry, aircraft, and ships. A pending MCV reserves its refinery and power funds from military production and optional building spending. Funded refinery/factory growth and factory recovery are considered before the older human-relative base-size limit, while `AIMaxBuildings`, refinery/factory/yard category caps, technology, and legal placement still apply.
- `AIMaxConYardsAndMCVsBasic/Mix/Naval`, `AIRefineryLimitOverwrite`, and `AIWarFactoryLimitBasic/Mix/Naval` retain their configured limits above the requested late-game minimum of two; explicitly disabled zero categories remain disabled. With Aftermath units enabled and `AIAftermathfastbuild=false`, native build speed stops improving after two War Factories, so the planner does not buy further factories for speed. The older `AIAllowLastMCVToDoSecondBase` / `AIAllowMoreMCVsToExtraBase` waypoint/Chronosphere expansion path applies to campaign behavior; skirmish expansion uses safe resource sites.
- `HarvesterOptimizeEnabled=1` enables the new automatic harvester logic for both AI and human-owned trucks. Explicit commands and native docking/unloading retain their control. `AIHarvesterMemoryValue` and the older thrashing/communalism weighting settings apply to legacy selection when optimization is disabled.
- `HarvyOptimizeUnloadWaitWeight` remains the queue-wait cost in equivalent route cells (default six). The new controller consistently uses cells even if native A-star is disabled. Waiting trucks reassess every three simulation seconds; autonomous ore movement can be rescanned after ten seconds without movement progress. `ConditionRed` controls the health threshold for early return.

The current local package is `build/packages/AI-Boost-4.0-RA-pressure-local.zip`; earlier local packages remain available. Extract it and, after closing the game, run `./SCRIPTS/Install-RedAlert.ps1` from the extracted folder in PowerShell 7. Review details are in `review/PRESSURE_2026-10-04.md` and `review/pressure-validation-2026-10-04.json`. The active Steam mod was left untouched.

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
