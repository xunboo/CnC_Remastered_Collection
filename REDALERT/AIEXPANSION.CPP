// Copyright 2026. Distributed under the project's GPLv3 license.
#include "AILOG.H"
#include "FUNCTION.H"
#include "AIEXPANSION.H"
#include "HARVESTAI.H"
#include "AISTRATEGY.H"
#include <algorithm>
#include <climits>
#include <map>

namespace {
using HarvestAI::TravelMap;
using HarvestAI::Clamp;
struct Patch { int Cell, Value, Distance, Claims; };
struct EconomyState {
    HouseClass const * Owner;
    int LastFrame, NextScan, NextExpansion, Inventory, DrySince, KennelUntil;
    int RefineryCell, ExpansionCell, SiloGoal;
    int FactoryCount, SiloCount, FactoryCell, StorageCell, BaseRefineryCell;
    bool RemoteRefinery, Income, OreAvailable;
    EconomyState() : Owner(NULL), LastFrame(-1), NextScan(0), NextExpansion(0), Inventory(-1), DrySince(-1), KennelUntil(0),
        RefineryCell(-1), ExpansionCell(-1), SiloGoal(0), FactoryCount(-1), SiloCount(-1),
        FactoryCell(-2), StorageCell(-2), BaseRefineryCell(-2), RemoteRefinery(false), Income(false), OreAvailable(false) {}
};
struct DeployState {
    UnitClass const * Owner;
    int LastFrame, NextCheck, Goal, Waypoint, Cell, ProgressFrame, FailedGoal, FailedUntil;
    bool Retreat;
    std::vector<int> Route;
    DeployState() : Owner(NULL), LastFrame(-1), NextCheck(0), Goal(-1), Waypoint(-1), Cell(-1), ProgressFrame(0), FailedGoal(-1), FailedUntil(0), Retreat(false) {}
};
EconomyState Economies[HOUSE_COUNT];
std::map<TARGET, DeployState> Deployments;

static bool Enabled(HouseClass const * house)
{
    return house && !house->IsHuman && house->IsActive && !house->IsDefeated
        && house->IsBaseBuilding && Session.Type != GAME_NORMAL;
}
static bool Late_Economy(HouseClass const * house)
{
    return Enabled(house) && Frame >= TICKS_PER_MINUTE * AIExpansion::LateMinutes;
}
static int Minimum_Limit(HouseClass const * house, int configured)
{
    // An enabled category may not leave a developed base permanently at one.
    return configured > 0 && Late_Economy(house) ? max(configured, (int)AIExpansion::CoreMinimum) : configured;
}
static bool Need_Core_Yard(HouseClass const * house)
{
    return Late_Economy(house) && house->AIMaxConYardsAndMCVs > 0
        && const_cast<HouseClass *>(house)->QuantityB(STRUCT_CONST) < AIExpansion::CoreMinimum;
}
static bool Alive(TechnoClass const * object)
{
    return object && object->IsActive && !object->IsInLimbo && object->Strength > 0
        && Map.In_Radar(Coord_Cell(object->Center_Coord()));
}
static bool Own_Building(BuildingClass const * building, HouseClass const * house)
{
    return Alive(building) && building->House == house && building->Mission != MISSION_DECONSTRUCTION;
}
static bool Refinery(BuildingClass const * building, HouseClass const * house)
{
    return Own_Building(building, house) && *building == STRUCT_REFINERY
        && building->Mission != MISSION_CONSTRUCTION && building->BState != BSTATE_CONSTRUCTION;
}
static int Entrance(BuildingClass const * building)
{
    return Coord_Cell(Adjacent_Cell(building->Center_Coord(), DIR_S));
}
// Refineries have a 3x3 foundation; the door is south of its center.
static int New_Entrance(int cell) { return cell + MAP_CELL_W * 2 + 1; }
static bool Opponent(HouseClass const * house, HouseClass const * other)
{
    return other && other != house && other->IsActive && !other->IsDefeated
        && other->Class->House != HOUSE_NEUTRAL && other->Class->House != HOUSE_JP && !house->Is_Ally(other);
}
static void Threat(std::vector<unsigned char> & unsafe, TechnoClass const * object, HouseClass const * house)
{
    if (!Alive(object) || !Opponent(house, object->House)) return;
    TechnoTypeClass const * type = object->Techno_Type_Class();
    int radius = object->What_Am_I() == RTTI_BUILDING ? 8 : 0;
    WeaponTypeClass const * weapons[] = {type->PrimaryWeapon, type->SecondaryWeapon};
    for (int i = 0; i < 2; ++i) {
        WeaponTypeClass const * weapon = weapons[i];
        if (weapon && weapon->Bullet && weapon->Bullet->IsAntiGround && weapon->Attack > 0)
            radius = max(radius, Clamp(weapon->Range / CELL_LEPTON_W + 5, 8, 24));
    }
    if (!radius) return;
    int cell = Coord_Cell(object->Center_Coord()), x = cell % MAP_CELL_W, y = cell / MAP_CELL_W;
    for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
        int nx = x + dx, ny = y + dy;
        if (nx >= 0 && nx < MAP_CELL_W && ny >= 0 && ny < MAP_CELL_H) unsafe[ny * MAP_CELL_W + nx] = 1;
    }
}
static void Terrain(TravelMap & field, std::vector<unsigned char> & unsafe, HouseClass const * house, bool avoid_threats = true)
{
    unsafe.assign(MAP_CELL_TOTAL, 0);
    for (int i = 0; i < Buildings.Count(); ++i) Threat(unsafe, Buildings.Ptr(i), house);
    for (int i = 0; i < Units.Count(); ++i) Threat(unsafe, Units.Ptr(i), house);
    for (int i = 0; i < Infantry.Count(); ++i) Threat(unsafe, Infantry.Ptr(i), house);
    for (int i = 0; i < Vessels.Count(); ++i) Threat(unsafe, Vessels.Ptr(i), house);
    for (int i = 0; i < Aircraft.Count(); ++i) Threat(unsafe, Aircraft.Ptr(i), house);
    UnitTypeClass const & type = UnitTypeClass::As_Reference(UNIT_MCV);
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        if (!Map.In_Radar((CELL)cell) || (avoid_threats && unsafe[cell]) || Map[(CELL)cell].Cell_Building()) continue;
        OverlayType overlay = Map[(CELL)cell].Overlay;
        if (overlay != OVERLAY_NONE && OverlayTypeClass::As_Reference(overlay).IsWall) continue;
        field.Clear[cell] = Map[(CELL)cell].Is_Clear_To_Move(type.Speed, true, true, -1, type.MZone) ? 1 : 0;
    }
}
static int Ore_Value(int cell)
{
    if (!Map.In_Radar((CELL)cell) || Map[(CELL)cell].Land_Type() != LAND_TIBERIUM
        || Map[(CELL)cell].Cell_Building()) return 0;
    int value = 0;
    switch (Map[(CELL)cell].Overlay) {
        case OVERLAY_GOLD1: case OVERLAY_GOLD2: case OVERLAY_GOLD3: case OVERLAY_GOLD4: value = Rule.GoldValue; break;
        case OVERLAY_GEMS1: case OVERLAY_GEMS2: case OVERLAY_GEMS3: case OVERLAY_GEMS4: value = Rule.GemValue * 4; break;
    }
    return Clamp((Map[(CELL)cell].OverlayData + 1) * value, 0, 1000000);
}
static std::vector<int> Footprint(BuildingTypeClass const & type, int cell, bool placement = true)
{
    std::vector<int> cells;
    short const * list = type.Occupy_List(placement);
    while (list && *list != REFRESH_EOL) cells.push_back(cell + *list++);
    return cells;
}
static bool Legal_Site(BuildingTypeClass const & type, int cell, std::vector<unsigned char> const & unsafe)
{
    if (cell < 0 || cell >= MAP_CELL_TOTAL) return false;
    short const * list = type.Occupy_List(true);
    while (list && *list != REFRESH_EOL) {
        int offset = *list++, next = cell + offset;
        int x = cell % MAP_CELL_W + offset % MAP_CELL_W;
        if (x < 0 || x >= MAP_CELL_W || next < 0 || next >= MAP_CELL_TOTAL
            || !Map.In_Radar((CELL)next) || unsafe[next] || Ore_Value(next)) return false;
    }
    return type.Legal_Placement((CELL)cell) != 0;
}
static bool Overlap(std::vector<int> const & a, std::vector<int> const & b)
{
    for (unsigned int i = 0; i < a.size(); ++i)
        if (std::find(b.begin(), b.end(), a[i]) != b.end()) return true;
    return false;
}
static int Separation(int a, int b)
{
    return max(abs(a % MAP_CELL_W - b % MAP_CELL_W), abs(a / MAP_CELL_W - b / MAP_CELL_W));
}
static bool Adjacent_Plots(std::vector<int> const & a, std::vector<int> const & b)
{
    for (unsigned int i = 0; i < a.size(); ++i)
        for (unsigned int j = 0; j < b.size(); ++j) if (Separation(a[i], b[j]) == 1) return true;
    return false;
}
static int Nearby_Clear(TravelMap const & field, int cell)
{
    if (field.Valid(cell) && field.Clear[cell]) return cell;
    int best = -1, distance = INT_MAX;
    for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) {
        int x = cell % MAP_CELL_W + dx, y = cell / MAP_CELL_W + dy;
        if (x < 0 || x >= MAP_CELL_W || y < 0 || y >= MAP_CELL_H) continue;
        int next = y * MAP_CELL_W + x, d = max(abs(dx), abs(dy));
        if (field.Clear[next] && d < distance) { best = next; distance = d; }
    }
    return best;
}
static std::vector<int> Production_Sources(TravelMap const & field, HouseClass const * house, bool factories_only = false)
{
    std::vector<int> sources;
    for (int i = 0; i < Buildings.Count(); ++i) {
        BuildingClass const * building = Buildings.Ptr(i);
        if (!Own_Building(building, house) || (*building != STRUCT_WEAP && (factories_only || *building != STRUCT_CONST))) continue;
        int cell;
        if (*building == STRUCT_WEAP && building->Class->ExitList) {
            cell = Coord_Cell(building->Coord) + building->Class->ExitList[0];
            if (!field.Valid(cell) || !field.Clear[cell]) continue;
        } else {
            cell = Nearby_Clear(field, Coord_Cell(building->Exit_Coord()));
        }
        if (cell >= 0) sources.push_back(cell);
    }
    return sources;
}
static std::vector<int> Refinery_Sources(TravelMap const & field, HouseClass const * house)
{
    std::vector<int> sources;
    for (int i = 0; i < Buildings.Count(); ++i) {
        BuildingClass const * building = Buildings.Ptr(i);
        if (Refinery(building, house) && field.Valid(Entrance(building)) && field.Clear[Entrance(building)]) sources.push_back(Entrance(building));
    }
    return sources;
}
static std::vector<Patch> Patches(TravelMap const & access, TravelMap const & refineries, HouseClass const * house, bool outpost = true)
{
    std::vector<Patch> candidates, result;
    int cost = BuildingTypeClass::As_Reference(STRUCT_REFINERY).Cost_Of();
    int minimum = outpost ? max(1000, cost * 2) : max(500, cost / 2);
    std::vector<int> claims(MAP_CELL_TOTAL, 0);
    for (int i = 0; i < Units.Count(); ++i) {
        UnitClass const * truck = Units.Ptr(i);
        if (!Alive(truck) || truck->House != house || !truck->Class->IsToHarvest
            || truck->Mission != MISSION_HARVEST || !Target_Legal(truck->NavCom)) continue;
        int destination = As_Cell(truck->NavCom);
        if (!access.Valid(destination) || !Ore_Value(destination)) continue;
        int x = destination % MAP_CELL_W, y = destination / MAP_CELL_W;
        for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && nx < MAP_CELL_W && ny >= 0 && ny < MAP_CELL_H) ++claims[ny * MAP_CELL_W + nx];
        }
    }
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        if (access.Distance[cell] < 0 || !Ore_Value(cell)) continue;
        int value = 0, x = cell % MAP_CELL_W, y = cell / MAP_CELL_W;
        for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) {
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= MAP_CELL_W || ny < 0 || ny >= MAP_CELL_H) continue;
            int next = ny * MAP_CELL_W + nx;
            if (access.Distance[next] >= 0) value += Ore_Value(next);
        }
        if (value < minimum) continue;
        Patch patch = {cell, Clamp(value, 0, 1000000), refineries.Distance[cell], claims[cell]};
        candidates.push_back(patch);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](Patch const & a, Patch const & b) {
        int av = a.Value + a.Claims * 4000, bv = b.Value + b.Claims * 4000;
        bool ar = a.Distance < 0 || a.Distance >= AIExpansion::RemoteRoute;
        bool br = b.Distance < 0 || b.Distance >= AIExpansion::RemoteRoute;
        return ar != br ? ar : av > bv;
    });
    for (unsigned int i = 0; i < candidates.size() && result.size() < 24; ++i) {
        bool duplicate = false;
        for (unsigned int j = 0; j < result.size(); ++j)
            if (Separation(candidates[i].Cell, result[j].Cell) <= 6) { duplicate = true; break; }
        if (!duplicate) result.push_back(candidates[i]);
    }
    return result;
}
static int Refinery_Plot(HouseClass const * house, TravelMap const & terrain, std::vector<unsigned char> const & unsafe,
    std::vector<Patch> const & patches, bool & remote)
{
    BuildingTypeClass const & type = BuildingTypeClass::As_Reference(STRUCT_REFINERY);
    int best = -1, score = INT_MIN;
    remote = false;
    for (unsigned int i = 0; i < patches.size(); ++i) {
        Patch const & patch = patches[i];
        TravelMap routes = terrain; routes.Search(patch.Cell);
        for (int dy = -10; dy <= 8; ++dy) for (int dx = -10; dx <= 8; ++dx) {
            int x = patch.Cell % MAP_CELL_W + dx, y = patch.Cell / MAP_CELL_W + dy;
            if (x < 0 || x >= MAP_CELL_W || y < 0 || y >= MAP_CELL_H) continue;
            int cell = y * MAP_CELL_W + x, entrance = New_Entrance(cell);
            if (!routes.Valid(entrance) || routes.Distance[entrance] < 0 || routes.Distance[entrance] > AIExpansion::LocalOreRoute) continue;
            if (!Legal_Site(type, cell, unsafe) || !Map.Passes_Proximity_Check(&type, house->Class->House, type.Occupy_List(true), (CELL)cell)) continue;
            bool is_remote = (patch.Distance < 0 || patch.Distance >= AIExpansion::RemoteRoute)
                && (patch.Distance < 0 || patch.Distance - routes.Distance[entrance] >= 8);
            int value = (is_remote ? 50000 : 0) + patch.Claims * 4000 + Clamp(patch.Value, 0, 16000) - routes.Distance[entrance] * 1000;
            if (value <= score) continue;
            TravelMap placed = terrain;
            std::vector<int> occupied = Footprint(type, cell, false);
            for (unsigned int k = 0; k < occupied.size(); ++k) placed.Clear[occupied[k]] = 0;
            placed.Clear[entrance] = 1;
            placed.Search(entrance);
            if (placed.Distance[patch.Cell] < 0 || placed.Distance[patch.Cell] > AIExpansion::LocalOreRoute) continue;
            value -= (placed.Distance[patch.Cell] - routes.Distance[entrance]) * 1000;
            if (value > score) { score = value; best = cell; remote = is_remote; }
        }
    }
    return best;
}
// Leave legal adjacent refinery and power plots, including their placement bibs.
// Recheck the mining route with the prospective foundations blocked.
static bool Followup_Plots(int yard, int ore, TravelMap const & terrain, std::vector<unsigned char> const & unsafe)
{
    BuildingTypeClass const & refinery = BuildingTypeClass::As_Reference(STRUCT_REFINERY);
    BuildingTypeClass const & power = BuildingTypeClass::As_Reference(STRUCT_POWER);
    std::vector<int> yard_cells = Footprint(BuildingTypeClass::As_Reference(STRUCT_CONST), yard);
    for (int dy = -5; dy <= 5; ++dy) for (int dx = -5; dx <= 5; ++dx) {
        int x = yard % MAP_CELL_W + dx, y = yard / MAP_CELL_W + dy;
        if (x < 0 || x >= MAP_CELL_W || y < 0 || y >= MAP_CELL_H) continue;
        int cell = y * MAP_CELL_W + x;
        if (!Legal_Site(refinery, cell, unsafe)) continue;
        std::vector<int> ref_cells = Footprint(refinery, cell);
        if (Overlap(yard_cells, ref_cells) || !Adjacent_Plots(yard_cells, ref_cells)) continue;
        TravelMap route = terrain;
        std::vector<int> yard_body = Footprint(BuildingTypeClass::As_Reference(STRUCT_CONST), yard, false);
        std::vector<int> ref_body = Footprint(refinery, cell, false);
        for (unsigned int k = 0; k < yard_body.size(); ++k) route.Clear[yard_body[k]] = 0;
        for (unsigned int k = 0; k < ref_body.size(); ++k) route.Clear[ref_body[k]] = 0;
        int entrance = New_Entrance(cell);
        if (!route.Valid(entrance) || !terrain.Clear[entrance]) continue;
        route.Clear[entrance] = 1;
        route.Search(entrance);
        if (route.Distance[ore] < 0 || route.Distance[ore] > AIExpansion::LocalOreRoute) continue;
        for (int py = -4; py <= 4; ++py) for (int px = -4; px <= 4; ++px) {
            int nx = yard % MAP_CELL_W + px, ny = yard / MAP_CELL_W + py;
            if (nx < 0 || nx >= MAP_CELL_W || ny < 0 || ny >= MAP_CELL_H) continue;
            int pcell = ny * MAP_CELL_W + nx;
            if (!Legal_Site(power, pcell, unsafe)) continue;
            std::vector<int> power_cells = Footprint(power, pcell);
            if (Overlap(power_cells, yard_cells) || Overlap(power_cells, ref_cells) || !Adjacent_Plots(power_cells, yard_cells)) continue;
            TravelMap powered = route;
            std::vector<int> power_body = Footprint(power, pcell, false);
            for (unsigned int k = 0; k < power_body.size(); ++k) powered.Clear[power_body[k]] = 0;
            powered.Search(entrance);
            if (powered.Distance[ore] >= 0 && powered.Distance[ore] <= AIExpansion::LocalOreRoute) return true;
        }
    }
    return false;
}
static int Expansion_Plot(HouseClass const * house, TravelMap const & routes, std::vector<unsigned char> const & unsafe,
    std::vector<Patch> const & patches, int failed = -1, bool local = false)
{
    BuildingTypeClass const & yard = BuildingTypeClass::As_Reference(STRUCT_CONST);
    int best = -1, score = INT_MIN;
    for (unsigned int i = 0; i < patches.size(); ++i) {
        Patch const & patch = patches[i];
        if (!local && const_cast<HouseClass *>(house)->QuantityB(STRUCT_CONST) > 0 && patch.Distance >= 0 && patch.Distance < AIExpansion::RemoteRoute) continue;
        for (int dy = -8; dy <= 8; ++dy) for (int dx = -8; dx <= 8; ++dx) {
            int x = patch.Cell % MAP_CELL_W + dx, y = patch.Cell / MAP_CELL_W + dy;
            if (x <= 0 || x >= MAP_CELL_W - 2 || y <= 0 || y >= MAP_CELL_H - 3) continue;
            int cell = y * MAP_CELL_W + x, top = cell - MAP_CELL_W - 1;
            if (routes.Distance[cell] < 0 || (failed >= 0 && Separation(cell, failed) <= 3)) continue;
            bool close = false;
            for (int b = 0; b < Buildings.Count(); ++b) {
                BuildingClass const * existing = Buildings.Ptr(b);
                if (Own_Building(existing, house) && *existing == STRUCT_CONST
                    && Separation(cell, Coord_Cell(existing->Center_Coord())) < 12) { close = true; break; }
            }
            if (close || !Legal_Site(yard, top, unsafe)) continue;
            int value = Clamp(patch.Value, 0, 16000) + patch.Claims * 4000 - routes.Distance[cell] * 300 - Separation(cell, patch.Cell) * 200;
            if (value <= score || !Followup_Plots(top, patch.Cell, routes, unsafe)) continue;
            best = cell; score = value;
        }
    }
    return best;
}
// Follow the safe flood-fill route, using short destinations that cannot cut
// across a blocked corner when the native movement code computes its path.
static std::vector<int> Route_To(TravelMap const & field, int goal)
{
    std::vector<int> route;
    if (!field.Valid(goal) || field.Distance[goal] < 0) return route;
    int cell = goal;
    route.push_back(cell);
    while (field.Distance[cell] > 0) {
        int next = -1, x = cell % MAP_CELL_W, y = cell / MAP_CELL_W;
        for (int dy = -1; dy <= 1 && next < 0; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            int nx = x + dx, ny = y + dy;
            if ((!dx && !dy) || nx < 0 || nx >= MAP_CELL_W || ny < 0 || ny >= MAP_CELL_H) continue;
            int candidate = ny * MAP_CELL_W + nx;
            if (field.Distance[candidate] != field.Distance[cell] - 1) continue;
            if (dx && dy && (!field.Clear[y * MAP_CELL_W + nx] || !field.Clear[ny * MAP_CELL_W + x])) continue;
            next = candidate; break;
        }
        if (next < 0) return std::vector<int>();
        cell = next; route.push_back(cell);
    }
    std::reverse(route.begin(), route.end());
    return route;
}
static int Route_Waypoint(TravelMap const & field, std::vector<int> const & route)
{
    if (route.empty()) return -1;
    int source = route[0], best = source;
    for (unsigned int i = 1; i < route.size() && i <= 4; ++i) {
        int goal = route[i]; bool clear = true;
        for (int y = min(source / MAP_CELL_W, goal / MAP_CELL_W); y <= max(source / MAP_CELL_W, goal / MAP_CELL_W) && clear; ++y)
            for (int x = min(source % MAP_CELL_W, goal % MAP_CELL_W); x <= max(source % MAP_CELL_W, goal % MAP_CELL_W); ++x)
                if (!field.Clear[y * MAP_CELL_W + x]) { clear = false; break; }
        if (!clear) break;
        best = goal;
    }
    return best;
}
static int Retreat_Home(HouseClass const * house, TravelMap const & routes, std::vector<unsigned char> const & unsafe)
{
    int best = -1, distance = INT_MAX;
    for (int i = 0; i < Buildings.Count(); ++i) {
        BuildingClass const * building = Buildings.Ptr(i);
        if (!Own_Building(building, house) || (*building != STRUCT_CONST && *building != STRUCT_REFINERY && *building != STRUCT_WEAP)) continue;
        int origin = Coord_Cell(building->Center_Coord());
        for (int dy = -5; dy <= 5; ++dy) for (int dx = -5; dx <= 5; ++dx) {
            if (max(abs(dx), abs(dy)) < 3) continue;
            int x = origin % MAP_CELL_W + dx, y = origin / MAP_CELL_W + dy;
            if (x < 0 || x >= MAP_CELL_W || y < 0 || y >= MAP_CELL_H) continue;
            int cell = y * MAP_CELL_W + x;
            if (unsafe[cell] || !routes.Clear[cell] || routes.Distance[cell] < 0 || Ore_Value(cell)) continue;
            if (routes.Distance[cell] < distance) { best = cell; distance = routes.Distance[cell]; }
        }
    }
    return best;
}

static int Tank_Cost(HouseClass const * house)
{
    UnitType const tanks[] = {UNIT_HTANK, UNIT_MTANK, UNIT_MTANK2, UNIT_LTANK};
    int cost = INT_MAX;
    for (int i = 0; i < 4; ++i) {
        UnitTypeClass const & type = UnitTypeClass::As_Reference(tanks[i]);
        if (house->Can_Build(&type, house->ActLike)) cost = min(cost, max(1, type.Cost_Of()));
    }
    return cost == INT_MAX ? 0 : cost;
}
static int Working_Harvesters(HouseClass const * house)
{
    int count = 0;
    for (int i = 0; i < Units.Count(); ++i) {
        UnitClass const * truck = Units.Ptr(i);
        if (Alive(truck) && truck->House == house && truck->Class->IsToHarvest && !truck->Team
            && (truck->Mission == MISSION_HARVEST || (truck->Mission == MISSION_ENTER && truck->Tiberium > 0))) ++count;
    }
    return count;
}
static int Reserve(HouseClass const * house)
{
    int reserve = max(1200, Tank_Cost(house) * 2) + BuildingTypeClass::As_Reference(STRUCT_POWER).Cost_Of();
    if (const_cast<HouseClass *>(house)->QuantityU(UNIT_HARVESTER) < 2) reserve += UnitTypeClass::As_Reference(UNIT_HARVESTER).Cost_Of();
    return reserve;
}
static EconomyState & Scan(HouseClass const * house, bool force = false)
{
    EconomyState & state = Economies[house->Class->House];
    if (state.Owner != house || Frame < state.LastFrame) { state = EconomyState(); state.Owner = house; }
    state.LastFrame = Frame;
    int inventory = const_cast<HouseClass *>(house)->QuantityB(STRUCT_CONST) + const_cast<HouseClass *>(house)->QuantityB(STRUCT_REFINERY) * 32 + const_cast<HouseClass *>(house)->QuantityU(UNIT_MCV) * 1024 + const_cast<HouseClass *>(house)->QuantityU(UNIT_HARVESTER) * 32768;
    int factories = const_cast<HouseClass *>(house)->QuantityB(STRUCT_WEAP);
    int silos = const_cast<HouseClass *>(house)->QuantityB(STRUCT_STORAGE);
    if (!force && Frame < state.NextScan && inventory == state.Inventory && factories == state.FactoryCount && silos == state.SiloCount) return state;
    state.FactoryCount = factories; state.SiloCount = silos;
    state.FactoryCell = state.StorageCell = state.BaseRefineryCell = -2;
    state.NextScan = Frame + TICKS_PER_SECOND * AIExpansion::ScanSeconds; state.Inventory = inventory;
    state.RefineryCell = state.ExpansionCell = -1; state.RemoteRefinery = state.Income = false;
    TravelMap terrain(MAP_CELL_W, MAP_CELL_H); std::vector<unsigned char> unsafe;
    Terrain(terrain, unsafe, house);
    TravelMap access = terrain; access.Search(Production_Sources(terrain, house));
    TravelMap refs = terrain; refs.Search(Refinery_Sources(terrain, house));
    // Income and map exhaustion must not depend on the dense, safe deposits
    // needed to justify an outpost. A single blocked truck is not a house-wide
    // shortage, and temporarily contested ore still exists.
    TravelMap physical(MAP_CELL_W, MAP_CELL_H); std::vector<unsigned char> ignored;
    Terrain(physical, ignored, house, false);
    std::vector<int> sources = Refinery_Sources(physical, house);
    std::vector<int> production = Production_Sources(physical, house);
    sources.insert(sources.end(), production.begin(), production.end());
    physical.Search(sources);
    state.OreAvailable = false;
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell)
        if (physical.Distance[cell] >= 0 && Ore_Value(cell) > 0) { state.OreAvailable = true; break; }
    bool loaded = false, workforce = false;
    for (int i = 0; i < Units.Count(); ++i) {
        UnitClass const * truck = Units.Ptr(i);
        if (!Alive(truck) || truck->House != house || !truck->Class->IsToHarvest || truck->Team
            || physical.Distance[Coord_Cell(truck->Center_Coord())] < 0) continue;
        if (truck->Tiberium > 0 && truck->Mission == MISSION_ENTER) loaded = true;
        if (truck->Mission == MISSION_HARVEST || truck->Mission == MISSION_GUARD
            || (truck->Mission == MISSION_ENTER && truck->Tiberium > 0)) workforce = true;
    }
    state.Income = const_cast<HouseClass *>(house)->QuantityB(STRUCT_REFINERY) > 0
        && (loaded || (state.OreAvailable && workforce));
    if (state.OreAvailable || loaded) state.DrySince = -1;
    else if (state.DrySince < 0) state.DrySince = Frame;
    std::vector<Patch> patches = Patches(access, refs, house, false);
    state.RefineryCell = Refinery_Plot(house, terrain, unsafe, patches, state.RemoteRefinery);
    // A legal reachable refinery extension can serve the route without an MCV.
    if (Need_Core_Yard(house) || !state.RemoteRefinery || const_cast<HouseClass *>(house)->QuantityB(STRUCT_REFINERY) >= Minimum_Limit(house, house->AIRefineryLimit)) {
        TravelMap vehicles = terrain; vehicles.Search(Production_Sources(terrain, house, true));
        state.ExpansionCell = Expansion_Plot(house, vehicles, unsafe, Patches(vehicles, refs, house), -1, Need_Core_Yard(house));
    }
    AILog::Record(Frame, house->Class->House, "economy_scan", AILog::Fields().Boolean("income", state.Income)
        .Boolean("ore_available", state.OreAvailable).Boolean("remote_refinery", state.RemoteRefinery)
        .Number("refinery_cell", state.RefineryCell).Number("expansion_cell", state.ExpansionCell)
        .Number("dry_since", state.DrySince).Number("next_expansion", state.NextExpansion)
        .Number("factory_count", state.FactoryCount).Number("silo_count", state.SiloCount).Number("silo_goal", state.SiloGoal));
    return state;
}
static int Factory_Plot(HouseClass const * house, BuildingTypeClass const & type)
{
    TravelMap terrain(MAP_CELL_W, MAP_CELL_H); std::vector<unsigned char> unsafe;
    Terrain(terrain, unsafe, house); terrain.Search(Production_Sources(terrain, house));
    int best = -1, score = INT_MAX;
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        int entrance = New_Entrance(cell);
        if (!terrain.Valid(entrance) || terrain.Distance[entrance] < 0 || !Legal_Site(type, cell, unsafe)
            || !Map.Passes_Proximity_Check(&type, house->Class->House, type.Occupy_List(true), (CELL)cell)) continue;
        int value = terrain.Distance[entrance] * 20;
        for (int i = 0; i < Buildings.Count(); ++i) {
            BuildingClass const * building = Buildings.Ptr(i);
            if (!Own_Building(building, house)) continue;
            int d = Separation(cell, Coord_Cell(building->Center_Coord()));
            if (*building == STRUCT_REFINERY && d < 5) value += 3000;
            if (*building == STRUCT_WEAP && d < 4) value += 2000;
        }
        if (value < score) { best = cell; score = value; }
    }
    return best;
}
// Keep new storage away from loading/production entrances and preserve routes
// used by working harvesters. Evaluate connectivity only for a better candidate.
static int Storage_Plot(HouseClass const * house, BuildingTypeClass const & type)
{
    TravelMap terrain(MAP_CELL_W, MAP_CELL_H); std::vector<unsigned char> unsafe;
    Terrain(terrain, unsafe, house); terrain.Search(Production_Sources(terrain, house));
    std::vector<int> exits = Refinery_Sources(terrain, house);
    std::vector<int> factories = Production_Sources(terrain, house, true);
    exits.insert(exits.end(), factories.begin(), factories.end());
    std::vector<int> routes = exits;
    for (int i = 0; i < Units.Count(); ++i) {
        UnitClass const * truck = Units.Ptr(i);
        if (!Alive(truck) || truck->House != house || !truck->Class->IsToHarvest) continue;
        routes.push_back(Coord_Cell(truck->Center_Coord()));
        if (truck->Mission == MISSION_HARVEST && Target_Legal(truck->NavCom)) routes.push_back(As_Cell(truck->NavCom));
    }
    int best = -1, score = INT_MAX;
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        if (terrain.Distance[cell] < 0 || !Legal_Site(type, cell, unsafe)
            || !Map.Passes_Proximity_Check(&type, house->Class->House, type.Occupy_List(true), (CELL)cell)) continue;
        std::vector<int> body = Footprint(type, cell, false);
        bool blocked = false;
        for (unsigned int j = 0; j < body.size() && !blocked; ++j)
            for (unsigned int k = 0; k < exits.size(); ++k)
                if (Separation(body[j], exits[k]) <= 2) { blocked = true; break; }
        int value = terrain.Distance[cell] * 20;
        if (blocked || value >= score) continue;
        TravelMap placed = terrain;
        for (unsigned int j = 0; j < body.size(); ++j) placed.Clear[body[j]] = 0;
        placed.Search(Production_Sources(placed, house));
        for (unsigned int j = 0; j < routes.size(); ++j)
            if (terrain.Valid(routes[j]) && terrain.Distance[routes[j]] >= 0 && placed.Distance[routes[j]] < 0) { blocked = true; break; }
        if (!blocked) { best = cell; score = value; }
    }
    return best;
}
// A failed or successful search is reused until the ten-second terrain scan,
// or an infrastructure inventory change. Free construction decisions stay cheap.
static int Infrastructure_Plot(HouseClass const * house, BuildingTypeClass const & type)
{
    EconomyState & state = Scan(house);
    int & cell = type.Type == STRUCT_STORAGE ? state.StorageCell
        : type.Type == STRUCT_REFINERY ? state.BaseRefineryCell : state.FactoryCell;
    if (cell == -2) cell = type.Type == STRUCT_STORAGE ? Storage_Plot(house, type) : Factory_Plot(house, type);
    return cell;
}
static StructType Invest(HouseClass const * house, BuildingTypeClass const & type, int money, int reserve, int plot)
{
    if (!house->Can_Build(&type, house->ActLike) || (long long)money < (long long)type.Cost_Of() + reserve || plot < 0) return STRUCT_NONE;
    if ((long long)house->Power >= (long long)house->Drain + type.Drain) return type.Type;
    // Fund the additional power before choosing a building that would brown out.
    StructType const choices[] = {STRUCT_ADVANCED_POWER, STRUCT_POWER};
    for (int i = 0; i < 2; ++i) {
        BuildingTypeClass const & power = BuildingTypeClass::As_Reference(choices[i]);
        if (power.Power > 0 && house->Can_Build(&power, house->ActLike)
            && (long long)money >= (long long)power.Cost_Of() + type.Cost_Of() + reserve
            && Factory_Plot(house, power) >= 0) return power.Type;
    }
    return STRUCT_NONE;
}
}

namespace AIExpansion {
void Reset(void)
{
    for (int i = 0; i < HOUSE_COUNT; ++i) Economies[i] = EconomyState();
    Deployments.clear();
}
int Base_Limit(HouseClass const * house) { return house ? Minimum_Limit(house, house->AIMaxConYardsAndMCVs) : 0; }
void Forget(UnitClass const * unit) { if (unit) Deployments.erase(unit->As_Target()); }
bool Controls(UnitClass const * unit)
{
    return Alive(unit) && *unit == UNIT_MCV && Enabled(unit->House) && !unit->Team && !unit->IsTethered
        && (unit->Mission == MISSION_HUNT || unit->Mission == MISSION_GUARD || unit->Mission == MISSION_UNLOAD)
        && (unit->MissionQueue == MISSION_NONE || unit->MissionQueue == MISSION_HUNT
            || unit->MissionQueue == MISSION_GUARD || unit->MissionQueue == MISSION_UNLOAD);
}
}

void HouseClass::AI_Refresh_Economy(void)
{
    if (!Enabled(this)) return;
    EconomyState const & state = Scan(this);
    if (state.OreAvailable || state.Income) IsTiberiumShort = false;
    else if (state.DrySince >= 0 && Frame - state.DrySince >= TICKS_PER_MINUTE) IsTiberiumShort = true;
}

bool HouseClass::AI_Economic_Has_Income(void) const
{
    return Enabled(this) && Scan(this).Income;
}

void AIExpansion::Kennel_Sold(HouseClass const * house)
{
    if (Enabled(house)) Scan(house).KennelUntil = Frame + TICKS_PER_MINUTE * 2;
}

bool HouseClass::AI_Economic_Kennel_Ready(void) const
{
    if (!Enabled(this)) return true;
    return BQuantity[STRUCT_KENNEL] == 0 && BQuantity[STRUCT_WEAP] > 0
        && Power > Drain && AI_Economic_Has_Income() && Frame >= Scan(this).KennelUntil
        && AI_Economic_Cash() >= AI_Economic_Reserve() + BuildingTypeClass::As_Reference(STRUCT_KENNEL).Cost_Of();
}

bool HouseClass::AI_Economic_MCV_Ready(void)
{
    AI_Refresh_Economy();
    int guards = AIStrategy::Combat_Unit_Count(CurUnits, UQuantity[UNIT_HARVESTER], UQuantity[UNIT_MCV])
        + (int)CurInfantry / 4 + (int)CurAircraft / 2;
    if (!Enabled(this) || IsTiberiumShort || Power < Drain || BQuantity[STRUCT_REFINERY] == 0
        || UQuantity[UNIT_HARVESTER] < 2 || Working_Harvesters(this) < 2 || UQuantity[UNIT_MCV] > 0 || BuildUnit == UNIT_MCV
        || !Can_Build(&UnitTypeClass::As_Reference(UNIT_MCV), ActLike)
        || guards < 6
        || (int)BQuantity[STRUCT_CONST] + (int)UQuantity[UNIT_MCV] >= AIExpansion::Base_Limit(this)
        || (int)CurBuildings + 4 > AIMaxBuildings) return false;
    int budget = UnitTypeClass::As_Reference(UNIT_MCV).Cost_Of() + BuildingTypeClass::As_Reference(STRUCT_REFINERY).Cost_Of() + Reserve(this);
    if (AI_Economic_Cash() < budget) return false;
    EconomyState & state = Scan(this);
    if (!state.Income || state.ExpansionCell < 0 || Frame < state.NextExpansion) return false;
    state.NextExpansion = Frame + TICKS_PER_MINUTE * AIExpansion::ExpansionMinutes;
    return true;
}

StructType HouseClass::AI_Economic_Building(void)
{
    AI_Refresh_Economy();
    if (!Enabled(this) || Power < Drain) return STRUCT_NONE;
    BuildingTypeClass const & recovery = BuildingTypeClass::As_Reference(STRUCT_WEAP);
    int truck_reserve = UQuantity[UNIT_HARVESTER] == 0 && !IsTiberiumShort ? UnitTypeClass::As_Reference(UNIT_HARVESTER).Cost_Of() : 0;
    if (BQuantity[STRUCT_WEAP] == 0 && AIWarFactoryLimit > 0 && Can_Build(&recovery, ActLike)
        && AI_Economic_Cash() >= recovery.Cost_Of() + truck_reserve && Infrastructure_Plot(this, recovery) >= 0) return STRUCT_WEAP;
    if (IsTiberiumShort) return STRUCT_NONE;
    EconomyState & state = Scan(this);
    if (!state.Income || Working_Harvesters(this) == 0) return STRUCT_NONE;
    int money = AI_Economic_Cash(), reserve = AI_Economic_Reserve();
    BuildingTypeClass const & storage = BuildingTypeClass::As_Reference(STRUCT_STORAGE);
    bool full = Capacity > 0 && (long long)Tiberium * 100 >= (long long)Capacity * AIExpansion::StoragePercent;
    if (full || BQuantity[STRUCT_STORAGE] > 0) state.SiloGoal = AIExpansion::CoreMinimum;
    if (state.SiloGoal > 0 && Capacity > 0 && storage.Capacity > 0
        && ((int)BQuantity[STRUCT_STORAGE] < state.SiloGoal
            || (long long)Capacity - Tiberium < (long long)storage.Capacity * AIExpansion::StorageBuffer)) {
        if (money >= storage.Cost_Of() + reserve && Can_Build(&storage, ActLike)) {
            StructType investment = Invest(this, storage, money, reserve, Infrastructure_Plot(this, storage));
            if (investment != STRUCT_NONE) return investment;
        }
    }
    BuildingTypeClass const & refinery = BuildingTypeClass::As_Reference(STRUCT_REFINERY);
    bool late = Late_Economy(this);
    if (late && AIRefineryLimit > 0 && BQuantity[STRUCT_REFINERY] < AIExpansion::CoreMinimum
        && Can_Build(&refinery, ActLike) && (long long)money >= (long long)refinery.Cost_Of() + reserve) {
        int plot = state.RefineryCell >= 0 ? state.RefineryCell : Infrastructure_Plot(this, refinery);
        StructType investment = Invest(this, refinery, money, reserve, plot);
        if (investment != STRUCT_NONE) return investment;
    }
    BuildingTypeClass const & factory = BuildingTypeClass::As_Reference(STRUCT_WEAP);
    if (late && AIWarFactoryLimit > 0 && AIMaxTanksNr > 0 && Tank_Cost(this) > 0
        && BQuantity[STRUCT_WEAP] > 0 && BQuantity[STRUCT_WEAP] < AIExpansion::CoreMinimum
        && BQuantity[STRUCT_REFINERY] >= AIExpansion::CoreMinimum && Working_Harvesters(this) >= 2
        && Can_Build(&factory, ActLike)
        && (long long)money >= (long long)factory.Cost_Of() + reserve + Tank_Cost(this) * BQuantity[STRUCT_WEAP]) {
        StructType investment = Invest(this, factory, money, reserve + Tank_Cost(this) * BQuantity[STRUCT_WEAP], Infrastructure_Plot(this, factory));
        if (investment != STRUCT_NONE) return investment;
    }
    bool need_ref = state.RefineryCell >= 0 && (state.RemoteRefinery || BQuantity[STRUCT_REFINERY] < BQuantity[STRUCT_CONST]
        || UQuantity[UNIT_HARVESTER] > BQuantity[STRUCT_REFINERY] * 2
        || (BQuantity[STRUCT_REFINERY] == 1 && UQuantity[UNIT_HARVESTER] >= 2 && (int)CurUnits >= UQuantity[UNIT_HARVESTER] + 6));
    if ((int)BQuantity[STRUCT_REFINERY] < AIRefineryLimit && need_ref && Can_Build(&refinery, ActLike) && money >= refinery.Cost_Of() + reserve) return STRUCT_REFINERY;
    int count = BQuantity[STRUCT_WEAP];
    int speed_limit = NewUnitsEnabled && Rule.AIAftermathfastbuild == 0 ? 2 : AIWarFactoryLimit;
    bool tanks = AIPersonalStrategyMode == 1 || AIPersonalStrategyMode == 2 || AIPersonalStrategyMode == 6 || AIStrategy::Armor_Pressure(AI_Enemy_Forces());
    if (count >= 1 && count < AIWarFactoryLimit && count < speed_limit && tanks && Tank_Cost(this) > 0
        && (int)BQuantity[STRUCT_REFINERY] > count && (int)UQuantity[UNIT_HARVESTER] > count
        && AIStrategy::Combat_Unit_Count(CurUnits, UQuantity[UNIT_HARVESTER], UQuantity[UNIT_MCV]) + 6 < AIMaxTanksNr
        && Can_Build(&factory, ActLike) && (long long)Power >= (long long)Drain + factory.Drain
        && money >= factory.Cost_Of() + reserve + Tank_Cost(this) * count
        && Factory_Plot(this, factory) >= 0) return STRUCT_WEAP;
    BuildingTypeClass const & repair = BuildingTypeClass::As_Reference(STRUCT_REPAIR);
    int expansion_budget = UnitTypeClass::As_Reference(UNIT_MCV).Cost_Of() + refinery.Cost_Of() + reserve;
    if (BQuantity[STRUCT_REPAIR] == 0 && state.ExpansionCell >= 0
        && UQuantity[UNIT_MCV] == 0 && UQuantity[UNIT_HARVESTER] >= 2 && Working_Harvesters(this) >= 2
        && (int)BQuantity[STRUCT_CONST] < AIExpansion::Base_Limit(this) && (int)CurBuildings + 5 <= AIMaxBuildings
        && AIStrategy::Combat_Unit_Count(CurUnits, UQuantity[UNIT_HARVESTER], UQuantity[UNIT_MCV]) >= 6
        && Can_Build(&repair, ActLike) && money >= repair.Cost_Of() + expansion_budget
        && Factory_Plot(this, repair) >= 0) return STRUCT_REPAIR;
    return STRUCT_NONE;
}

int HouseClass::AI_Economic_Cash(void) const
{
    int money = Available_Money();
    if (!Enabled(this)) return money;
    // Subtract unpaid production commitments, including requests that have
    // been selected this tick but do not have a factory object yet.
    RTTIType const categories[] = {RTTI_UNITTYPE, RTTI_INFANTRYTYPE, RTTI_AIRCRAFTTYPE, RTTI_VESSELTYPE, RTTI_BUILDINGTYPE};
    int requests[] = {
        BuildUnit != UNIT_NONE ? UnitTypeClass::As_Reference(BuildUnit).Cost_Of() : 0,
        BuildInfantry != INFANTRY_NONE ? InfantryTypeClass::As_Reference(BuildInfantry).Cost_Of() : 0,
        BuildAircraft != AIRCRAFT_NONE ? AircraftTypeClass::As_Reference(BuildAircraft).Cost_Of() : 0,
        BuildVessel != VESSEL_NONE ? VesselTypeClass::As_Reference(BuildVessel).Cost_Of() : 0,
        BuildStructure != STRUCT_NONE ? BuildingTypeClass::As_Reference(BuildStructure).Cost_Of() : 0
    };
    for (int i = 0; i < 5; ++i) {
        FactoryClass const * factory = Fetch_Factory(categories[i]);
        if (factory) money -= factory->Remaining_Cost();
        if (!factory || !factory->Get_Object()) money -= max(0, requests[i]);
    }
    return max(0, money);
}

int HouseClass::AI_Economic_Combat_Budget(void) const
{
    int money = AI_Economic_Cash();
    if (Enabled(this) && (UQuantity[UNIT_MCV] > 0 || BuildUnit == UNIT_MCV))
        money -= BuildingTypeClass::As_Reference(STRUCT_REFINERY).Cost_Of() + BuildingTypeClass::As_Reference(STRUCT_POWER).Cost_Of();
    return max(0, money);
}

int HouseClass::AI_Economic_Reserve(void) const
{
    int reserve = Reserve(this);
    if (UQuantity[UNIT_MCV] > 0 || BuildUnit == UNIT_MCV) reserve += BuildingTypeClass::As_Reference(STRUCT_REFINERY).Cost_Of();
    return reserve;
}

COORDINATE HouseClass::AI_Economic_Location(BuildingClass const * building) const
{
    if (!Enabled(this) || !building) return 0;
    int cell = -1;
    if (*building == STRUCT_REFINERY) {
        cell = Scan(this, true).RefineryCell;
        if (cell < 0 && Late_Economy(this) && BQuantity[STRUCT_REFINERY] < AIExpansion::CoreMinimum) cell = Factory_Plot(this, *building->Class);
    }
    if (*building == STRUCT_WEAP) cell = Factory_Plot(this, *building->Class);
    if (*building == STRUCT_STORAGE) cell = Storage_Plot(this, *building->Class);
    return cell >= 0 ? Cell_Coord((CELL)cell) : 0;
}

// Called while Mark(MARK_UP) removes this unit from map occupancy.
// Native missions and Try_To_Deploy perform the actual turning and deployment.
bool UnitClass::MCV_Goto_Resource(void)
{
    if (!AIExpansion::Controls(this)) return false;
    DeployState & state = Deployments[As_Target()];
    if (state.Owner != this || Frame < state.LastFrame) { state = DeployState(); state.Owner = this; }
    state.LastFrame = Frame;
    int current = Coord_Cell(Center_Coord());
    if (current != state.Cell) { state.Cell = current; state.ProgressFrame = Frame; }
    if (state.Goal >= 0 && Frame - state.ProgressFrame >= TICKS_PER_SECOND * AIExpansion::StalledSeconds && !IsDriving) {
        state.FailedGoal = state.Goal; state.FailedUntil = Frame + TICKS_PER_MINUTE;
        state.Goal = -1; state.NextCheck = 0;
    }
    if (Frame >= state.NextCheck || state.Goal == current || state.Waypoint == current) {
        state.NextCheck = Frame + TICKS_PER_SECOND * AIExpansion::DeployCheckSeconds;
        TravelMap terrain(MAP_CELL_W, MAP_CELL_H); std::vector<unsigned char> unsafe;
        Terrain(terrain, unsafe, House); terrain.Clear[current] = 1; terrain.Search(current);
        TravelMap refs = terrain; refs.Search(Refinery_Sources(terrain, House));
        std::vector<Patch> patches = Patches(terrain, refs, House);
        BuildingTypeClass const & yard = BuildingTypeClass::As_Reference(STRUCT_CONST);
        bool initial = House->QuantityB(STRUCT_CONST) == 0;
        bool funded = House->QuantityB(STRUCT_REFINERY) > 0 && Working_Harvesters(House) > 0
            && House->QuantityB(STRUCT_CONST) < AIExpansion::Base_Limit(House)
            && House->AI_Economic_Cash() >= BuildingTypeClass::As_Reference(STRUCT_REFINERY).Cost_Of()
                + BuildingTypeClass::As_Reference(STRUCT_POWER).Cost_Of();
        if (!initial && !funded) patches.clear();
        int top = state.Goal - MAP_CELL_W - 1;
        bool valid = !state.Retreat && state.Goal >= 0 && terrain.Distance[state.Goal] >= 0 && Legal_Site(yard, top, unsafe);
        if (valid && !initial) {
            valid = false;
            for (unsigned int i = 0; i < patches.size(); ++i)
                if ((Need_Core_Yard(House) || patches[i].Distance < 0 || patches[i].Distance >= AIExpansion::RemoteRoute)
                    && Separation(state.Goal, patches[i].Cell) <= 10 && Followup_Plots(top, patches[i].Cell, terrain, unsafe)) { valid = true; break; }
        }
        if (!valid) {
            int failed = Frame < state.FailedUntil ? state.FailedGoal : -1;
            state.Retreat = false;
            state.Goal = Expansion_Plot(House, terrain, unsafe, patches, failed, Need_Core_Yard(House));
            // Establish/recover the first yard even on sparse-ore maps.
            if (initial && Legal_Site(yard, current - MAP_CELL_W - 1, unsafe)) state.Goal = current;
            if (initial && state.Goal < 0) {
                int distance = INT_MAX;
                for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
                    if (terrain.Distance[cell] < 0 || terrain.Distance[cell] > 12 || terrain.Distance[cell] >= distance
                        || (failed >= 0 && Separation(cell, failed) <= 3)) continue;
                    if (Legal_Site(yard, cell - MAP_CELL_W - 1, unsafe)) { state.Goal = cell; distance = terrain.Distance[cell]; }
                }
            }
            state.ProgressFrame = Frame;
        }
        // A newly threatened MCV must be allowed to escape its current danger
        // area; blocking every threatened tile would otherwise leave it trapped.
        if (state.Goal < 0 && unsafe[current]) {
            TravelMap escape(MAP_CELL_W, MAP_CELL_H);
            Terrain(escape, unsafe, House, false); escape.Clear[current] = 1; escape.Search(current);
            state.Goal = Retreat_Home(House, escape, unsafe);
            state.Retreat = state.Goal >= 0;
            if (state.Retreat) terrain = escape;
        }
        state.Route = Route_To(terrain, state.Goal);
        state.Waypoint = Route_Waypoint(terrain, state.Route);
        AILog::Record(Frame, House->Class->House, "mcv_deployment_plan", AILog::Fields().Number("unit", As_Target())
            .Number("current", current).Number("goal", state.Goal).Number("waypoint", state.Waypoint)
            .Number("route_length", state.Route.size()).Boolean("retreat", state.Retreat)
            .Boolean("initial_base", initial).Boolean("funded", funded).Number("patch_count", patches.size()));
    }
    if (state.Goal < 0) { if (Target_Legal(NavCom)) Assign_Destination(TARGET_NONE); return false; }
    if (state.Goal == current) {
        if (Target_Legal(NavCom)) Assign_Destination(TARGET_NONE);
        if (state.Retreat) return false;
        EconomyState & economy = Economies[House->Class->House];
        if (economy.Owner != House) { economy = EconomyState(); economy.Owner = House; }
        economy.NextExpansion = Frame + TICKS_PER_MINUTE * AIExpansion::ExpansionMinutes;
        return true;
    }
    if (state.Waypoint < 0 || state.Waypoint == current) return false;
    TARGET destination = ::As_Target((CELL)state.Waypoint);
    if (NavCom != destination) Assign_Destination(destination);
    return false;
}
