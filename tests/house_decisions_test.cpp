// Runs selected HouseClass methods extracted from HOUSE.CPP, with production
// and world fixtures. The economic decisions and strategy code are unmodified.
#include "../REDALERT/AISTRATEGY.H"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
using std::min;
using std::max;
#define ARRAY_SIZE(a) (int)(sizeof(a) / sizeof((a)[0]))
typedef int UnitType;
typedef int StructType;
typedef int HousesType;
typedef int CELL;
typedef int COORDINATE;
const int HOUSE_NONE = -1, HOUSE_FIRST = 0, HOUSE_NEUTRAL = 10, HOUSE_JP = 11, HOUSE_COUNT = 20;
const int UNIT_NONE = -1, UNIT_HTANK = 0, UNIT_MTANK = 1, UNIT_MTANK2 = 2, UNIT_LTANK = 3;
const int UNIT_APC = 4, UNIT_MINELAYER = 5, UNIT_JEEP = 6, UNIT_HARVESTER = 7;
const int UNIT_ARTY = 8, UNIT_MRJ = 9, UNIT_MGG = 10, UNIT_MCV = 11, UNIT_V2_LAUNCHER = 12;
const int UNIT_TRUCK = 13, UNIT_CHRONOTANK = 14, UNIT_TESLATANK = 15, UNIT_MAD = 16, UNIT_DEMOTRUCK = 17;
const int UNIT_FIRST = 0, UNIT_COUNT = 18;
const int STRUCT_CONST = 0, STRUCT_WEAP = 1, STRUCT_REFINERY = 2, STRUCT_REPAIR = 3, STRUCT_ADVANCED_TECH = 4;
const int STRUCT_SOVIET_TECH = 5, STRUCT_POWER = 6, STRUCT_ADVANCED_POWER = 7, STRUCT_HELIPAD = 8;
const int STRUCT_AIRSTRIP = 9, STRUCT_TENT = 10, STRUCT_BARRACKS = 11;
const int GAME_NORMAL = 0, TICKS_PER_SECOND = 15, RTTI_UNITTYPE = 1, ICON_LEPTON_W = 256;

struct SessionFixture { int Type; SessionFixture() : Type(1) {} } Session;
struct RuleFixture {
    int IQHarvester, AIHarvesterMaxLimit, AIMaxChronotanks, AIGroundUnitSlowDown, AIStrategyMode;
    double AIHarvesterMultiplier;
    bool AIAllowLastMCVToDoSecondBase, AIAllowMoreMCVsToExtraBase;
    int AIMaxMCVOrConYardsInMainBase;
    RuleFixture() : IQHarvester(1), AIHarvesterMaxLimit(10), AIMaxChronotanks(5), AIGroundUnitSlowDown(98),
        AIStrategyMode(0), AIHarvesterMultiplier(2), AIAllowLastMCVToDoSecondBase(false),
        AIAllowMoreMCVsToExtraBase(false), AIMaxMCVOrConYardsInMainBase(2) {}
} Rule;
struct TechnoTypeClass {
    int Type, Cost;
    unsigned Level;
    bool Allowed;
    void const * PrimaryWeapon;
    TechnoTypeClass() : Type(0), Cost(800), Level(1), Allowed(true), PrimaryWeapon(this) {}
    int What_Am_I() const { return RTTI_UNITTYPE; }
    int Cost_Of() const { return Cost; }
    int Get_Ownable() const { return 1; }
};
struct UnitTypeClass : TechnoTypeClass {
    static UnitTypeClass Types[UNIT_COUNT];
    static UnitTypeClass const & As_Reference(UnitType type) { return Types[type]; }
};
UnitTypeClass UnitTypeClass::Types[UNIT_COUNT];
struct BuildingTypeClass : TechnoTypeClass {
    static BuildingTypeClass Types[12];
    static BuildingTypeClass const & As_Reference(StructType type) { return Types[type]; }
};
BuildingTypeClass BuildingTypeClass::Types[12];
struct HouseClass;
struct UnitClass {
    UnitTypeClass const * Class;
    bool Is_Recruitable(HouseClass const *) const { return true; }
};
struct TeamTypeClass {
    bool IsReinforcable, IsPrebuilt, IsAutocreate;
    HousesType House;
    int ClassCount;
    struct Member { TechnoTypeClass const * Class; int Quantity; } Members[8];
};
struct TeamClass {
    TeamTypeClass const * Class;
    bool IsFullStrength, IsForcedActive, IsHasBeen, JustAltered;
};
template<class T> struct ObjectList {
    std::vector<T *> Objects;
    int Count() const { return (int)Objects.size(); }
    T * Ptr(int index) const { return Objects[index]; }
};
ObjectList<TeamClass> Teams;
ObjectList<TeamTypeClass> TeamTypes;
ObjectList<UnitClass> Units;
int random_calls = 0, random_offset = 0;
int Random_Pick(int low, int high)
{
    assert(high >= low);
    ++random_calls;
    return low + random_offset % (high - low + 1);
}
bool Percent_Chance(int chance) { return chance >= 100; }
int Distance(COORDINATE a, COORDINATE b) { return std::abs(a - b); }

struct HouseClass {
    struct ClassFixture { HousesType House; } OwnClass;
    ClassFixture * Class;
    struct ControlFixture { unsigned MaxUnit; int TechLevel; } Control;
    int ID, BuildUnit;
    unsigned CurUnits, CurInfantry, CurAircraft;
    int AIMaxTanksNr, AIMaxConYardsAndMCVs, IQ, ActLike, Money;
    bool IsHuman, IsActive, IsDefeated, Ally, IsBaseBuilding, IsTiberiumShort, UnitInProduction;
    int AI_Economic_Combat_Budget() const {
        return max(0, Money - (!IsHuman && Session.Type != GAME_NORMAL && UQuantity[UNIT_MCV]>0
            ? BuildingTypeClass::As_Reference(STRUCT_REFINERY).Cost_Of()+BuildingTypeClass::As_Reference(STRUCT_POWER).Cost_Of() : 0));
    }
    bool EconomyExpansionReady=false;
    bool AI_Economic_MCV_Ready() { return EconomyExpansionReady; }
    bool AllowExtraBaseAtWaypoint, IncreaseLimitDone, IsAlerted, AIDetectedNavalWar, AIDetectHumanGroundWar, NavalAccess;
    int AICurrentSelectedWeap, AICurrentNrOfWEAPs, AIPersonalStrategyMode;
    int BQuantity[12], UQuantity[UNIT_COUNT];
    AIStrategy::EnemyForces Forces;
    HouseClass() : Class(&OwnClass), ID(0), BuildUnit(UNIT_NONE), CurUnits(0), CurInfantry(0), CurAircraft(0),
        AIMaxTanksNr(50), AIMaxConYardsAndMCVs(2), IQ(5), ActLike(0), Money(10000), IsHuman(false), IsActive(true),
        IsDefeated(false), Ally(false), IsBaseBuilding(true), IsTiberiumShort(false), UnitInProduction(false),
        AllowExtraBaseAtWaypoint(false), IncreaseLimitDone(false), IsAlerted(false), AIDetectedNavalWar(false),
        AIDetectHumanGroundWar(true), NavalAccess(false), AICurrentSelectedWeap(0), AICurrentNrOfWEAPs(1), AIPersonalStrategyMode(0)
    {
        OwnClass.House = 0;
        Control.MaxUnit = 1000;
        Control.TechLevel = 10;
        std::memset(BQuantity, 0, sizeof(BQuantity));
        std::memset(UQuantity, 0, sizeof(UQuantity));
        BQuantity[STRUCT_WEAP] = 1;
        BQuantity[STRUCT_CONST] = 1;
    }
    bool Is_Ally(HouseClass const * other) const { return other == this || other->Ally; }
    bool Can_Build(TechnoTypeClass const * type, HousesType) const { return type->Allowed && type->Level <= (unsigned)Control.TechLevel; }
    int Available_Money() const { return Money; }
    void AI_Calc2ndBaseLocations() {}
    bool AI_Has_Naval_Access() const { return NavalAccess; }
    AIStrategy::EnemyForces AI_Enemy_Forces() const { return Forces; }
    int AI_Unit_Weight(UnitType, AIStrategy::EnemyForces const &) const;
    int AI_Unit();
    void AI_StrategySwitcher();
};
struct HousesFixture { int ID(HouseClass const * house) const { return house->ID; } } Houses;
struct BuildingClass {
    BuildingTypeClass Type;
    BuildingTypeClass const * Class;
    HouseClass const * House;
    bool IsActive, IsInLimbo;
    int Strength;
    COORDINATE Position;
    BuildingClass(HouseClass const * house, StructType type, COORDINATE position) : Class(&Type), House(house),
        IsActive(true), IsInLimbo(false), Strength(100), Position(position) { Type.Type = type; }
    COORDINATE Center_Coord() const { return Position; }
};
ObjectList<BuildingClass> Buildings;

#include "house_decisions.inc"

static int checks = 0;
static void check(bool condition, char const * message)
{
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
static void types_reset()
{
    for (int i = 0; i < UNIT_COUNT; ++i) {
        UnitTypeClass::Types[i] = UnitTypeClass();
        UnitTypeClass::Types[i].Type = i;
    }
    for (int i = 0; i < 12; ++i) {
        BuildingTypeClass::Types[i] = BuildingTypeClass();
        BuildingTypeClass::Types[i].Type = i;
    }
    Rule = RuleFixture();
    random_calls = random_offset = 0;
}

int main()
{
    types_reset();
    HouseClass economy;
    economy.CurUnits = 51;
    economy.BQuantity[STRUCT_REFINERY] = 1;
    economy.AI_Unit();
    check(economy.BuildUnit == UNIT_HARVESTER, "an army beyond its combat cap can replace a destroyed harvester");
    economy.BuildUnit = UNIT_NONE;
    economy.AIMaxTanksNr = 0;
    economy.AI_Unit();
    check(economy.BuildUnit == UNIT_HARVESTER, "non-ground strategies can still rebuild economic vehicles");

    HouseClass recovery;
    recovery.CurUnits = 50;
    recovery.UQuantity[UNIT_HARVESTER] = 2;
    recovery.BQuantity[STRUCT_REFINERY] = 1;
    recovery.BQuantity[STRUCT_CONST] = 0;
    recovery.BQuantity[STRUCT_REPAIR] = 1;
    recovery.AIMaxConYardsAndMCVs = 0;
    recovery.IsTiberiumShort = true;
    recovery.AI_Unit();
    check(recovery.BuildUnit == UNIT_MCV, "a destroyed construction yard can be recovered without expanding its limit");
    recovery.BuildUnit = UNIT_NONE;
    recovery.CurUnits = 1000;
    recovery.AI_Unit();
    check(recovery.BuildUnit == UNIT_NONE, "economic recovery still respects the engine's unit limit");

    HouseClass full;
    full.CurUnits = 53;
    full.UQuantity[UNIT_HARVESTER] = 2;
    full.UQuantity[UNIT_MCV] = 1;
    full.BQuantity[STRUCT_REFINERY] = 1;
    full.AI_Unit();
    check(full.BuildUnit == UNIT_NONE, "combat production stops exactly at its cap after utility vehicles are excluded");

    for (int i = 0; i < UNIT_COUNT; ++i) UnitTypeClass::Types[i].Allowed = i == UNIT_MTANK;
    HouseClass poor;
    poor.Money = 799;
    poor.AI_Unit();
    check(poor.BuildUnit == UNIT_NONE, "military production cannot lock on an unaffordable unit");
    poor.Money = 800;
    poor.AI_Unit();
    check(poor.BuildUnit == UNIT_MTANK, "exact-price military purchases remain possible");
    for (int i = 0; i < UNIT_COUNT; ++i) UnitTypeClass::Types[i].Allowed = i == UNIT_CHRONOTANK;
    HouseClass chrono;
    chrono.CurUnits = chrono.UQuantity[UNIT_CHRONOTANK] = 5;
    chrono.AI_Unit();
    check(chrono.BuildUnit == UNIT_NONE, "Chrono Tank production stops at the configured count");
    chrono.CurUnits = chrono.UQuantity[UNIT_CHRONOTANK] = 4;
    chrono.AI_Unit();
    check(chrono.BuildUnit == UNIT_CHRONOTANK, "a lost Chrono Tank can be replaced below its cap");

    types_reset();
    for (int i=0;i<UNIT_COUNT;++i) UnitTypeClass::Types[i].Allowed=i==UNIT_MTANK || i==UNIT_MCV;
    HouseClass expansion;
    expansion.BQuantity[STRUCT_REFINERY]=1; expansion.BQuantity[STRUCT_REPAIR]=1;
    expansion.UQuantity[UNIT_HARVESTER]=2; expansion.CurUnits=10; expansion.EconomyExpansionReady=true;
    expansion.AI_Unit();
    check(expansion.BuildUnit==UNIT_MCV,"an approved skirmish economy plan orders an MCV without the legacy random chance");
    expansion.BuildUnit=UNIT_NONE; expansion.EconomyExpansionReady=false; expansion.AI_Unit();
    check(expansion.BuildUnit==UNIT_MTANK,"a rejected expansion leaves the war factory producing fighting units");
    expansion.BuildUnit=UNIT_NONE; expansion.EconomyExpansionReady=true; expansion.BQuantity[STRUCT_REPAIR]=0; expansion.AI_Unit();
    check(expansion.BuildUnit==UNIT_MTANK,"economic MCV production still needs its native repair-depot prerequisite");
    expansion.BuildUnit=UNIT_NONE; expansion.BQuantity[STRUCT_REPAIR]=1; expansion.BQuantity[STRUCT_CONST]=2; expansion.AI_Unit();
    check(expansion.BuildUnit==UNIT_MTANK,"native unit selection enforces the yard limit before an MCV request");
    expansion.BuildUnit=UNIT_NONE; expansion.EconomyExpansionReady=false; expansion.BQuantity[STRUCT_CONST]=1;
    expansion.UQuantity[UNIT_MCV]=1; expansion.Money=2399; expansion.AI_Unit();
    check(expansion.BuildUnit==UNIT_NONE,"native combat production preserves refinery and power funding for a moving MCV");
    expansion.Money=2400; expansion.AI_Unit();
    check(expansion.BuildUnit==UNIT_MTANK,"native combat production can spend the surplus above expansion funding");

    types_reset();
    HouseClass strategy;
    strategy.AIPersonalStrategyMode = 3;
    strategy.AI_StrategySwitcher();
    check(strategy.AIPersonalStrategyMode == 1 && random_calls <= 1, "naval strategy on a landlocked base terminates with a valid fallback");
    strategy.NavalAccess = true;
    strategy.AIPersonalStrategyMode = 3;
    strategy.AI_StrategySwitcher();
    check(strategy.AIPersonalStrategyMode == 3, "naval rush does not loop when the human has no ships");
    Rule.AIStrategyMode = 3;
    strategy.NavalAccess = false;
    strategy.AI_StrategySwitcher();
    check(strategy.AIPersonalStrategyMode == 1, "forced naval strategy falls back when its base lacks water access");
    Rule.AIStrategyMode = 4;
    strategy.Control.TechLevel = 1;
    BuildingTypeClass::Types[STRUCT_HELIPAD].Level = 9;
    BuildingTypeClass::Types[STRUCT_AIRSTRIP].Level = 9;
    strategy.AI_StrategySwitcher();
    check(strategy.AIPersonalStrategyMode == 1, "forced air rush falls back below aircraft tech level");
    Rule.AIStrategyMode = 0;
    strategy.Control.TechLevel = 10;
    strategy.AIPersonalStrategyMode = 5;
    strategy.Forces.Armor = 20;
    strategy.AI_StrategySwitcher();
    check(strategy.AIPersonalStrategyMode == 1, "infantry rush responds to a strong armored counter");
    strategy.AIPersonalStrategyMode = 4;
    strategy.Forces.AntiAirDefenses = 10;
    strategy.AI_StrategySwitcher();
    check(strategy.AIPersonalStrategyMode == 1, "air rush responds to strong anti-air defenses");
    strategy.AIPersonalStrategyMode = 3;
    strategy.NavalAccess = true;
    strategy.Forces.Aircraft = 5;
    strategy.AI_StrategySwitcher();
    check(strategy.AIPersonalStrategyMode == 2, "naval rush adds other arms when attacked by aircraft");
    Rule.AIStrategyMode = 99;
    strategy.AI_StrategySwitcher();
    check(strategy.AIPersonalStrategyMode == 1, "invalid forced strategies do not create an invalid selection");

    HouseClass counter;
    counter.Forces.Armor = 30;
    check(counter.AI_Unit_Weight(UNIT_MTANK, counter.Forces) > counter.AI_Unit_Weight(UNIT_LTANK, counter.Forces), "heavy enemy armor favors main battle tanks over light tanks");
    check(counter.AI_Unit_Weight(UNIT_MTANK, counter.Forces) > counter.AI_Unit_Weight(UNIT_JEEP, counter.Forces) * 10, "massed armor strongly reduces anti-infantry vehicle production");
    UnitTypeClass::Types[UNIT_TRUCK].PrimaryWeapon = NULL;
    check(counter.AI_Unit_Weight(UNIT_TRUCK, counter.Forces) < 5, "unarmed trucks cannot inherit the armor-response bonus");
    for (int i = 0; i < UNIT_COUNT; ++i) UnitTypeClass::Types[i].Allowed = i == UNIT_LTANK;
    counter.AI_Unit();
    check(counter.BuildUnit == UNIT_LTANK, "heavy-unit preferences retain a legal low-tech fallback");
    types_reset();

    HouseClass own, foe, ally, neutral;
    foe.OwnClass.House = 1;
    ally.OwnClass.House = 2;
    ally.Ally = true;
    neutral.OwnClass.House = HOUSE_NEUTRAL;
    Buildings.Objects.clear();
    check(AI_Chrono_Target(own, 0) == NULL, "no enemy buildings produces a safe empty Chrono target");
    BuildingClass own_building(&own, STRUCT_CONST, 0), ally_building(&ally, STRUCT_CONST, 256), neutral_building(&neutral, STRUCT_CONST, 512);
    Buildings.Objects.push_back(&own_building);
    Buildings.Objects.push_back(&ally_building);
    Buildings.Objects.push_back(&neutral_building);
    check(AI_Chrono_Target(own, 0) == NULL, "Chrono targeting excludes own, allied, and neutral buildings");
    BuildingClass yard(&foe, STRUCT_CONST, 2560), filler(&foe, STRUCT_TENT, 256);
    Buildings.Objects.push_back(&yard);
    Buildings.Objects.push_back(&filler);
    check(AI_Chrono_Target(own, 0) == &yard, "Chrono Tanks prefer important enemy infrastructure");
    yard.IsInLimbo = true;
    check(AI_Chrono_Target(own, 0) == &filler, "Chrono targeting excludes buildings in limbo");
    filler.Strength = 0;
    check(AI_Chrono_Target(own, 0) == NULL, "destroyed buildings cannot become Chrono targets");
    filler.Strength = 100;
    foe.IsDefeated = true;
    check(AI_Chrono_Target(own, 0) == NULL, "defeated owners cannot become Chrono targets");
    std::cout << checks << " actual house-decision scenarios passed.\n";
}
