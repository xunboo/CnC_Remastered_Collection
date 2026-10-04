# AI-Boost 3.2b for Red Alert
Last update at: 3 nov 2022

## Current source fixes and build instructions

This checkout includes AI fixes for economy recovery, counter-unit production, safe Chrono Tank targeting, and strategy selection. It also repairs 14 invalid address calculations in the legacy sprite renderer that caused startup crashes while drawing terrain shadows. Full match startup and competitive win rates still require playtesting.

To build the updated RedAlert DLL, install Visual Studio C++ build tools with the v143 toolset and a Windows SDK, then run this command in PowerShell 7 from the repository root:

```powershell
./SCRIPTS/Build-RedAlert.ps1
```

The DLL and matching debug symbols are written to `build/redalert/`. Python 3 and the installed C++ tools can run the regression scenarios:

```powershell
python SCRIPTS/test_redalert_ai.py
python SCRIPTS/test_redalert_renderer.py
```

The AI suite covers 60 decision and production scenarios. The renderer suite exercises the actual Win32 assembly and checks 16,164 drawing scenarios against expected pixels. Close the game before copying the newly built `RedAlert.dll` into your active AIBoost mod's `Data` folder.

The repository excludes `bin`, `build`, `review`, `Changelogs`, Visual Studio output, and Python caches. The base mod's 2022 notes follow below.

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
