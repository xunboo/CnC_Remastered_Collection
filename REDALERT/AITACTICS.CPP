// Copyright 2026. Distributed under the project's GPLv3 license.
// Read standard stream headers before the engine's legacy character macros.
#include <sstream>
#include <iomanip>
#include "LLMBRIDGE.H"
#include "AILOG.H"
#include "FUNCTION.H"
#include "AITACTICS.H"
#include "AISTRATEGY.H"
#include <algorithm>
#include <climits>
#include <map>
#include <deque>

namespace {
using AITactics::Clamp;
using AITactics::RouteMap;

enum StrikePhase { STRIKE_HOLD, STRIKE_RALLY, STRIKE_ADVANCE, STRIKE_ENGAGE };
enum StrikeIntent { STRIKE_NORMAL, STRIKE_ECONOMY, STRIKE_POWER, STRIKE_BASE };
struct Fighter {
    FootClass * Object;
    int Cell;
    int Power;
    bool Naval;
    bool Air;
};
struct Contact {
    TechnoClass * Object;
    int Owner;
    int Cell;
    int Power;
    int Range;
    bool Mobile;
    bool Naval;
    bool Air;
};
struct StrikePlan {
    StrikePhase Phase;
    TARGET Target;
    int Enemy;
    int Home;
    int Waypoint;
    int Step;
    int ProgressFrame;
    int NextAttempt;
    int GatheredPower;
    int TargetScore;
    int RetargetFrame;
    bool AllOut;
    bool Forced;
    StrikeIntent Intent;
    bool Fighting;
    bool Resume;
    bool External;
    TARGET BattleTarget;
    std::vector<TARGET> Members;
    std::vector<int> Route;
    RouteMap Field;
    StrikePlan() : Phase(STRIKE_HOLD), Target(TARGET_NONE), Enemy(-1), Home(0),
        Waypoint(0), Step(0), ProgressFrame(0), NextAttempt(0), GatheredPower(0), TargetScore(0), RetargetFrame(0), AllOut(false), Forced(false), Intent(STRIKE_NORMAL), Fighting(false), Resume(false), External(false), BattleTarget(TARGET_NONE), Field(MAP_CELL_W, MAP_CELL_H) {}
};
struct TacticalState {
    HouseClass * Owner;
    int LastFrame;
    int NextUpdate;
    int NextScan;
    int NextAttack;
    int NextAirRaid;
    int NextForcedAttempt;
    bool NumericalAssault;
    int NextContactCheck;
    int NextAllOutRoll;
    int NextAllOutStrike;
    int AttackedCell;
    int AttackedFrame;
    int DefenseCell;
    int ThreatPower;
    bool AirThreat;
    bool DefenseReady;
    std::vector<TARGET> DefenseTargets;
    std::vector<TARGET> Defenders;
    StrikePlan Ground;
    StrikePlan Fleet;
    StrikePlan Air;
    TacticalState() : Owner(NULL), LastFrame(-1), NextUpdate(0), NextScan(0), NextAttack(0), NextAirRaid(0), NextForcedAttempt(0), NumericalAssault(false), NextContactCheck(0), NextAllOutRoll(0), NextAllOutStrike(0),
        AttackedCell(0), AttackedFrame(-1), DefenseCell(0), ThreatPower(0), AirThreat(false), DefenseReady(true) {}
};
TacticalState TacticalStates[HOUSE_COUNT];

static bool LLM_Group_Controlled(HouseClass const * house, int group);
static bool LLM_Has_Member(HouseClass const * house, TARGET target);

static TacticalState & Tactical_State(HouseClass * house)
{
    TacticalState & state = TacticalStates[house->Class->House];
    if (state.Owner != house || Frame < state.LastFrame) {
        state = TacticalState();
        state.Owner = house;
    }
    state.LastFrame = Frame;
    return state;
}

static bool Opponent(HouseClass const * house, HouseClass const * other)
{
    return other && other != house && other->IsActive && !other->IsDefeated
        && other->Class->House != HOUSE_NEUTRAL && other->Class->House != HOUSE_JP
        && !house->Is_Ally(other);
}

static bool Alive(TechnoClass const * object)
{
    return object && object->IsActive && !object->IsInLimbo && object->Strength > 0
        && Map.In_Radar(Coord_Cell(object->Center_Coord()));
}

static WeaponTypeClass const * Weapon_Against(TechnoClass const * object, bool air)
{
    TechnoTypeClass const * type = object->Techno_Type_Class();
    WeaponTypeClass const * weapons[] = {type->PrimaryWeapon, type->SecondaryWeapon};
    for (int i = 0; i < 2; ++i) {
        WeaponTypeClass const * weapon = weapons[i];
        if (weapon && weapon->Bullet && weapon->Attack > 0
            && (air ? weapon->Bullet->IsAntiAircraft : weapon->Bullet->IsAntiGround)) return weapon;
    }
    return NULL;
}

static bool Armed(TechnoClass const * object)
{
    return Weapon_Against(object, false) != NULL;
}

static int Power(TechnoClass const * object)
{
    if (!Weapon_Against(object, false) && !Weapon_Against(object, true)) return 0;
    return AITactics::Combat_Power(object->Techno_Type_Class()->Cost_Of(),
        object->Strength, object->Techno_Type_Class()->MaxStrength);
}

static bool Available(FootClass const * object, HouseClass const * house)
{
    if (!Alive(object) || (HouseClass const *)object->House != house || object->Team || (object->IsTethered && object->What_Am_I() != RTTI_AIRCRAFT) || object->Techno_Type_Class()->MaxSpeed <= MPH_IMMOBILE || !Armed(object)) return false;
    if (object->What_Am_I() == RTTI_AIRCRAFT) {
        AircraftClass const * aircraft = (AircraftClass const *)object;
        // Docked aircraft remain tethered to the pad. A full magazine, not
        // detachment from the pad, is the condition for a new sortie.
        if (aircraft->Ammo == 0 || (aircraft->Height == 0 && aircraft->Class->MaxAmmo > 0
            && aircraft->Ammo < aircraft->Class->MaxAmmo)) return false;
    }
    if (object->Mission == MISSION_ENTER || object->Mission == MISSION_RETREAT
        || object->Mission == MISSION_CAPTURE || object->Mission == MISSION_UNLOAD) return false;
    if (object->What_Am_I() == RTTI_UNIT) {
        UnitType type = ((UnitClass const *)object)->Class->Type;
        if (type == UNIT_HARVESTER || type == UNIT_MCV || type == UNIT_MINELAYER
            || type == UNIT_MAD || type == UNIT_DEMOTRUCK) return false;
    }
    return true;
}

// Count living armed mobile units symmetrically, including scripted or rearming
// units. Buildings, transports, harvesters and construction vehicles are not
// an army. Availability separately protects native teams, docking and missions.
static bool Combat_Mobile(TechnoClass const * object)
{
    if (!object->Is_Foot() || object->Techno_Type_Class()->MaxSpeed <= MPH_IMMOBILE
        || (!Weapon_Against(object, false) && !Weapon_Against(object, true))) return false;
    if (object->What_Am_I() == RTTI_UNIT) {
        UnitType type = ((UnitClass const *)object)->Class->Type;
        if (type == UNIT_HARVESTER || type == UNIT_MCV || type == UNIT_MINELAYER) return false;
    }
    return true;
}

static void Gather(HouseClass * house, std::vector<Fighter> & friendly, std::vector<Contact> & enemy,
    int * friendly_count = NULL, int * enemy_count = NULL)
{
    if (friendly_count) *friendly_count = 0;
    if (enemy_count) *enemy_count = 0;
    for (int kind = 0; kind < 5; ++kind) {
        int count = kind == 0 ? Units.Count() : kind == 1 ? Infantry.Count()
            : kind == 2 ? Vessels.Count() : kind == 3 ? Aircraft.Count() : Buildings.Count();
        for (int index = 0; index < count; ++index) {
            TechnoClass * object = kind == 0 ? (TechnoClass *)Units.Ptr(index)
                : kind == 1 ? (TechnoClass *)Infantry.Ptr(index)
                : kind == 2 ? (TechnoClass *)Vessels.Ptr(index)
                : kind == 3 ? (TechnoClass *)Aircraft.Ptr(index) : (TechnoClass *)Buildings.Ptr(index);
            if (!Alive(object)) continue;
            int power = Power(object);
            if (Combat_Mobile(object)) {
                if ((HouseClass const *)object->House == house && friendly_count) ++*friendly_count;
                else if (Opponent(house, object->House) && enemy_count) ++*enemy_count;
            }
            if (kind < 4 && Available((FootClass *)object, house)
                && (kind != 3 || ((AircraftClass *)object)->Ammo != 0)) {
                Fighter fighter = {(FootClass *)object, Coord_Cell(object->Center_Coord()), power, kind == 2, kind == 3};
                friendly.push_back(fighter);
            }
            if (!Opponent(house, object->House)) continue;
            if (kind == 3 && power == 0) continue;
            WeaponTypeClass const * weapon = Weapon_Against(object, false);
            if (!weapon) weapon = Weapon_Against(object, true);
            Contact contact = {object, object->House->Class->House, Coord_Cell(object->Center_Coord()),
                power, weapon ? Clamp(weapon->Range / CELL_LEPTON_W, 1, 20) : 0,
                kind != 4, kind == 2, kind == 3};
            enemy.push_back(contact);
        }
    }
}

static void Fill_Routes(RouteMap & field, FootClass const * mover, int source)
{
    TechnoTypeClass const * type = mover->Techno_Type_Class();
    field.Clear.assign(MAP_CELL_TOTAL, 0);
    field.Danger.assign(MAP_CELL_TOTAL, 0);
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        if (!Map.In_Radar((CELL)cell)) continue;
        if (type->Speed == SPEED_WINGED) { field.Clear[cell] = 1; continue; }
        if (Map[(CELL)cell].Cell_Building()) continue;
        // Ignore mobile occupancy, but never treat a wall as an open route.
        // Movement zones alone do not account for buildings blocking a corridor.
        OverlayType overlay = Map[(CELL)cell].Overlay;
        if (overlay != OVERLAY_NONE && OverlayTypeClass::As_Reference(overlay).IsWall) continue;
        field.Clear[cell] = Map[(CELL)cell].Is_Clear_To_Move(type->Speed, true, true, -1, type->MZone) ? 1 : 0;
    }
    field.Search(source);
}

static FootClass * Member(TARGET target, HouseClass * house)
{
    TechnoClass * object = As_Techno(target);
    if (!Alive(object) || !object->Is_Foot() || !Available((FootClass *)object, house)) return NULL;
    if (object->What_Am_I() == RTTI_AIRCRAFT && ((AircraftClass *)object)->Ammo == 0) return NULL;
    return (FootClass *)object;
}

static bool Contains(std::vector<TARGET> const & list, TARGET target)
{
    return std::find(list.begin(), list.end(), target) != list.end();
}

static void Move(FootClass * unit, int cell, bool formation = false, MPHType speed = MPH_IMMOBILE)
{
    if (!Map.In_Radar((CELL)cell)) return;
    if (unit->AttackMove) unit->ResetAttackMove();
    unit->IsFormationMove = formation;
    if (formation) {
        unit->FormationSpeed = unit->Techno_Type_Class()->Speed;
        unit->FormationMaxSpeed = speed;
    }
    TARGET destination = ::As_Target((CELL)cell);
    if (unit->NavCom == destination && unit->Mission == MISSION_MOVE) return;
    unit->Clear_Navigation_List();
    unit->Assign_Target(TARGET_NONE);
    unit->Assign_Mission(MISSION_MOVE);
    unit->Assign_Destination(destination);
}

static void Guard(FootClass * unit)
{
    if (unit->AttackMove) unit->ResetAttackMove();
    unit->IsFormationMove = false;
    if (unit->Mission == MISSION_GUARD && !Target_Legal(unit->NavCom)) return;
    unit->Clear_Navigation_List();
    unit->Assign_Destination(TARGET_NONE);
    unit->Assign_Target(TARGET_NONE);
    unit->Assign_Mission(MISSION_GUARD);
}

static void Attack_Target(FootClass * unit, TARGET target)
{
    if (unit->AttackMove) unit->ResetAttackMove();
    unit->IsFormationMove = false;
    if (unit->TarCom == target && unit->Mission == MISSION_ATTACK) return;
    unit->Clear_Navigation_List();
    unit->Assign_Destination(TARGET_NONE);
    unit->Assign_Target(target);
    unit->Assign_Mission(MISSION_ATTACK);
}

static void Release_Formation(TARGET target, HouseClass * house)
{
    TechnoClass * object = As_Techno(target);
    if (Alive(object) && object->Is_Foot() && (HouseClass const *)object->House == house
        && !((FootClass *)object)->Team) ((FootClass *)object)->IsFormationMove = false;
}

static void Cancel(StrikePlan & plan, HouseClass * house, std::vector<TARGET> const * protected_units = NULL)
{
    bool active = plan.Phase != STRIKE_HOLD;
    if (active) AILog::Record(Frame, house->Class->House, "strike_cancel",
        AILog::Fields().Number("target", plan.Target).Number("members", plan.Members.size()).Number("phase", plan.Phase)
            .Boolean("external", plan.External).Boolean("forced", plan.Forced));
    for (unsigned int i = 0; i < plan.Members.size(); ++i) {
        if (protected_units && Contains(*protected_units, plan.Members[i])) continue;
        FootClass * unit = Member(plan.Members[i], house);
        if (unit) Guard(unit);
        else Release_Formation(plan.Members[i], house);
    }
    plan = StrikePlan();
    plan.NextAttempt = Frame + TICKS_PER_SECOND * 5;
    if (active) house->AI_Replan_Attack();
}

static int Nearby_Power(std::vector<Contact> const & contacts, RouteMap const & field, int cell, bool air = false)
{
    int power = 0;
    for (unsigned int i = 0; i < contacts.size(); ++i) {
        Contact const & contact = contacts[i];
        WeaponTypeClass const * weapon = Weapon_Against(contact.Object, air);
        if (!weapon) continue;
        int radius = contact.Mobile ? (contact.Air ? 16 : 10) : Clamp(weapon->Range / CELL_LEPTON_W, 1, 20) + 4;
        if (field.Separation(cell, contact.Cell) <= radius) {
            power += contact.Mobile ? contact.Power : contact.Power * 2;
        }
    }
    return power;
}

// Count mobile forces that can respond to this sector soon. Ground
// reinforcement distance follows passable terrain, including walls and bridges.
static int Enemy_Response(std::vector<Contact> const & contacts, RouteMap const & field,
    int cell, bool naval, bool air = false)
{
    RouteMap response(MAP_CELL_W, MAP_CELL_H);
    response.Clear = field.Clear;
    std::queue<int> queue;
    int source = field.Approach(cell, 3);
    if (source >= 0) { response.Distance[source] = 0; queue.push(source); }
    while (!queue.empty()) {
        int current = queue.front(); queue.pop();
        if (response.Distance[current] >= 18) continue;
        int x = current % MAP_CELL_W, y = current / MAP_CELL_W;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            int nx = x + dx, ny = y + dy;
            if ((!dx && !dy) || nx < 0 || nx >= MAP_CELL_W || ny < 0 || ny >= MAP_CELL_H) continue;
            int next = ny * MAP_CELL_W + nx;
            if (!response.Clear[next] || response.Distance[next] >= 0) continue;
            if (dx && dy && (!response.Clear[y * MAP_CELL_W + nx] || !response.Clear[ny * MAP_CELL_W + x])) continue;
            response.Distance[next] = response.Distance[current] + 1;
            queue.push(next);
        }
    }
    int power = 0;
    for (unsigned int i = 0; i < contacts.size(); ++i) {
        Contact const & contact = contacts[i];
        if (!contact.Mobile || !Weapon_Against(contact.Object, air) || (!air && contact.Naval != naval && !contact.Air)) continue;
        if (contact.Air) {
            if (field.Separation(cell, contact.Cell) <= 26) power += contact.Power;
        } else if (field.Separation(cell, contact.Cell) <= 18 && response.Distance[contact.Cell] >= 0) {
            power += contact.Power;
        }
    }
    return power;
}

static void Threat_Costs(RouteMap & field, std::vector<Contact> const & contacts, int strength, bool air)
{
    for (unsigned int i = 0; i < contacts.size(); ++i) {
        Contact const & contact = contacts[i];
        WeaponTypeClass const * weapon = Weapon_Against(contact.Object, air);
        if (contact.Power <= 0 || !weapon) continue;
        int radius = contact.Mobile ? (std::max)(contact.Air ? 16 : 10,
            Clamp(weapon->Range / CELL_LEPTON_W, 1, 20) + 2) : Clamp(weapon->Range / CELL_LEPTON_W, 1, 20) + 4;
        int x = contact.Cell % MAP_CELL_W, y = contact.Cell / MAP_CELL_W;
        int penalty = Clamp((int)((long long)contact.Power * (contact.Mobile ? 4 : 16)
            / Clamp(strength, 1, 100000000)), 1, 50);
        for (int cy = Clamp(y - radius, 0, MAP_CELL_H - 1); cy <= Clamp(y + radius, 0, MAP_CELL_H - 1); ++cy) {
            for (int cx = Clamp(x - radius, 0, MAP_CELL_W - 1); cx <= Clamp(x + radius, 0, MAP_CELL_W - 1); ++cx)
                field.Danger[cy * MAP_CELL_W + cx] += penalty;
        }
    }
}

static bool Safe_Strike_Route(RouteMap const & field, std::vector<Contact> const & contacts,
    int destination, int strength, bool air)
{
    std::vector<int> path = field.Path(destination);
    if (path.empty()) return false;
    for (unsigned int i = 0; i < path.size(); ++i) {
        if (i % 4 != 0 && i + 1 != path.size()) continue;
        if (AITactics::Should_Withdraw(strength, 0, Nearby_Power(contacts, field, path[i], air))) return false;
    }
    return true;
}

static int Target_Value(Contact const & contact)
{
    if (contact.Mobile) {
        int value = contact.Object->Techno_Type_Class()->Cost_Of();
        if (contact.Object->What_Am_I() == RTTI_UNIT) {
            UnitType type = ((UnitClass const *)contact.Object)->Class->Type;
            if (type == UNIT_MCV) value += 1500;
            else if (type == UNIT_HARVESTER) value += 750;
        }
        return value;
    }
    BuildingClass const * building = (BuildingClass const *)contact.Object;
    int value = building->Class->Cost_Of();
    switch (building->Class->Type) {
    case STRUCT_CONST: value += 1500; break;
    case STRUCT_REFINERY: case STRUCT_WEAP: value += 1000; break;
    case STRUCT_POWER: case STRUCT_ADVANCED_POWER: value += 750; break;
    default: break;
    }
    return value;
}

static bool Can_Hit(FootClass const * unit, Contact const & target)
{
    TechnoTypeClass const * type = unit->Techno_Type_Class();
    WeaponTypeClass const * weapons[] = {type->PrimaryWeapon, type->SecondaryWeapon};
    for (int i = 0; i < 2; ++i) {
        WeaponTypeClass const * weapon = weapons[i];
        if (!weapon || !weapon->Bullet || !weapon->WarheadPtr || weapon->Attack <= 0
            || weapon->WarheadPtr->Modifier[target.Object->Techno_Type_Class()->Armor] <= 0) continue;
        BulletTypeClass const * bullet = weapon->Bullet;
        if (target.Air) { if (bullet->IsAntiAircraft) return true; continue; }
        if (bullet->IsSubSurface && !target.Naval && target.Object->Techno_Type_Class()->Speed != SPEED_FLOAT) continue;
        if (bullet->IsAntiGround) return true;
    }
    return false;
}

static bool Tank_Type(Fighter const & fighter)
{
    if (fighter.Naval || fighter.Air || fighter.Object->What_Am_I() != RTTI_UNIT) return false;
    UnitType type = ((UnitClass const *)fighter.Object)->Class->Type;
    bool tank = type == UNIT_HTANK || type == UNIT_MTANK || type == UNIT_MTANK2 || type == UNIT_LTANK;
#ifdef FIXIT_CSII
    tank = tank || type == UNIT_CHRONOTANK || type == UNIT_TESLATANK;
#endif
    return tank && fighter.Object->Ammo != 0;
}

static bool Main_Tank(Fighter const & fighter)
{
    return Tank_Type(fighter)
        && (long long)fighter.Object->Strength * 2 >= fighter.Object->Techno_Type_Class()->MaxStrength;
}

static int Tank_Count(std::vector<Fighter> const & army)
{
    int count = 0;
    for (unsigned int i = 0; i < army.size(); ++i) if (Tank_Type(army[i])) ++count;
    return count;
}

static bool Power_Target(Contact const & contact)
{
    if (contact.Mobile) return false;
    StructType type = ((BuildingClass const *)contact.Object)->Class->Type;
    return type == STRUCT_POWER || type == STRUCT_ADVANCED_POWER;
}

static int Strike_Score(Contact const & contact, StrikeIntent intent, int defenders, int distance)
{
    int priority = 0;
    bool economic = Power_Target(contact) || (!contact.Mobile
        && ((BuildingClass const *)contact.Object)->Class->Type == STRUCT_REFINERY);
    if (intent == STRIKE_ECONOMY && economic) priority = 1;
    if (intent == STRIKE_BASE && !contact.Mobile) priority = economic ? 3 : 2;
    // Preferences apply only after reachability and ordinary safety checks.
    return priority * 500000 + AITactics::Weak_Point_Score(Target_Value(contact), defenders, distance);
}

static bool Strike_Ready(int count, int power, int response, int defense, bool air, bool forced)
{
    if (forced) return count > 0 && power > 0;
    if (!air) return AITactics::Can_Launch(count, power, response, defense);
    // A loaded aircraft can raid an exposed power plant without waiting for a
    // four-unit ground force. Anti-air superiority remains a local requirement.
    return count > 0 && power >= 500
        && (long long)power * 100 >= (long long)response * 115
        && (long long)power * 100 >= (long long)defense * 150;
}

static int Battle_Damage(WeaponTypeClass const * weapon, TechnoClass const * target)
{
    if (!weapon->WarheadPtr) return 0;
    return Clamp((int)(Clamp(weapon->Attack, 0, 10000)
        * weapon->WarheadPtr->Modifier[target->Techno_Type_Class()->Armor]), 0, 1000000);
}

// Respect native range, cloak, ammunition and weapon restrictions. Turning,
// stopping and the normal firing cooldown can be resolved by an attack order.
static WeaponTypeClass const * Battle_Weapon(TechnoClass const * unit, TechnoClass const * target, bool in_range)
{
    if (unit->Ammo == 0) return NULL;
    TechnoTypeClass const * type = unit->Techno_Type_Class();
    WeaponTypeClass const * weapons[] = {type->PrimaryWeapon, type->SecondaryWeapon};
    for (int i = 0; i < 2; ++i) {
        if (!weapons[i] || !weapons[i]->Bullet || Battle_Damage(weapons[i], target) <= 0) continue;
        if (in_range && !unit->In_Range(target->As_Target(), i, false)) continue;
        FireErrorType error = unit->Can_Fire(target->As_Target(), i);
        if (error == FIRE_OK || error == FIRE_REARM || error == FIRE_FACING || error == FIRE_ROTATING
            || error == FIRE_MOVING || error == FIRE_CLOAKED || (!in_range && error == FIRE_RANGE)) return weapons[i];
    }
    return NULL;
}

static bool Fight_Close_Contacts(std::vector<Fighter> const & squad, std::vector<Contact> const & contacts,
    RouteMap const * field, bool support, TARGET & battle_target)
{
    int seed = -1, distance = INT_MAX;
    std::vector<unsigned char> close(contacts.size(), 0);
    for (unsigned int i = 0; i < contacts.size(); ++i) {
        Contact const & contact = contacts[i];
        if (contact.Power <= 0) continue;
        for (unsigned int j = 0; j < squad.size(); ++j) {
            FootClass * unit = squad[j].Object;
            if (!Can_Hit(unit, contact)) continue;
            TechnoTypeClass const * type = unit->Techno_Type_Class();
            int own_range = 0;
            if (type->PrimaryWeapon) own_range = type->PrimaryWeapon->Range / CELL_LEPTON_W;
            if (type->SecondaryWeapon) own_range = (std::max)(own_range, type->SecondaryWeapon->Range / CELL_LEPTON_W);
            int separation = unit->Distance(contact.Object->Center_Coord());
            if (separation > ((std::max)(own_range, contact.Range) + 2) * CELL_LEPTON_W) continue;
            if (!Battle_Weapon(unit, contact.Object, false)) continue;
            bool direct = Battle_Weapon(unit, contact.Object, true) != NULL;
            bool incoming = Battle_Weapon(contact.Object, unit, true) != NULL;
            if (direct || (support && incoming)) {
                close[i] = 1;
                if (separation < distance) { distance = separation; seed = (int)i; }
                break;
            }
        }
    }
    if (seed < 0) return false;
    std::vector<int> pressure(contacts.size(), 0);
    std::map<int, int> approaches;
    int assigned = 0, largest = -1;
    for (unsigned int i = 0; i < squad.size(); ++i) {
        FootClass * unit = squad[i].Object;
        int chosen = -1; long long best = LLONG_MAX;
        WeaponTypeClass const * chosen_weapon = NULL;
        for (unsigned int j = 0; j < contacts.size(); ++j) {
            Contact const & contact = contacts[j];
            if (contact.Power <= 0 || !Can_Hit(unit, contact)) continue;
            int dx = abs(contact.Cell % MAP_CELL_W - contacts[seed].Cell % MAP_CELL_W);
            int dy = abs(contact.Cell / MAP_CELL_W - contacts[seed].Cell / MAP_CELL_W);
            if (!close[j] && (std::max)(dx, dy) > 12) continue;
            WeaponTypeClass const * weapon = Battle_Weapon(unit, contact.Object, false);
            if (!weapon) continue;
            bool direct = Battle_Weapon(unit, contact.Object, true) != NULL;
            if (!direct) {
                if (!support || !field || field->Distance[squad[i].Cell] < 0) continue;
                int radius = Clamp(weapon->Range / CELL_LEPTON_W, 1, 20);
                int key = (int)j * 32 + radius;
                std::map<int, int>::iterator approach = approaches.find(key);
                if (approach == approaches.end()) {
                    int cell = field->Approach(contact.Cell, radius);
                    approach = approaches.insert(std::make_pair(key, cell)).first;
                }
                if (approach->second < 0) continue;
            }
            // Prefer a target the unit can shoot now, then nearby targets.
            // A modest focus bonus concentrates fire without chasing past closer
            // threats; estimated assigned damage avoids excessive overkill.
            long long score = (direct ? 0 : 100000000LL) + (long long)unit->Distance(contact.Object->Center_Coord()) * 4;
            score += (long long)contact.Object->Strength * 400 / Clamp(contact.Object->Techno_Type_Class()->MaxStrength, 1, INT_MAX);
            if (!contact.Mobile) score += CELL_LEPTON_W * 2;
            if (pressure[j] > 0 && pressure[j] < contact.Object->Strength) score -= CELL_LEPTON_W * 4;
            if (pressure[j] >= contact.Object->Strength) score += CELL_LEPTON_W * 8;
            if (unit->TarCom == contact.Object->As_Target()) score -= CELL_LEPTON_W * 2;
            if (score < best) { best = score; chosen = (int)j; chosen_weapon = weapon; }
        }
        if (chosen < 0) { if (support) Guard(unit); continue; }
        Attack_Target(unit, contacts[chosen].Object->As_Target());
        pressure[chosen] += Battle_Damage(chosen_weapon, contacts[chosen].Object);
        ++assigned;
        if (pressure[chosen] > largest) { largest = pressure[chosen]; battle_target = contacts[chosen].Object->As_Target(); }
    }
    return assigned > 0;
}

static bool Encounter(StrikePlan & plan, HouseClass * house, std::vector<Contact> const & contacts,
    std::vector<TARGET> const & defenders)
{
    bool was_fighting = plan.Fighting;
    plan.Fighting = false; plan.BattleTarget = TARGET_NONE;
    if (plan.Phase == STRIKE_HOLD) return false;
    std::vector<Fighter> squad;
    std::vector<TARGET> remaining;
    for (unsigned int i = 0; i < plan.Members.size(); ++i) {
        FootClass * unit = Member(plan.Members[i], house);
        if (!unit) { Release_Formation(plan.Members[i], house); continue; }
        if (Contains(defenders, plan.Members[i])) continue;
        remaining.push_back(plan.Members[i]);
        if (unit->What_Am_I() == RTTI_AIRCRAFT) continue;
        Fighter fighter = {unit, Coord_Cell(unit->Center_Coord()), Power(unit),
            unit->What_Am_I() == RTTI_VESSEL, false};
        squad.push_back(fighter);
    }
    plan.Members.swap(remaining);
    if (!Fight_Close_Contacts(squad, contacts, &plan.Field, true, plan.BattleTarget)) {
        if (was_fighting) {
            plan.Resume = true;
            for (unsigned int i = 0; i < squad.size(); ++i) Guard(squad[i].Object);
        }
        return false;
    }
    plan.Fighting = true; plan.Resume = false;
    plan.ProgressFrame = Frame; // Fighting is real progress, not a stalled rally.
    return true;
}

static int Formation_Width(int count)
{
    int width = 5;
    while ((long long)width * width < count) width += 2;
    return width;
}

static int Rally_Cell(RouteMap const & field, int desired, int count)
{
    int width = Formation_Width(count);
    int radius = width / 2;
    int required = width * width;
    int tx = desired % MAP_CELL_W, ty = desired / MAP_CELL_W;
    int best = desired, best_score = INT_MAX;
    for (int y = Clamp(ty - 10, 0, MAP_CELL_H - 1); y <= Clamp(ty + 10, 0, MAP_CELL_H - 1); ++y) {
        for (int x = Clamp(tx - 10, 0, MAP_CELL_W - 1); x <= Clamp(tx + 10, 0, MAP_CELL_W - 1); ++x) {
            int cell = y * MAP_CELL_W + x;
            if (!field.Clear[cell] || field.Distance[cell] < 0) continue;
            int capacity = 0;
            for (int cy = Clamp(y - radius, 0, MAP_CELL_H - 1); cy <= Clamp(y + radius, 0, MAP_CELL_H - 1); ++cy) {
                for (int cx = Clamp(x - radius, 0, MAP_CELL_W - 1); cx <= Clamp(x + radius, 0, MAP_CELL_W - 1); ++cx) {
                    int slot = cy * MAP_CELL_W + cx;
                    if (field.Clear[slot] && field.Distance[slot] >= 0) ++capacity;
                }
            }
            int missing = required > capacity ? required - capacity : 0;
            int score = missing * 128 + field.Separation(cell, desired) * 8;
            if (score < best_score) { best = cell; best_score = score; }
        }
    }
    return best;
}

static bool Start_Strike(StrikePlan & plan, HouseClass * house, std::vector<Fighter> const & army,
    std::vector<Contact> const & contacts, std::vector<TARGET> const & defenders, bool naval, bool air = false, bool continuing = false, bool all_out = false, int rally = -1,
    StrikeIntent intent = STRIKE_NORMAL, bool forced = false, TARGET specified_target = TARGET_NONE, int commit_percent = 0)
{
    if (Frame < plan.NextAttempt) return false;
    FootClass * representative = NULL;
    int largest = -1, available_count = 0;
    for (unsigned int i = 0; i < army.size(); ++i) {
        Fighter const & fighter = army[i];
        if (fighter.Naval != naval || fighter.Air != air || Contains(defenders, fighter.Object->As_Target())) continue;
        ++available_count;
        // A ground vehicle leads the path search even if a Tanya costs more.
        int priority = fighter.Power + (fighter.Object->What_Am_I() == RTTI_UNIT ? 100000 : 0);
        if (priority > largest) { representative = fighter.Object; largest = priority; }
    }
    if (!representative || available_count < (air || forced ? 1 : 4)) return false;
    int origin = Coord_Cell(representative->Center_Coord());
    RouteMap field(MAP_CELL_W, MAP_CELL_H);
    Fill_Routes(field, representative, origin);
    int home = naval || continuing || !house->Center ? origin : field.Approach(Coord_Cell(house->Center), 8);
    if (rally >= 0) {
        home = field.Approach(rally, 3);
        if (home < 0) return false;
    }
    if (home < 0) home = origin;
    std::vector<Fighter> recruits;
    for (unsigned int i = 0; i < army.size(); ++i) {
        Fighter const & fighter = army[i];
        if (fighter.Naval == naval && fighter.Air == air && field.Distance[fighter.Cell] >= 0
            && !Contains(defenders, fighter.Object->As_Target())
            && (!air || forced || !((AircraftClass *)fighter.Object)->Class->IsFixedWing || field.Separation(fighter.Cell, origin) <= 20))
            recruits.push_back(fighter);
    }
    // Keep the units closest to the base as a reaction reserve.
    std::stable_sort(recruits.begin(), recruits.end(), [&field, home](Fighter const & a, Fighter const & b) {
        return field.Separation(a.Cell, home) < field.Separation(b.Cell, home);
    });
    int total = 0;
    for (unsigned int i = 0; i < recruits.size(); ++i) total += recruits[i].Power;
    int reserved = 0, first = 0;
    if (commit_percent > 0) {
        int keep = (int)((long long)total * (100 - commit_percent) / 100);
        while (first < (int)recruits.size() && reserved + recruits[first].Power <= keep) reserved += recruits[first++].Power;
    } else {
        while (!air && !continuing && !all_out && first + 4 < (int)recruits.size() && reserved < total / 5) reserved += recruits[first++].Power;
    }
    if (!Strike_Ready((int)recruits.size() - first, total - reserved, 0, 0, air, forced)) {
        plan.NextAttempt = Frame + TICKS_PER_SECOND * 5; return false;
    }
    home = Rally_Cell(field, home, (int)recruits.size() - first);
    Contact const * chosen = NULL;
    int approach = -1, best = 0;
    // Strong defenses along the march make a detour preferable to a frontal crossing.
    Threat_Costs(field, contacts, total - reserved, air);
    field.Search(home);
    for (unsigned int i = 0; i < contacts.size(); ++i) {
        Contact const & contact = contacts[i];
        if (specified_target != TARGET_NONE && contact.Object->As_Target() != specified_target) continue;
        if (contact.Air || (intent == STRIKE_POWER && !Power_Target(contact))
            || (contact.Mobile && !air && contact.Naval != naval)) continue;
        int strike = 0, count = 0;
        for (int j = first; j < (int)recruits.size(); ++j) {
            if (Can_Hit(recruits[j].Object, contact)) { strike += recruits[j].Power; ++count; }
        }
        int cell = field.Approach(contact.Cell, air ? 1 : (naval ? 6 : 3));
        if (cell < 0) {
            AILog::Record(Frame, house->Class->House, "strike_candidate", AILog::Fields().Number("target", contact.Object->As_Target()).Text("rejection", "unreachable"));
            continue;
        }
        int defense = Nearby_Power(contacts, field, contact.Cell, air);
        int response = Enemy_Response(contacts, field, contact.Cell, naval, air);
        bool ready = Strike_Ready(count, strike, response, defense, air, forced);
        bool safe = ready && (forced || Safe_Strike_Route(field, contacts, cell, strike, air));
        AILog::Record(Frame, house->Class->House, "strike_candidate", AILog::Fields().Number("target", contact.Object->As_Target())
            .Number("group", air ? 2 : naval ? 1 : 0).Number("count", count).Number("power", strike).Number("defense", defense)
            .Number("response", response).Number("distance", field.Distance[cell]).Boolean("ready", ready).Boolean("route_safe", safe));
        if (!ready || !safe) continue;
        int score = Strike_Score(contact, intent, (std::max)(defense, response), field.Distance[cell]);
        if (!chosen || score > best) { chosen = &contact; approach = cell; best = score; }
    }
    plan.NextAttempt = Frame + TICKS_PER_SECOND * 5;
    if (!chosen) {
        AILog::Record(Frame, house->Class->House, "strike_rejected", AILog::Fields().Text("reason", "no_safe_compatible_target")
            .Number("available", available_count).Number("power", total - reserved).Boolean("forced", forced).Number("specified_target", specified_target));
        return false;
    }
    plan.Phase = STRIKE_RALLY;
    plan.AllOut = all_out;
    plan.Forced = forced;
    plan.Intent = intent;
    plan.Target = chosen->Object->As_Target();
    plan.TargetScore = best;
    plan.RetargetFrame = Frame;
    plan.Enemy = chosen->Owner;
    plan.Home = home;
    plan.Waypoint = home;
    plan.Step = 0;
    plan.ProgressFrame = Frame;
    plan.Route = field.Path(approach);
    plan.Field = field;
    for (int i = first; i < (int)recruits.size(); ++i) {
        if (Can_Hit(recruits[i].Object, *chosen)) plan.Members.push_back(recruits[i].Object->As_Target());
    }
    AILog::Record(Frame, house->Class->House, "strike_started", AILog::Fields().Number("target", plan.Target)
        .Number("score", best).Number("home", home).Number("approach", approach).Number("members", plan.Members.size())
        .Number("route_length", plan.Route.size()).Boolean("naval", naval).Boolean("air", air)
        .Boolean("forced", forced).Boolean("all_out", all_out).Number("intent", intent).Number("commit_percent", commit_percent));
    if (air) {
        for (unsigned int i = 0; i < plan.Members.size(); ++i) {
            AircraftClass * aircraft = (AircraftClass *)Member(plan.Members[i], house);
            if (aircraft && aircraft->Class->IsFixedWing) {
                // Jets cannot hover at ground-style rally waypoints. Ready jets
                // launch a common sortie together from their nearby airfields.
                plan.Phase = STRIKE_ENGAGE; break;
            }
        }
    }
    return true;
}

static bool Retarget_Strike(StrikePlan & plan, HouseClass * house, std::vector<Contact> const & contacts,
    std::vector<TARGET> const & defenders, bool required, int current_score)
{
    if (plan.External) return false;
    std::vector<Fighter> survivors;
    for (unsigned int i = 0; i < plan.Members.size(); ++i) {
        FootClass * unit = Member(plan.Members[i], house);
        if (!unit || Contains(defenders, plan.Members[i])) continue;
        Fighter fighter = {unit, Coord_Cell(unit->Center_Coord()), Power(unit),
            unit->What_Am_I() == RTTI_VESSEL, unit->What_Am_I() == RTTI_AIRCRAFT};
        survivors.push_back(fighter);
    }
    StrikePlan next;
    if (survivors.empty() || !Start_Strike(next, house, survivors, contacts, defenders,
        survivors[0].Naval, survivors[0].Air, true, plan.AllOut, -1, plan.Intent, plan.Forced) || next.Target == plan.Target) return false;
    // Ignore small score changes so an advancing force does not oscillate.
    if (!required && next.TargetScore < current_score + (std::max)(512, abs(current_score % 500000) / 3)) return false;
    for (unsigned int i = 0; i < plan.Members.size(); ++i) {
        if (!Contains(next.Members, plan.Members[i]) && !Contains(defenders, plan.Members[i])) {
            FootClass * unit = Member(plan.Members[i], house);
            if (unit) Guard(unit); else Release_Formation(plan.Members[i], house);
        }
    }
    plan = next;
    return true;
}

static void Update_Strike(StrikePlan & plan, HouseClass * house, std::vector<Contact> const & contacts,
    std::vector<TARGET> const & defenders, bool scan)
{
    if (plan.Phase == STRIKE_HOLD) return;
    if (Encounter(plan, house, contacts, defenders)) return;
    TechnoClass * target = As_Techno(plan.Target);
    if (!Alive(target) || !Opponent(house, target->House) || target->House->Class->House != plan.Enemy) {
        if (!Retarget_Strike(plan, house, contacts, defenders, true, 0)) {
            Cancel(plan, house); return;
        }
        target = As_Techno(plan.Target);
    }
    std::vector<TARGET> remaining;
    int power = 0, ready_power = 0, ready_count = 0;
    FootClass * representative = NULL;
    MPHType speed = MPH_LIGHT_SPEED;
    int formation_width = Formation_Width((int)plan.Members.size());
    int radius = formation_width / 2 + 1;
    RouteMap & field = plan.Field;
    for (unsigned int i = 0; i < plan.Members.size(); ++i) {
        FootClass * unit = Member(plan.Members[i], house);
        if (!unit) { Release_Formation(plan.Members[i], house); continue; }
        if (Contains(defenders, plan.Members[i])) continue;
        remaining.push_back(plan.Members[i]);
        int value = Power(unit);
        power += value;
        int priority = value + (unit->What_Am_I() == RTTI_UNIT ? 100000 : 0);
        int old_priority = representative ? Power(representative) + (representative->What_Am_I() == RTTI_UNIT ? 100000 : 0) : -1;
        if (priority > old_priority) representative = unit;
        if (unit->Techno_Type_Class()->MaxSpeed < speed) speed = unit->Techno_Type_Class()->MaxSpeed;
        if (field.Separation(Coord_Cell(unit->Center_Coord()), plan.Waypoint) <= radius) {
            ++ready_count;
            ready_power += value;
        }
    }
    plan.Members.swap(remaining);
    if (representative && plan.Resume) {
        plan.Resume = false; scan = true;
        if (plan.Phase != STRIKE_ENGAGE) {
            // Regroup where the battle ended, without returning to a checkpoint
            // that may now be far behind the force.
            plan.Phase = STRIKE_RALLY;
            plan.Home = plan.Waypoint = Coord_Cell(representative->Center_Coord());
            plan.Step = 0; plan.ProgressFrame = Frame; plan.GatheredPower = 0;
            ready_count = ready_power = 0;
            for (unsigned int i = 0; i < plan.Members.size(); ++i) {
                FootClass * unit = Member(plan.Members[i], house);
                if (unit && field.Separation(Coord_Cell(unit->Center_Coord()), plan.Waypoint) <= radius) {
                    ++ready_count; ready_power += Power(unit);
                }
            }
        }
    }
    if (!representative || (!plan.Forced && AITactics::Should_Withdraw(power, 0,
        Nearby_Power(contacts, field, Coord_Cell(representative->Center_Coord()), representative->What_Am_I() == RTTI_AIRCRAFT)))) {
        Cancel(plan, house);
        return;
    }
    if (scan) {
        bool air = representative->What_Am_I() == RTTI_AIRCRAFT;
        bool naval = representative->What_Am_I() == RTTI_VESSEL;
        int target_cell = Coord_Cell(target->Center_Coord());
        Fill_Routes(field, representative, Coord_Cell(representative->Center_Coord()));
        Threat_Costs(field, contacts, power, air);
        field.Search(Coord_Cell(representative->Center_Coord()));
        int destination = field.Approach(target_cell, air ? 1 : (naval ? 6 : 3));
        int defense = Nearby_Power(contacts, field, target_cell, air);
        int response = Enemy_Response(contacts, field, target_cell, naval, air);
        bool unsafe = destination < 0 || (!plan.Forced
            && (!Safe_Strike_Route(field, contacts, destination, power, air)
            || (plan.Phase == STRIKE_ENGAGE
            ? AITactics::Should_Withdraw(power, response, defense)
            : !Strike_Ready((int)plan.Members.size(), power, response, defense, air, false))));
        int current_score = 0;
        for (unsigned int i = 0; i < contacts.size(); ++i) {
            if (contacts[i].Object == target) {
                current_score = Strike_Score(contacts[i], plan.Intent, (std::max)(defense, response),
                    destination < 0 ? MAP_CELL_TOTAL : field.Distance[destination]);
                break;
            }
        }
        if (unsafe || Frame - plan.RetargetFrame >= TICKS_PER_SECOND * AITactics::RetargetSeconds) {
            if (Retarget_Strike(plan, house, contacts, defenders, unsafe, current_score)) {
                Update_Strike(plan, house, contacts, defenders, false);
                return;
            }
            if (unsafe) { Cancel(plan, house); return; }
            plan.RetargetFrame = Frame;
        }
        // Rebuild the remaining route when structures/walls change during the march.
        if (plan.Phase == STRIKE_RALLY || plan.Phase == STRIKE_ADVANCE) {
            if (!field.Valid(plan.Waypoint) || !field.Clear[plan.Waypoint] || field.Distance[plan.Waypoint] < 0) {
                plan.Waypoint = Coord_Cell(representative->Center_Coord());
            }
            field.Search(plan.Waypoint);
            destination = field.Approach(target_cell, air ? 1 : (naval ? 6 : 3));
            plan.Route = field.Path(destination);
            plan.Step = 0;
            if (plan.Route.empty()) { Cancel(plan, house); return; }
        }
    }
    if (ready_power > plan.GatheredPower) {
        plan.GatheredPower = ready_power;
        plan.ProgressFrame = Frame;
    }
    if (plan.Phase != STRIKE_ENGAGE && AITactics::Cohesion_Ready((int)plan.Members.size(), ready_count, power, ready_power)) {
        if (plan.Step + 1 >= (int)plan.Route.size()) {
            plan.Phase = STRIKE_ENGAGE;
        } else {
            plan.Phase = STRIKE_ADVANCE;
            plan.Step = Clamp(plan.Step + 4, 0, (int)plan.Route.size() - 1);
            // A narrow bridge cannot hold the whole force. Cross to a wide
            // outlet and regroup there before committing to combat.
            for (; plan.Step + 1 < (int)plan.Route.size(); ++plan.Step) {
                int point = plan.Route[plan.Step], capacity = 0;
                int px = point % MAP_CELL_W, py = point / MAP_CELL_W;
                for (int cy = Clamp(py - radius, 0, MAP_CELL_H - 1); cy <= Clamp(py + radius, 0, MAP_CELL_H - 1); ++cy) {
                    for (int cx = Clamp(px - radius, 0, MAP_CELL_W - 1); cx <= Clamp(px + radius, 0, MAP_CELL_W - 1); ++cx)
                        if (field.Clear[cy * MAP_CELL_W + cx]) ++capacity;
                }
                if ((long long)capacity * 100 >= (long long)plan.Members.size() * 80) break;
            }
            plan.Waypoint = plan.Route[plan.Step];
        }
        plan.ProgressFrame = Frame;
        plan.GatheredPower = 0;
    }
    if (plan.Phase != STRIKE_ENGAGE && Frame - plan.ProgressFrame > TICKS_PER_SECOND * 40) {
        // A blocked or strung-out force never releases a small suicide wave.
        Cancel(plan, house);
        return;
    }
    // Separate nearby destinations keep a column from queuing on one occupied cell.
    for (unsigned int i = 0; i < plan.Members.size(); ++i) {
        FootClass * unit = Member(plan.Members[i], house);
        if (!unit) continue;
        if (plan.Phase == STRIKE_ENGAGE) {
            Attack_Target(unit, plan.Target);
        } else {
            int x = plan.Waypoint % MAP_CELL_W + (int)(i % formation_width) - formation_width / 2;
            int y = plan.Waypoint / MAP_CELL_W + (int)(i / formation_width) - formation_width / 2;
            int slot = Clamp(y, 0, MAP_CELL_H - 1) * MAP_CELL_W + Clamp(x, 0, MAP_CELL_W - 1);
            slot = field.Approach(slot, 2);
            if (slot >= 0) Move(unit, slot, true, speed);
        }
    }
}

struct ThreatRegion {
    Contact const * Enemy;
    int Power;
    int Score;
};

static void Defend(TacticalState & state, HouseClass * house, std::vector<Fighter> const & army,
    std::vector<Contact> const & contacts, std::vector<TARGET> & defenders)
{
    RouteMap field(MAP_CELL_W, MAP_CELL_H);
    std::vector<ThreatRegion> regions;
    state.ThreatPower = 0;
    state.DefenseReady = true;
    state.AirThreat = false;
    state.DefenseTargets.clear();
    int approach_cell = 0, approach_score = -1;
    for (int i = 0; i < Buildings.Count() + Units.Count(); ++i) {
        TechnoClass * asset = i < Buildings.Count() ? (TechnoClass *)Buildings.Ptr(i)
            : (TechnoClass *)Units.Ptr(i - Buildings.Count());
        if (!Alive(asset) || (HouseClass const *)asset->House != house) continue;
        if (i >= Buildings.Count() && ((UnitClass *)asset)->Class->Type != UNIT_HARVESTER
            && ((UnitClass *)asset)->Class->Type != UNIT_MCV) continue;
        int cell = Coord_Cell(asset->Center_Coord());
        int local = 0;
        Contact const * nearest = NULL;
        int distance = MAP_CELL_W + MAP_CELL_H;
        for (unsigned int j = 0; j < contacts.size(); ++j) {
            Contact const & contact = contacts[j];
            if (!contact.Mobile || contact.Power <= 0) continue;
            int separation = field.Separation(cell, contact.Cell);
            if (separation <= Clamp(contact.Range + 4, 10, 20)) {
                local += contact.Power;
                if (separation < distance) { nearest = &contact; distance = separation; }
            }
            int potential = contact.Power * 32 / (separation + 4);
            if (potential > approach_score) { approach_score = potential; approach_cell = contact.Cell; }
        }
        if (local == 0 || !nearest) continue;
        int score = local;
        if (state.AttackedFrame >= 0 && Frame - state.AttackedFrame <= TICKS_PER_SECOND * 20
            && field.Separation(cell, state.AttackedCell) < 4) score += 2000;
        ThreatRegion region = {nearest, local, score};
        bool merged = false;
        for (unsigned int j = 0; j < regions.size(); ++j) {
            if (field.Separation(regions[j].Enemy->Cell, nearest->Cell) <= 12) {
                if (score > regions[j].Score) regions[j] = region;
                merged = true; break;
            }
        }
        if (!merged) regions.push_back(region);
    }
    if (regions.empty()) {
        if (!approach_cell) {
            int distance = MAP_CELL_TOTAL;
            for (unsigned int i = 0; i < contacts.size(); ++i) {
                if (contacts[i].Mobile) continue;
                int d = field.Separation(Coord_Cell(house->Center), contacts[i].Cell);
                if (d < distance) { distance = d; approach_cell = contacts[i].Cell; }
            }
        }
        state.DefenseCell = approach_cell;
        return;
    }
    std::stable_sort(regions.begin(), regions.end(), [](ThreatRegion const & a, ThreatRegion const & b) {
        return a.Score > b.Score;
    });
    state.DefenseCell = regions[0].Enemy->Cell;
    state.AirThreat = regions[0].Enemy->Air;
    if (regions.size() > 3) state.DefenseReady = false;
    // Concurrent raids get separate nearby responders instead of pulling every
    // defender to one front. A unit is committed to at most one region.
    for (unsigned int region = 0; region < regions.size() && region < 3; ++region) {
        Contact const * threat = regions[region].Enemy;
        int enemy_power = regions[region].Power;
        state.ThreatPower += enemy_power;
        state.DefenseTargets.push_back(threat->Object->As_Target());
        std::vector<Fighter> candidates;
        for (unsigned int i = 0; i < army.size(); ++i) {
            Fighter const & fighter = army[i];
            if (fighter.Air || Contains(defenders, fighter.Object->As_Target())) continue;
            if (fighter.Naval && !threat->Naval) continue;
            if (Can_Hit(fighter.Object, *threat)) candidates.push_back(fighter);
        }
        if (candidates.empty()) { state.DefenseReady = false; continue; }
        std::stable_sort(candidates.begin(), candidates.end(), [&field, threat](Fighter const & a, Fighter const & b) {
            return field.Separation(a.Cell, threat->Cell) * Clamp((int)b.Object->Techno_Type_Class()->MaxSpeed, 1, 1000)
                < field.Separation(b.Cell, threat->Cell) * Clamp((int)a.Object->Techno_Type_Class()->MaxSpeed, 1, 1000);
        });
        int defense_cell = -1;
        std::vector<int> searched(MAP_CELL_TOTAL, -1);
        for (unsigned int i = 0; i < candidates.size(); ++i) {
            TechnoTypeClass const * type = candidates[i].Object->Techno_Type_Class();
            int movement = type->Speed * 4 + type->MZone;
            if (searched[candidates[i].Cell] == movement) continue;
            Fill_Routes(field, candidates[i].Object, candidates[i].Cell);
            // Cache actual connectivity: native zone IDs may ignore a new wall.
            for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
                if (field.Distance[cell] >= 0) searched[cell] = movement;
            }
            defense_cell = field.Approach(threat->Cell, threat->Air ? 4 : 2);
            if (defense_cell >= 0) break;
        }
        if (defense_cell < 0) { state.DefenseReady = false; continue; }
        field.Search(defense_cell);
        std::stable_sort(candidates.begin(), candidates.end(), [&field](Fighter const & a, Fighter const & b) {
            int ad = field.Distance[a.Cell] < 0 ? MAP_CELL_TOTAL : field.Distance[a.Cell];
            int bd = field.Distance[b.Cell] < 0 ? MAP_CELL_TOTAL : field.Distance[b.Cell];
            return (long long)ad * Clamp((int)b.Object->Techno_Type_Class()->MaxSpeed, 1, 1000)
                < (long long)bd * Clamp((int)a.Object->Techno_Type_Class()->MaxSpeed, 1, 1000);
        });
        int strength = 0, responders = 0;
        for (int i = 0; i < Buildings.Count(); ++i) {
            BuildingClass * tower = Buildings.Ptr(i);
            if (!Alive(tower) || (HouseClass const *)tower->House != house) continue;
            WeaponTypeClass const * weapon = Weapon_Against(tower, threat->Air);
            if (!weapon) continue;
            if (field.Separation(Coord_Cell(tower->Center_Coord()), threat->Cell) <= weapon->Range / CELL_LEPTON_W)
                strength += Power(tower) * 2;
        }
        for (unsigned int i = 0; i < candidates.size(); ++i) {
            Fighter const & fighter = candidates[i];
            if (field.Distance[fighter.Cell] < 0) continue;
            defenders.push_back(fighter.Object->As_Target());
            Attack_Target(fighter.Object, threat->Object->As_Target());
            strength += fighter.Power; ++responders;
            if (responders >= 2 && AITactics::Defense_Sufficient(strength, enemy_power)) break;
        }
        if (!AITactics::Defense_Sufficient(strength, enemy_power)) state.DefenseReady = false;
    }
}

static bool All_Out(TacticalState & state, HouseClass * house, std::vector<Fighter> const & army,
    std::vector<Contact> const & contacts, std::vector<TARGET> const & defenders)
{
    if (LLM_Group_Controlled(house, LLM::GROUND)) return false;
    if (Frame < state.NextAllOutRoll || Frame < state.NextAllOutStrike || state.ThreatPower > 0
        || state.Ground.Fighting || state.NumericalAssault) return false;
    state.NextAllOutRoll = Frame + TICKS_PER_SECOND * AITactics::AllOutRollSeconds;
    int tanks = 0, power = 0;
    for (unsigned int i = 0; i < army.size(); ++i) {
        if (!Contains(defenders, army[i].Object->As_Target()) && Main_Tank(army[i])) { ++tanks; power += army[i].Power; }
    }
    bool ready = AITactics::All_Out_Ready(tanks, power);
    int roll = ready ? Random_Pick(0, 99) : -1;
    AILog::Record(Frame, house->Class->House, "all_out_roll", AILog::Fields().Number("tanks", tanks)
        .Number("power", power).Number("roll", roll).Number("chance", AITactics::AllOutChance).Boolean("eligible", ready));
    if (!ready || roll >= AITactics::AllOutChance) return false;
    int rally = -1;
    for (unsigned int i = 0; i < state.Ground.Members.size(); ++i) {
        FootClass * unit = Member(state.Ground.Members[i], house);
        if (unit && !Contains(defenders, state.Ground.Members[i])) { rally = Coord_Cell(unit->Center_Coord()); break; }
    }
    StrikePlan next;
    if (!Start_Strike(next, house, army, contacts, defenders, false, false, false, true, rally,
        tanks > AITactics::EconomyRaidTanks ? STRIKE_ECONOMY : STRIKE_NORMAL)) return false;
    // The count must belong to the reachable recruited force, not two islands.
    tanks = power = 0;
    for (unsigned int i = 0; i < next.Members.size(); ++i) {
        FootClass * unit = Member(next.Members[i], house);
        if (!unit) continue;
        Fighter fighter = {unit, Coord_Cell(unit->Center_Coord()), Power(unit), false, false};
        if (Main_Tank(fighter)) { ++tanks; power += fighter.Power; }
    }
    if (!AITactics::All_Out_Ready(tanks, power)) return false;
    for (unsigned int i = 0; i < state.Ground.Members.size(); ++i)
        if (!Contains(next.Members, state.Ground.Members[i]) && !Contains(defenders, state.Ground.Members[i])) {
            FootClass * unit = Member(state.Ground.Members[i], house);
            if (unit) Guard(unit);
        }
    state.Ground = next;
    state.NextAllOutStrike = Frame + TICKS_PER_MINUTE * AITactics::AllOutCooldownMinutes;
    return true;
}

// Numerical superiority is an explicit command to commit ready combat forces.
// It bypasses random rolls, economy/attack timers and local power vetoes, but
// still requires compatible weapons and a real route. Close battles run first
// in Update_Strike, so removing a static garrison does not stop nearby defense.
static bool Numerical_Assault(TacticalState & state, HouseClass * house, std::vector<Fighter> const & army,
    std::vector<Contact> const & contacts, std::vector<TARGET> & defenders, bool advantage)
{
    if (!advantage) {
        state.NumericalAssault = false;
        state.Ground.Forced = state.Fleet.Forced = state.Air.Forced = false;
        return false;
    }
    if (!state.NumericalAssault) state.NextForcedAttempt = 0;
    state.NumericalAssault = true;
    if (!LLM_Group_Controlled(house, LLM::GROUND) && !LLM_Group_Controlled(house, LLM::NAVAL)
        && !LLM_Group_Controlled(house, LLM::AIR)) {
        defenders.clear(); state.Defenders.clear();
    }
    if (Frame < state.NextForcedAttempt) return false;
    state.NextForcedAttempt = Frame + TICKS_PER_SECOND * AITactics::ForcedRetrySeconds;
    bool started = false;
    for (int kind = 0; kind < 3; ++kind) {
        if (LLM_Group_Controlled(house, kind)) continue;
        StrikePlan & plan = kind == 0 ? state.Ground : kind == 1 ? state.Fleet : state.Air;
        bool naval = kind == 1, air = kind == 2;
        if (plan.Phase != STRIKE_HOLD && (plan.Forced || plan.Fighting)) {
            plan.Forced = plan.AllOut = true; plan.Intent = STRIKE_BASE;
            Contact const * target = NULL;
            for (unsigned int i = 0; i < contacts.size(); ++i)
                if (contacts[i].Object->As_Target() == plan.Target) { target = &contacts[i]; break; }
            if (!target) continue; // The next update retargets without losing the front.
            for (unsigned int i = 0; i < army.size(); ++i) {
                Fighter const & fighter = army[i];
                if (fighter.Naval != naval || fighter.Air != air || Contains(plan.Members, fighter.Object->As_Target())
                    || plan.Field.Distance[fighter.Cell] < 0 || !Can_Hit(fighter.Object, *target)) continue;
                plan.Members.push_back(fighter.Object->As_Target());
                started = true;
            }
            continue;
        }
        int rally = -1;
        for (unsigned int i = 0; i < plan.Members.size(); ++i) {
            FootClass * unit = Member(plan.Members[i], house);
            if (unit) { rally = Coord_Cell(unit->Center_Coord()); break; }
        }
        StrikePlan next;
        if (!Start_Strike(next, house, army, contacts, defenders, naval, air, false, true,
            rally, STRIKE_BASE, true)) continue;
        for (unsigned int i = 0; i < plan.Members.size(); ++i) {
            if (Contains(next.Members, plan.Members[i])) continue;
            FootClass * unit = Member(plan.Members[i], house);
            if (unit) Guard(unit); else Release_Formation(plan.Members[i], house);
        }
        plan = next; started = true;
    }
    return started;
}

static void Defender_Encounter(std::vector<Fighter> const & army, std::vector<Contact> const & contacts,
    std::vector<TARGET> const & defenders)
{
    std::vector<Fighter> squad;
    for (unsigned int i = 0; i < army.size(); ++i)
        if (!army[i].Air && Contains(defenders, army[i].Object->As_Target())) squad.push_back(army[i]);
    TARGET target = TARGET_NONE;
    Fight_Close_Contacts(squad, contacts, NULL, false, target);
}

static void Reserve_Encounter(TacticalState const & state, std::vector<Fighter> const & army,
    std::vector<Contact> const & contacts, std::vector<TARGET> & defenders)
{
    // Active base raids already have responders chosen by actual connectivity.
    if (state.ThreatPower > 0) return;
    for (unsigned int i = 0; i < army.size(); ++i) {
        TARGET member = army[i].Object->As_Target();
        if (LLM_Has_Member(army[i].Object->House, member)) continue;
        if (army[i].Air || Contains(defenders, member) || Contains(state.Ground.Members, member)
            || Contains(state.Fleet.Members, member) || Contains(state.Air.Members, member)) continue;
        std::vector<Fighter> single(1, army[i]); TARGET target = TARGET_NONE;
        if (Fight_Close_Contacts(single, contacts, NULL, false, target)) defenders.push_back(member);
    }
}

static void Hold_Reserve(HouseClass * house, std::vector<Fighter> const & army, TacticalState const & state,
    std::vector<TARGET> const & defenders)
{
    for (unsigned int i = 0; i < army.size(); ++i) {
        Fighter const & fighter = army[i];
        TARGET target = fighter.Object->As_Target();
        if (LLM_Has_Member(house, target)) continue;
        if (Contains(defenders, target) || Contains(state.Ground.Members, target) || Contains(state.Fleet.Members, target) || Contains(state.Air.Members, target)) continue;
        if (fighter.Air) {
            if (state.ThreatPower == 0 && state.Ground.Phase != STRIKE_ENGAGE && state.Fleet.Phase != STRIKE_ENGAGE) Guard(fighter.Object);
            continue;
        }
        // Keep naval reserves in water. Ground troops guard the economic base.
        if (fighter.Naval || !house->Center || fighter.Object->Distance(house->Center) <= CELL_LEPTON_W * 8) {
            if (fighter.Object->Mission == MISSION_MOVE && !fighter.Object->IsFormationMove) continue;
            Guard(fighter.Object);
            continue;
        }
        TechnoTypeClass const * type = fighter.Object->Techno_Type_Class();
        CELL home = Map.Nearby_Location(Coord_Cell(house->Center), type->Speed,
            Map[(CELL)fighter.Cell].Zones[type->MZone], type->MZone);
        if (home && Map.In_Radar(home)) Move(fighter.Object, home);
    }
}

static void Air_Support(HouseClass * house, TacticalState const & state, std::vector<Contact> const & contacts)
{
    TARGET strike = state.Ground.Fighting ? state.Ground.BattleTarget
        : state.Fleet.Fighting ? state.Fleet.BattleTarget
        : state.Ground.Phase == STRIKE_ENGAGE ? state.Ground.Target
        : state.Fleet.Phase == STRIKE_ENGAGE ? state.Fleet.Target : TARGET_NONE;
    for (int i = 0; i < Aircraft.Count(); ++i) {
        AircraftClass * aircraft = Aircraft.Ptr(i);
        if (LLM_Has_Member(house, aircraft->As_Target()) && state.ThreatPower == 0) continue;
        if (!Alive(aircraft) || (HouseClass const *)aircraft->House != house || aircraft->Team || !Armed(aircraft)
            || aircraft->Ammo == 0 || aircraft->Mission == MISSION_ENTER
            || (aircraft->Height == 0 && aircraft->Class->MaxAmmo > 0 && aircraft->Ammo < aircraft->Class->MaxAmmo)
            || Contains(state.Air.Members, aircraft->As_Target())) continue;
        TARGET target = strike;
        if (state.ThreatPower > 0) {
            target = TARGET_NONE;
            for (unsigned int j = 0; j < state.DefenseTargets.size(); ++j) {
                for (unsigned int k = 0; k < contacts.size(); ++k) {
                    if (contacts[k].Object->As_Target() == state.DefenseTargets[j] && Can_Hit(aircraft, contacts[k])) {
                        target = state.DefenseTargets[j]; break;
                    }
                }
                if (Target_Legal(target)) break;
            }
        }
        if (Target_Legal(target)) Attack_Target(aircraft, target);
        else if (aircraft->Mission == MISSION_HUNT || aircraft->Mission == MISSION_ATTACK) Guard(aircraft);
    }
}

#include "LLMTACTICS.INC"

} // namespace

void HouseClass::AI_Replan_Attack(void)
{
    if (Attack > TICKS_PER_SECOND * 15) Attack = TICKS_PER_SECOND * 15;
    Tactical_State(this).NextAttack = Frame + TICKS_PER_SECOND * 5;
}

void HouseClass::AI_Tactics_Init(void)
{
    for (int i = 0; i < HOUSE_COUNT; ++i) TacticalStates[i] = TacticalState();
    LLM_Reset();
    AILog::Prepare(LLMMatchLow, LLMMatchHigh, Frame, "scenario_reset_or_load");
}

void HouseClass::AI_Tactical_Attacked(BuildingClass const * building)
{
    if (IsHuman || Session.Type == GAME_NORMAL || !building) return;
    TacticalState & state = Tactical_State(this);
    state.AttackedCell = Coord_Cell(building->Center_Coord());
    state.AttackedFrame = Frame;
    state.NextUpdate = 0;
}

bool HouseClass::AI_Update_Tactics(void)
{
    if (IsHuman || Session.Type == GAME_NORMAL || !IsActive || IsDefeated) return false;
    TacticalState & state = Tactical_State(this);
    if (Frame < state.NextUpdate) {
        if (Frame >= state.NextContactCheck) {
            state.NextContactCheck = Frame + AITactics::ContactTicks;
            std::vector<Fighter> army; std::vector<Contact> contacts;
            int friendly_count = 0, enemy_count = 0;
            Gather(this, army, contacts, &friendly_count, &enemy_count);
            std::vector<TARGET> defenders = state.Defenders;
            LLM_Tick(this, state, army, contacts, defenders);
            if (Numerical_Assault(state, this, army, contacts, defenders,
                AITactics::Numerical_Advantage(friendly_count, enemy_count))) {
                Update_Strike(state.Ground, this, contacts, defenders, false);
                Update_Strike(state.Fleet, this, contacts, defenders, false);
                Update_Strike(state.Air, this, contacts, defenders, false);
            }
            Defender_Encounter(army, contacts, defenders);
            Encounter(state.Ground, this, contacts, defenders);
            Encounter(state.Fleet, this, contacts, defenders);
            Reserve_Encounter(state, army, contacts, defenders);
            Air_Support(this, state, contacts);
        }
        return state.Ground.Phase != STRIKE_HOLD || state.Fleet.Phase != STRIKE_HOLD || state.Air.Phase != STRIKE_HOLD;
    }
    state.NextContactCheck = Frame + AITactics::ContactTicks;
    state.NextUpdate = Frame + TICKS_PER_SECOND;
    bool scan = Frame >= state.NextScan;
    if (scan) { state.NextScan = Frame + TICKS_PER_SECOND * AITactics::OpportunitySeconds; AI_CalcDynamics(); }
    std::vector<Fighter> army;
    std::vector<Contact> contacts;
    std::vector<TARGET> defenders;
    int friendly_count = 0, enemy_count = 0;
    Gather(this, army, contacts, &friendly_count, &enemy_count);
    bool numerical = AITactics::Numerical_Advantage(friendly_count, enemy_count);
    AILog::Record(Frame, Class->House, "tactics_evaluation", AILog::Fields().Number("friendly_combat_count", friendly_count)
        .Number("enemy_combat_count", enemy_count).Number("available", army.size()).Number("contacts", contacts.size())
        .Boolean("numerical_advantage", numerical).Boolean("opportunity_scan", scan)
        .Number("ground_phase", state.Ground.Phase).Number("naval_phase", state.Fleet.Phase).Number("air_phase", state.Air.Phase));
    Defend(state, this, army, contacts, defenders);
    AILog::Record(Frame, Class->House, "defense_decision", AILog::Fields().Number("defense_cell", state.DefenseCell)
        .Number("threat_power", state.ThreatPower).Number("defender_count", defenders.size()).Boolean("air_threat", state.AirThreat)
        .Boolean("defense_ready", state.DefenseReady));
    state.Defenders = defenders;
    LLM_Tick(this, state, army, contacts, defenders);
    bool forced_started = Numerical_Assault(state, this, army, contacts, defenders, numerical);
    if (!state.Ground.Forced && !LLM_Group_Controlled(this, LLM::GROUND))
        state.Ground.Intent = Tank_Count(army) > AITactics::EconomyRaidTanks ? STRIKE_ECONOMY : STRIKE_NORMAL;
    Defender_Encounter(army, contacts, defenders);
    Update_Strike(state.Ground, this, contacts, defenders, scan);
    Update_Strike(state.Fleet, this, contacts, defenders, scan);
    Update_Strike(state.Air, this, contacts, defenders, scan);
    bool economy = AITactics::Economy_Ready(BQuantity[STRUCT_REFINERY], UQuantity[UNIT_HARVESTER], IsTiberiumShort);
    if (!economy && !numerical) {
        if (!state.Ground.Fighting) Cancel(state.Ground, this);
        if (!state.Fleet.Fighting) Cancel(state.Fleet, this);
        Cancel(state.Air, this);
    }
    bool started = !numerical && economy && Frame > TICKS_PER_MINUTE && All_Out(state, this, army, contacts, defenders);
    if (!numerical && !started && scan && economy && state.DefenseReady && Frame > TICKS_PER_MINUTE && Frame >= state.NextAttack) {
        if (state.Ground.Phase == STRIKE_HOLD && !LLM_Group_Controlled(this, LLM::GROUND))
            started |= Start_Strike(state.Ground, this, army, contacts, defenders, false, false, false, false, -1,
                Tank_Count(army) > AITactics::EconomyRaidTanks ? STRIKE_ECONOMY : STRIKE_NORMAL);
        if (state.Fleet.Phase == STRIKE_HOLD && !LLM_Group_Controlled(this, LLM::NAVAL)) started |= Start_Strike(state.Fleet, this, army, contacts, defenders, true);
    }
    bool air_started = false;
    if (!numerical && scan && economy && state.DefenseReady && Frame > TICKS_PER_MINUTE
        && Frame >= state.NextAirRaid && state.Air.Phase == STRIKE_HOLD && !LLM_Group_Controlled(this, LLM::AIR)) {
        state.NextAirRaid = Frame + TICKS_PER_SECOND * AITactics::AirRaidSeconds;
        StrikePlan raid;
        air_started = Start_Strike(raid, this, army, contacts, defenders, false, true, false, false, -1, STRIKE_POWER);
        if (!air_started) {
            raid.NextAttempt = 0;
            air_started = Start_Strike(raid, this, army, contacts, defenders, false, true);
        }
        if (air_started) state.Air = raid;
    }
    if (started) {
        Attack = TICKS_PER_SECOND * AITactics::Attack_Delay(Rule.AIAttackInterval, Rule.AISpeedRush == 1);
        state.NextAttack = Frame + Attack;
    }
    if (started || air_started || forced_started) {
        Update_Strike(state.Ground, this, contacts, defenders, false);
        Update_Strike(state.Fleet, this, contacts, defenders, false);
        Update_Strike(state.Air, this, contacts, defenders, false);
    }
    Reserve_Encounter(state, army, contacts, defenders);
    Hold_Reserve(this, army, state, defenders);
    Air_Support(this, state, contacts);
    return state.Ground.Phase != STRIKE_HOLD || state.Fleet.Phase != STRIKE_HOLD || state.Air.Phase != STRIKE_HOLD;
}

COORDINATE HouseClass::AI_Defense_Location(BuildingClass const * building) const
{
    if (IsHuman || Session.Type == GAME_NORMAL || !Center || ::Power(building) == 0) return 0;
    TacticalState const & state = TacticalStates[Class->House];
    if (state.Owner != this || !state.DefenseCell) return 0;
    COORDINATE approach = Cell_Coord((CELL)state.DefenseCell);
    int distance = ::Distance(Center, approach);
    if (distance == 0) return 0;
    int forward = Clamp(Radius + CELL_LEPTON_W * 3, CELL_LEPTON_W * 4, CELL_LEPTON_W * 14);
    if (forward > distance) forward = distance;
    COORDINATE preferred = Coord_Move(Center, Direction(Center, approach), forward);
    TechnoTypeClass const * type = building->Techno_Type_Class();
    short const * occupy = building->Occupy_List(true);
    int best = -1, score = 0;
    for (int cell = 0; cell < MAP_CELL_TOTAL; ++cell) {
        if (!Map.In_Radar((CELL)cell) || Which_Zone((CELL)cell) == ZONE_NONE
            || !type->Legal_Placement((CELL)cell)
            || !Map.Passes_Proximity_Check(type, Class->House, occupy, (CELL)cell)) continue;
        int value = ::Distance(Cell_Coord((CELL)cell), preferred);
        // Spread towers along the approach; protect refinery exits from congestion.
        for (int i = 0; i < Buildings.Count(); ++i) {
            BuildingClass * existing = Buildings.Ptr(i);
            if (!Alive(existing) || existing->House != this) continue;
            int d = ::Distance(Cell_Coord((CELL)cell), existing->Center_Coord());
            if (::Power(existing) > 0 && d < CELL_LEPTON_W * 3) value += CELL_LEPTON_W * 6;
            if (existing->Class->Type == STRUCT_REFINERY && d < CELL_LEPTON_W * 3) value += CELL_LEPTON_W * 8;
        }
        if (best < 0 || value < score) { best = cell; score = value; }
    }
    return best >= 0 ? Cell_Coord((CELL)best) : 0;
}

StructType HouseClass::AI_Threat_Defense(void) const
{
    if (IsHuman || Session.Type == GAME_NORMAL || BQuantity[STRUCT_REFINERY] == 0 || Power < Drain) return STRUCT_NONE;
    TacticalState const & state = TacticalStates[Class->House];
    if (state.Owner != this || state.ThreatPower <= 0) return STRUCT_NONE;
    StructType choices[] = {STRUCT_TESLA, STRUCT_TURRET, STRUCT_FLAME_TURRET, STRUCT_PILLBOX};
    int limits[] = {AITeslaGunLimit, AITurretGunLimit, AIFlameTurretLimit, AIPillBoxLimit};
    int choice_count = 4;
    if (state.AirThreat) {
        choice_count = 2;
        choices[0] = STRUCT_SAM; limits[0] = AIAirdefenseLimit;
        choices[1] = STRUCT_AAGUN; limits[1] = AIAirdefenseLimit;
    }
    int desired = Clamp(2 + state.ThreatPower / 1500, 2, 8);
    for (int i = 0; i < choice_count; ++i) {
        BuildingTypeClass const & type = BuildingTypeClass::As_Reference(choices[i]);
        if (Can_Build(&type, ActLike) && (int)BQuantity[choices[i]] < Clamp(desired, 0, limits[i])) return choices[i];
    }
    return STRUCT_NONE;
}
