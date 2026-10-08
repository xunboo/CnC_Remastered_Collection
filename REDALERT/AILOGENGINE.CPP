// Copyright 2026. Distributed under the project's GPLv3 license.
#include "AILOGENGINE.H"
#include "LLMBRIDGE.H"
#include "FUNCTION.H"

namespace {
int NextWorldSnapshot = 0;
std::string LastMatchPath;
AILog::Fields House_State(HouseClass const * house) {
    return AILog::Fields().Number("house_id", house->Class->House).Boolean("human", house->IsHuman)
        .Boolean("active", house->IsActive).Boolean("defeated", house->IsDefeated)
        .Number("credits", house->Available_Money()).Number("power", house->Power).Number("drain", house->Drain)
        .Number("build_structure", house->BuildStructure).Number("build_unit", house->BuildUnit)
        .Number("build_infantry", house->BuildInfantry).Number("build_aircraft", house->BuildAircraft)
        .Number("build_vessel", house->BuildVessel).Number("strategy", house->AIPersonalStrategyMode)
        .Number("units", house->CurUnits).Number("infantry", house->CurInfantry).Number("aircraft", house->CurAircraft)
        .Number("buildings", house->CurBuildings).Number("enemy_house", house->Enemy)
        .Boolean("ore_short", house->IsTiberiumShort).Boolean("unit_in_production", house->UnitInProduction)
        .Boolean("building_in_production", house->BuildingInProduction)
        .Number("iq", house->IQ).Number("tank_limit", house->AIMaxTanksNr).Number("building_limit", house->AIMaxBuildings);
}
TechnoClass const * Techno(MissionClass const * object) {
    if (!object) return NULL;
    RTTIType type = object->What_Am_I();
    return type == RTTI_UNIT || type == RTTI_INFANTRY || type == RTTI_AIRCRAFT || type == RTTI_VESSEL
        || type == RTTI_BUILDING ? static_cast<TechnoClass const *>(object) : NULL;
}
AILog::Fields Object_State(TechnoClass const * object) {
    int cell = Coord_Cell(object->Center_Coord());
    AILog::Fields state;
    state.Number("target", object->As_Target()).Number("generation", LLMBridge::Object_Generation(object->As_Target()))
        .Number("house_id", object->House->Class->House).Text("type", object->Techno_Type_Class()->IniName)
        .Number("rtti", object->What_Am_I()).Number("cell_x", cell % MAP_CELL_W).Number("cell_y", cell / MAP_CELL_W)
        .Number("health", object->Strength).Number("max_health", object->Techno_Type_Class()->MaxStrength)
        .Number("cost", object->Techno_Type_Class()->Cost_Of())
        .Number("ammo", object->Ammo).Number("mission", object->Mission).Text("mission_name", MissionClass::Mission_Name(object->Mission))
        .Number("mission_queue", object->MissionQueue).Number("status", object->Status)
        .Number("attack_target", object->TarCom).Boolean("active", object->IsActive).Boolean("limbo", object->IsInLimbo);
    if (object->Is_Foot()) state.Number("destination", static_cast<FootClass const *>(object)->NavCom)
        .Boolean("scripted_team", static_cast<TeamClass *>(static_cast<FootClass const *>(object)->Team) != NULL);
    return state;
}
}
void AILog::Simulation_Tick() {
    Fields context;
    context.Text("scenario", Scen.ScenarioName).Number("session_type", Session.Type)
        .Number("ticks_per_second", TICKS_PER_SECOND).Number("map_x", Map.MapCellX).Number("map_y", Map.MapCellY)
        .Number("map_width", Map.MapCellWidth).Number("map_height", Map.MapCellHeight);
    Begin(Frame, context);
    if (!Enabled()) return;
    std::string path = Path();
    if (LastMatchPath != path) { LastMatchPath = path; NextWorldSnapshot = 0; }
    if (Frame < NextWorldSnapshot) return;
    NextWorldSnapshot = Frame + TICKS_PER_SECOND;
    std::string houses = "[", units = "[";
    for (int i = 0; i < HOUSE_COUNT; ++i) {
        HouseClass const * house = HouseClass::As_Pointer(static_cast<HousesType>(i));
        if (!house) continue;
        if (houses.size() > 1) houses += ',';
        houses += House_State(house).Json();
    }
    for (int kind = 0; kind < 5; ++kind) {
        int count = kind == 0 ? Units.Count() : kind == 1 ? Infantry.Count() : kind == 2 ? Vessels.Count()
            : kind == 3 ? Aircraft.Count() : Buildings.Count();
        for (int i = 0; i < count; ++i) {
            TechnoClass const * object = kind == 0 ? static_cast<TechnoClass *>(Units.Ptr(i))
                : kind == 1 ? static_cast<TechnoClass *>(Infantry.Ptr(i)) : kind == 2 ? static_cast<TechnoClass *>(Vessels.Ptr(i))
                : kind == 3 ? static_cast<TechnoClass *>(Aircraft.Ptr(i)) : static_cast<TechnoClass *>(Buildings.Ptr(i));
            if (!object || !object->IsActive || !object->House) continue;
            if (units.size() > 1) units += ',';
            units += Object_State(object).Json();
        }
    }
    Record(Frame, -1, "world_snapshot", context.Raw("houses", houses + ']').Raw("objects", units + ']'));
}
void AILog::Object_Order(MissionClass const * actor, char const * event, unsigned int requested) {
    if (!Enabled()) return;
    TechnoClass const * object = Techno(actor);
    if (!object || !object->House || object->House->IsHuman) return;
    Record(Frame, object->House->Class->House, event, Object_State(object).Number("requested", requested));
}
void AILog::House_Event(HouseClass const * house, char const * event, Fields const & details) {
    if (Enabled() && house && !house->IsHuman)
        Record(Frame, house->Class->House, event, Fields().Raw("state", House_State(house).Json()).Raw("details", details.Json()));
}
AILog::HouseScope::HouseScope(HouseClass const * house, char const * policy) : House(house), Policy(policy) {
    if (Enabled() && House && !House->IsHuman) Before = House_State(House).Json();
}
AILog::HouseScope::~HouseScope() {
    if (!Before.empty()) Record(Frame, House->Class->House, Policy, Fields().Raw("before", Before).Raw("after", House_State(House).Json()));
}
AILog::MissionScope::MissionScope(MissionClass const * actor) : Target(0), Generation(0), House(-1) {
    if (!Enabled()) return;
    TechnoClass const * object = Techno(actor);
    if (!object || !object->House || object->House->IsHuman) return;
    Target = object->As_Target(); Generation = LLMBridge::Object_Generation(Target); House = object->House->Class->House;
    Before = Object_State(object).Json();
}
AILog::MissionScope::~MissionScope() {
    if (Before.empty()) return;
    TechnoClass const * object = As_Techno(Target);
    std::string after = object && object->IsActive && LLMBridge::Object_Generation(Target) == Generation
        ? Object_State(object).Json() : "null";
    Record(Frame, House, "mission_evaluation", Fields().Raw("before", Before).Raw("after", after));
}
