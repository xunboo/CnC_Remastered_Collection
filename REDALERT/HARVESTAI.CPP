// Copyright 2026. Distributed under the project's GPLv3 license.
#include "AILOG.H"
#include "FUNCTION.H"
#include "HARVESTAI.H"
#include <map>
#include <climits>

namespace {
using HarvestAI::TravelMap;
struct HarvestState {
    UnitClass const * Owner;
    int LastFrame, NextCheck, ProgressFrame, Cell;
    TARGET Destination;
    HarvestState() : Owner(NULL), LastFrame(-1), NextCheck(0), ProgressFrame(0), Cell(-1), Destination(TARGET_NONE) {}
};
std::map<TARGET, HarvestState> HarvestStates;

static bool Harvest_Alive(UnitClass const * unit)
{
    return unit && unit->IsActive && !unit->IsInLimbo && unit->Strength > 0 && unit->Class->IsToHarvest;
}
static bool Harvest_Automatic(UnitClass const * unit)
{
    return Harvest_Alive(unit) && !unit->Team && !unit->IsTethered && !unit->IsDumping
        && unit->Mission == MISSION_HARVEST
        && (unit->MissionQueue == MISSION_NONE || unit->MissionQueue == MISSION_HARVEST);
}
static void Harvest_Routes(TravelMap & field, UnitClass const * unit)
{
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        if (!Map.In_Radar((CELL)cell) || Map[(CELL)cell].Cell_Building()) continue;
        OverlayType overlay = Map[(CELL)cell].Overlay;
        if (overlay != OVERLAY_NONE && OverlayTypeClass::As_Reference(overlay).IsWall) continue;
        field.Clear[cell] = Map[(CELL)cell].Is_Clear_To_Move(unit->Class->Speed, true, true, -1, unit->Class->MZone) ? 1 : 0;
    }
    field.Search(Coord_Cell(unit->Center_Coord()));
}
static bool Harvest_Refinery_Valid(BuildingClass const * refinery, UnitClass const * unit)
{
    return refinery && refinery->IsActive && !refinery->IsInLimbo && refinery->Strength > 0
        && refinery->House == unit->House && *refinery == STRUCT_REFINERY
        && refinery->Mission != MISSION_CONSTRUCTION && refinery->Mission != MISSION_DECONSTRUCTION
        && refinery->BState != BSTATE_CONSTRUCTION;
}
static CELL Harvest_Entrance(BuildingClass const * refinery)
{
    // Match the refinery's RADIO_DOCKING command, rather than its blocked
    // center or an arbitrary nearby edge of the footprint.
    return Coord_Cell(Adjacent_Cell(refinery->Center_Coord(), DIR_S));
}
static bool Harvest_Busy(BuildingClass const * refinery, UnitClass const * unit)
{
    return refinery->Is_Something_Attached()
        || (refinery->In_Radio_Contact() && refinery->Contact_With_Whom() != unit);
}
static int Harvest_Queue(BuildingClass const * refinery, UnitClass const * unit)
{
    int queue = Harvest_Busy(refinery, unit) ? 1 : 0;
    for (int i = 0; i < Units.Count(); ++i) {
        UnitClass const * other = Units.Ptr(i);
        if (!Harvest_Alive(other) || other == unit || other->House != unit->House || other->Tiberium <= 0) continue;
        if (refinery->Contact_With_Whom() == other) continue; // Already counted as busy.
        if ((other->Mission == MISSION_HARVEST && (other->Status == 2 || other->Status == 3))
            || other->Mission == MISSION_ENTER || other->MissionQueue == MISSION_ENTER) {
            if (other->TiberiumUnloadRefinery == refinery->As_Target()
                || other->Contact_With_Whom() == refinery || other->NavCom == refinery->As_Target()) ++queue;
        }
    }
    return queue;
}
static int Harvest_Wait_Weight(void)
{
    // The legacy CFE setup converts this setting to leptons when native
    // A-star is disabled. These routes consistently use cells in both modes.
    int wait = Rule.HarvyOptimizeUnloadWaitWeight;
    if (!Rule.AstarPathFindingEnabled) wait /= CELL_LEPTON_W;
    return HarvestAI::Clamp(wait, 1, 64);
}
static int Harvest_Ore_Yield(CELL cell)
{
    // A native harvest step lifts one ore level, but four gem bails. Compare
    // that return before the stockpile size so a sparse gem cell still wins
    // over a thick ordinary ore cell at a similar reachable distance.
    switch (Map[cell].Overlay) {
        case OVERLAY_GOLD1:
        case OVERLAY_GOLD2:
        case OVERLAY_GOLD3:
        case OVERLAY_GOLD4:
            return HarvestAI::Clamp(Rule.GoldValue, 0, 10000000);
        case OVERLAY_GEMS1:
        case OVERLAY_GEMS2:
        case OVERLAY_GEMS3:
        case OVERLAY_GEMS4:
            return HarvestAI::Clamp(Rule.GemValue, 0, 2500000) * 4;
        default:
            return 0;
    }
}
static void Harvest_Reservations(UnitClass const * unit, TravelMap const & field, std::vector<int> & claims)
{
    for (int i = 0; i < Units.Count(); ++i) {
        UnitClass const * other = Units.Ptr(i);
        if (!Harvest_Alive(other) || other == unit || other->House != unit->House
            || other->Mission != MISSION_HARVEST || other->Status > 1 || other->IsTethered) continue;
        CELL destination = Target_Legal(other->NavCom) ? As_Cell(other->NavCom) : Coord_Cell(other->Center_Coord());
        if (!Map.In_Radar(destination)) continue;
        int x = destination % MAP_CELL_W, y = destination / MAP_CELL_W;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= MAP_CELL_W || ny < 0 || ny >= MAP_CELL_H) continue;
                int cell = ny * MAP_CELL_W + nx;
                if (field.Distance[cell] >= 0) ++claims[cell];
            }
        }
    }
}
}

void HarvestAI::Reset(void)
{
    HarvestStates.clear();
}
void HarvestAI::Forget(UnitClass const * unit)
{
    HarvestStates.erase(unit->As_Target());
}

bool UnitClass::Harvest_Goto_Ore(int radius)
{
    if (Target_Legal(NavCom)) return false; // Preserve an active ore order.
    CELL source = Coord_Cell(Center_Coord());
    if (!Map.In_Radar(source)) return false;
    bool on_ore = Map[source].Land_Type() == LAND_TIBERIUM;
    if (on_ore) House->IsTiberiumShort = false;
    if (radius <= 1) return on_ore;
    radius = HarvestAI::Clamp(radius - 1, 1, MAP_CELL_W + MAP_CELL_H);
    TravelMap field(MAP_CELL_W, MAP_CELL_H);
    Harvest_Routes(field, this);
    std::vector<int> claims(MAP_CELL_TOTAL, 0), values(MAP_CELL_TOTAL, 0);
    Harvest_Reservations(this, field, claims);
    int nearest = INT_MAX;
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        if (field.Distance[cell] < 0 || field.Separation(source, cell) > radius) continue;
        CELL candidate = (CELL)cell;
        int value = Tiberium_Check(candidate, 0, 0);
        if (value <= 0) continue;
        values[cell] = value;
        int distance = field.Distance[cell] + HarvestAI::Clamp(claims[cell] * 2, 0, 12);
        if (distance < nearest) nearest = distance;
    }
    if (nearest == INT_MAX) return on_ore;
    int best = -1, best_score = -1, best_yield = -1, best_distance = INT_MAX;
    int source_score = 0, source_yield = 0;
    int band = nearest + HarvestAI::Nearby_Band(nearest);
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        if (values[cell] <= 0) continue;
        int distance = field.Distance[cell] + HarvestAI::Clamp(claims[cell] * 2, 0, 12);
        if (distance > band) continue;
        int surrounding = 0, x = cell % MAP_CELL_W, y = cell / MAP_CELL_W;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= MAP_CELL_W || ny < 0 || ny >= MAP_CELL_H) continue;
                int neighbor = ny * MAP_CELL_W + nx;
                if (field.Distance[neighbor] >= 0) surrounding += HarvestAI::Clamp(values[neighbor], 0, 1000000);
            }
        }
        int score = HarvestAI::Ore_Score(values[cell], surrounding, claims[cell]);
        int yield = HarvestAI::Ore_Score(Harvest_Ore_Yield((CELL)cell), 0, claims[cell]);
        if (cell == source) { source_score = score; source_yield = yield; }
        if (best < 0 || yield > best_yield
            || (yield == best_yield && (score > best_score || (score == best_score && distance < best_distance)))) {
            best = cell; best_score = score; best_yield = yield; best_distance = distance;
        }
    }
    if (best < 0 || best == source) return on_ore;
    // Keep harvesting comparable cells instead of continually moving between
    // them. A higher-yield resource wins immediately; equal-yield density
    // changes must exceed both a 25% margin and one harvest step's value.
    if (on_ore && best_yield <= source_yield
        && best_score <= source_score + max(source_score / 4, source_yield)) return true;
    if (best >= 0) {
        AILog::Record(Frame, House->Class->House, "harvester_ore_selected", AILog::Fields().Number("unit", As_Target())
            .Number("source", source).Number("destination", best).Number("score", best_score).Number("distance", best_distance).Number("nearby_band", band)
            .Number("route_distance", field.Distance[best]).Number("ore_value", values[best]).Number("yield_per_step", Harvest_Ore_Yield((CELL)best))
            .Number("claims", claims[best]).Number("source_value", values[source]).Number("source_yield_per_step", Harvest_Ore_Yield(source))
            .Text("reason", !on_ore ? "best_nearby_ore" : best_yield > source_yield ? "higher_harvest_yield" : "richer_nearby_cell"));
        House->IsTiberiumShort = false;
        Assign_Destination(::As_Target((CELL)best));
    }
    return false;
}

BuildingClass * UnitClass::Harvest_Find_Refinery(void) const
{
    if (!Harvest_Alive(this)) return NULL;
    BuildingClass * contact = In_Radio_Contact() && Contact_With_Whom()->What_Am_I() == RTTI_BUILDING
        ? (BuildingClass *)Contact_With_Whom() : NULL;
    if (Harvest_Refinery_Valid(contact, this)) return contact; // Finish accepted docking.
    TravelMap field(MAP_CELL_W, MAP_CELL_H);
    Harvest_Routes(field, this);
    BuildingClass * best = NULL, * current = NULL;
    int best_score = INT_MAX, current_score = INT_MAX, best_queue = INT_MAX, current_queue = INT_MAX;
    int wait = Harvest_Wait_Weight();
    for (int i = 0; i < Buildings.Count(); ++i) {
        BuildingClass * refinery = Buildings.Ptr(i);
        if (!Harvest_Refinery_Valid(refinery, this)) continue;
        CELL entrance = Harvest_Entrance(refinery);
        if (!Map.In_Radar(entrance) || field.Distance[entrance] < 0) continue;
        int queue = Harvest_Queue(refinery, this);
        int score = HarvestAI::Refinery_Score(field.Distance[entrance], queue, wait);
        if (refinery->As_Target() == TiberiumUnloadRefinery) {
            current = refinery; current_score = score; current_queue = queue;
        }
        if (!best || score < best_score || (score == best_score && queue < best_queue)) {
            best = refinery; best_score = score; best_queue = queue;
        }
    }
    // Avoid bouncing between almost-equivalent refineries. A newly free
    // refinery can still win over a busy current choice.
    if (current && current_queue == best_queue && current_score <= best_score + 2) {
        AILog::Record(Frame, House->Class->House, "harvester_refinery_selected", AILog::Fields().Number("unit", As_Target())
            .Number("refinery", current->As_Target()).Number("score", current_score).Number("queue", current_queue).Text("reason", "preserve_close_existing_choice"));
        return current;
    }
    AILog::Record(Frame, House->Class->House, "harvester_refinery_selected", AILog::Fields().Number("unit", As_Target())
        .Number("refinery", best ? best->As_Target() : 0).Number("score", best_score).Number("queue", best_queue).Text("reason", best ? "best_reachable_queue" : "no_valid_refinery"));
    return best;
}

CELL UnitClass::Harvest_Wait_Cell(BuildingClass const * refinery) const
{
    TravelMap field(MAP_CELL_W, MAP_CELL_H);
    Harvest_Routes(field, this);
    CELL entrance = Harvest_Entrance(refinery);
    int best = -1, score = INT_MAX;
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        int separation = field.Separation(cell, entrance);
        TechnoClass const * occupant = Map[(CELL)cell].Cell_Techno();
        if (field.Distance[cell] < 0 || separation < 2 || separation > 4
            || (occupant && occupant != this) || Map[(CELL)cell].Land_Type() == LAND_TIBERIUM) continue;
        int occupied = 0;
        for (int i = 0; i < Units.Count(); ++i) {
            UnitClass const * other = Units.Ptr(i);
            if (Harvest_Alive(other) && other != this && other->NavCom == ::As_Target((CELL)cell)) ++occupied;
        }
        int value = field.Distance[cell] + separation * 2 + occupied * 12;
        if (value < score) { best = cell; score = value; }
    }
    // If there is no free holding square, stay put and retry instead of
    // entering an unreachable footprint or blocking the unloading pad.
    return best >= 0 ? (CELL)best : Coord_Cell(Center_Coord());
}

bool UnitClass::Harvest_Retreat(TechnoClass const * attacker)
{
    if (!Rule.HarvesterOptimizeEnabled || !Harvest_Alive(this) || Team || IsTethered || IsDumping
        || !attacker || House->Is_Ally(attacker) || Tiberium <= 0 || Health_Ratio() > Rule.ConditionRed) return false;
    if (Mission == MISSION_ENTER && In_Radio_Contact()
        && Contact_With_Whom()->What_Am_I() == RTTI_BUILDING
        && Harvest_Refinery_Valid((BuildingClass *)Contact_With_Whom(), this)) return true;
    if (!Harvest_Automatic(this)) return false;
    BuildingClass * refinery = Harvest_Find_Refinery();
    if (!refinery) return false;
    AILog::Record(Frame, House->Class->House, "harvester_retreat", AILog::Fields().Number("unit", As_Target())
        .Number("attacker", attacker->As_Target()).Number("refinery", refinery->As_Target()).Number("health", Strength).Number("cargo", Tiberium));
    if (AttackMove) ResetAttackMove();
    IsHarvesting = false;
    Clear_Navigation_List();
    Assign_Target(TARGET_NONE);
    Assign_Destination(TARGET_NONE);
    ArchiveTarget = TARGET_NONE;
    TiberiumUnloadRefinery = refinery->As_Target();
    // Keep the native harvest/enter/unload state machine even for partial
    // loads. Do not force entry while another truck occupies the refinery.
    Status = 2; // FINDHOME
    if (Transmit_Message(RADIO_HELLO, refinery) == RADIO_ROGER) {
        Assign_Mission(MISSION_ENTER);
    } else {
        Assign_Destination(::As_Target(Harvest_Wait_Cell(refinery)));
    }
    return true;
}

void UnitClass::Harvest_Think(void)
{
    if (!Rule.HarvesterOptimizeEnabled || !Harvest_Automatic(this)) return;
    HarvestState & state = HarvestStates[As_Target()];
    CELL cell = Coord_Cell(Center_Coord());
    if (state.Owner != this || Frame < state.LastFrame) {
        state = HarvestState(); state.Owner = this;
    }
    state.LastFrame = Frame;
    if (state.Cell != cell || state.Destination != NavCom) {
        state.Cell = cell; state.Destination = NavCom; state.ProgressFrame = Frame;
    }
    if (Frame < state.NextCheck) return;
    state.NextCheck = Frame + TICKS_PER_SECOND * 3;
    if (Status == 2) {
        ReconsiderRefinery();
    } else if ((Status == 0 || Status == 1) && !Target_Legal(NavCom) && !IsDriving
        && Tiberium_Load() < 1 && Map.In_Radar(cell) && Map[cell].Land_Type() == LAND_TIBERIUM) {
        // Notice richer nearby resources while mining, without waiting for
        // the current cell to run out. Preserve partial cargo and resume the
        // native LOOKING -> HARVESTING sequence after reaching the new cell.
        if (!Harvest_Goto_Ore(Rule.TiberiumShortScan / CELL_LEPTON_W) && Target_Legal(NavCom)) {
            IsHarvesting = false;
            ArchiveTarget = TARGET_NONE;
            Status = 0; // LOOKING
            Set_Stage(0);
            Set_Rate(0);
        }
    } else if ((Status == 0 || Status == 1) && Target_Legal(NavCom) && Frame - state.ProgressFrame >= TICKS_PER_SECOND * 10) {
        // Re-evaluate a depleted or blocked autonomous ore destination. A
        // moving truck and a queued player command keep their existing order.
        if (!IsDriving) {
            Assign_Destination(TARGET_NONE);
            ArchiveTarget = TARGET_NONE;
            state.ProgressFrame = Frame;
        }
    }
}
