// Copyright 2026. Distributed under the project's GPLv3 license.
#include "FUNCTION.H"
#include "AITACTICS.H"
#include "AISTRATEGY.H"
#include <algorithm>
#include <climits>

namespace {
using AITactics::Clamp;
using AITactics::RouteMap;

enum StrikePhase { STRIKE_HOLD, STRIKE_RALLY, STRIKE_ADVANCE, STRIKE_ENGAGE };
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
    std::vector<TARGET> Members;
    std::vector<int> Route;
    RouteMap Field;
    StrikePlan() : Phase(STRIKE_HOLD), Target(TARGET_NONE), Enemy(-1), Home(0),
        Waypoint(0), Step(0), ProgressFrame(0), NextAttempt(0), GatheredPower(0), Field(MAP_CELL_W, MAP_CELL_H) {}
};
struct TacticalState {
    HouseClass * Owner;
    int LastFrame;
    int NextUpdate;
    int NextScan;
    int AttackedCell;
    int AttackedFrame;
    int DefenseCell;
    int ThreatPower;
    bool AirThreat;
    std::vector<TARGET> DefenseTargets;
    StrikePlan Ground;
    StrikePlan Fleet;
    StrikePlan Air;
    TacticalState() : Owner(NULL), LastFrame(-1), NextUpdate(0), NextScan(0),
        AttackedCell(0), AttackedFrame(-1), DefenseCell(0), ThreatPower(0), AirThreat(false) {}
};
TacticalState TacticalStates[HOUSE_COUNT];

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

static void Gather(HouseClass * house, std::vector<Fighter> & friendly, std::vector<Contact> & enemy)
{
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

static void Cancel(StrikePlan & plan, HouseClass * house)
{
    bool active = plan.Phase != STRIKE_HOLD;
    for (unsigned int i = 0; i < plan.Members.size(); ++i) {
        FootClass * unit = Member(plan.Members[i], house);
        if (unit) Guard(unit);
        else Release_Formation(plan.Members[i], house);
    }
    plan = StrikePlan();
    plan.NextAttempt = Frame + TICKS_PER_SECOND * 15;
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

static int Enemy_Mobile(std::vector<Contact> const & contacts, int owner, bool naval, bool air = false)
{
    int power = 0;
    for (unsigned int i = 0; i < contacts.size(); ++i) {
        Contact const & contact = contacts[i];
        if (contact.Owner == owner && contact.Mobile && (air || contact.Naval == naval || contact.Air))
            power += contact.Air ? contact.Power / 2 : contact.Power;
    }
    return power;
}

static void Threat_Costs(RouteMap & field, std::vector<Contact> const & contacts, int strength, bool air)
{
    for (unsigned int i = 0; i < contacts.size(); ++i) {
        Contact const & contact = contacts[i];
        WeaponTypeClass const * weapon = Weapon_Against(contact.Object, air);
        if (contact.Power <= 0 || !weapon) continue;
        int radius = contact.Mobile ? 3 : Clamp(weapon->Range / CELL_LEPTON_W, 1, 20) + 1;
        int x = contact.Cell % MAP_CELL_W, y = contact.Cell / MAP_CELL_W;
        int penalty = Clamp((int)((long long)contact.Power * (contact.Mobile ? 4 : 16)
            / Clamp(strength, 1, 100000000)), 1, 50);
        for (int cy = Clamp(y - radius, 0, MAP_CELL_H - 1); cy <= Clamp(y + radius, 0, MAP_CELL_H - 1); ++cy) {
            for (int cx = Clamp(x - radius, 0, MAP_CELL_W - 1); cx <= Clamp(x + radius, 0, MAP_CELL_W - 1); ++cx)
                field.Danger[cy * MAP_CELL_W + cx] += penalty;
        }
    }
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
        if (!weapon || !weapon->Bullet || weapon->Attack <= 0) continue;
        BulletTypeClass const * bullet = weapon->Bullet;
        if (target.Air) { if (bullet->IsAntiAircraft) return true; continue; }
        if (bullet->IsSubSurface && !target.Naval && target.Object->Techno_Type_Class()->Speed != SPEED_FLOAT) continue;
        if (bullet->IsAntiGround) return true;
    }
    return false;
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
    std::vector<Contact> const & contacts, std::vector<TARGET> const & defenders, bool naval, bool air = false, bool continuing = false)
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
    if (!representative || available_count < 5) return false;
    int origin = Coord_Cell(representative->Center_Coord());
    RouteMap field(MAP_CELL_W, MAP_CELL_H);
    Fill_Routes(field, representative, origin);
    int home = naval || continuing || !house->Center ? origin : field.Approach(Coord_Cell(house->Center), 8);
    if (home < 0) home = origin;
    std::vector<Fighter> recruits;
    for (unsigned int i = 0; i < army.size(); ++i) {
        Fighter const & fighter = army[i];
        if (fighter.Naval == naval && fighter.Air == air && field.Distance[fighter.Cell] >= 0
            && !Contains(defenders, fighter.Object->As_Target())
            && (!air || !((AircraftClass *)fighter.Object)->Class->IsFixedWing || field.Separation(fighter.Cell, origin) <= 20))
            recruits.push_back(fighter);
    }
    // Keep the units closest to the base as a reaction reserve.
    std::stable_sort(recruits.begin(), recruits.end(), [&field, home](Fighter const & a, Fighter const & b) {
        return field.Separation(a.Cell, home) < field.Separation(b.Cell, home);
    });
    int total = 0;
    for (unsigned int i = 0; i < recruits.size(); ++i) total += recruits[i].Power;
    int reserved = 0, first = 0;
    while (!continuing && first < (int)recruits.size() && reserved < total / 5) reserved += recruits[first++].Power;
    if (!AITactics::Can_Launch((int)recruits.size() - first, total - reserved, 0, 0)) {
        plan.NextAttempt = Frame + TICKS_PER_SECOND * 5; return false;
    }
    home = Rally_Cell(field, home, (int)recruits.size() - first);
    Contact const * chosen = NULL;
    int approach = -1, best = 0;
    // Strong defenses along the march make a detour preferable to a frontal crossing.
    Threat_Costs(field, contacts, total - reserved, air);
    field.Search(home);
    bool have_buildings[HOUSE_COUNT] = {};
    for (unsigned int i = 0; i < contacts.size(); ++i) if (!contacts[i].Mobile) have_buildings[contacts[i].Owner] = true;
    for (unsigned int i = 0; i < contacts.size(); ++i) {
        Contact const & contact = contacts[i];
        if (contact.Air || (contact.Mobile && (!naval && have_buildings[contact.Owner]))
            || (contact.Mobile && !air && contact.Naval != naval)) continue;
        int strike = 0, count = 0;
        for (int j = first; j < (int)recruits.size(); ++j) {
            if (Can_Hit(recruits[j].Object, contact)) { strike += recruits[j].Power; ++count; }
        }
        int cell = field.Approach(contact.Cell, air ? 1 : (naval ? 6 : 3));
        if (cell < 0) continue;
        int defense = Nearby_Power(contacts, field, contact.Cell, air);
        if (!AITactics::Can_Launch(count, strike, Enemy_Mobile(contacts, contact.Owner, naval, air), defense)) continue;
        int score = AITactics::Weak_Point_Score(Target_Value(contact), defense, field.Distance[cell]);
        if (!chosen || score > best) { chosen = &contact; approach = cell; best = score; }
    }
    plan.NextAttempt = Frame + TICKS_PER_SECOND * 5;
    if (!chosen) return false;
    plan.Phase = STRIKE_RALLY;
    plan.Target = chosen->Object->As_Target();
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

static void Update_Strike(StrikePlan & plan, HouseClass * house, std::vector<Contact> const & contacts,
    std::vector<TARGET> const & defenders, bool scan)
{
    if (plan.Phase == STRIKE_HOLD) return;
    TechnoClass * target = As_Techno(plan.Target);
    if (!Alive(target) || !Opponent(house, target->House) || target->House->Class->House != plan.Enemy) {
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
            survivors[0].Naval, survivors[0].Air, true)) {
            Cancel(plan, house); return;
        }
        // Continue the successful wave from its current position, with another
        // weak target, rather than marching home after destroying one building.
        for (unsigned int i = 0; i < plan.Members.size(); ++i) {
            if (!Contains(next.Members, plan.Members[i]) && !Contains(defenders, plan.Members[i])) {
                FootClass * unit = Member(plan.Members[i], house);
                if (unit) Guard(unit); else Release_Formation(plan.Members[i], house);
            }
        }
        plan = next;
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
    if (!representative || AITactics::Should_Withdraw(power, Enemy_Mobile(contacts, plan.Enemy,
        representative->What_Am_I() == RTTI_VESSEL, representative->What_Am_I() == RTTI_AIRCRAFT),
        Nearby_Power(contacts, field, plan.Waypoint, representative->What_Am_I() == RTTI_AIRCRAFT))) {
        Cancel(plan, house);
        return;
    }
    if (scan) {
        bool air = representative->What_Am_I() == RTTI_AIRCRAFT;
        if (plan.Phase != STRIKE_ENGAGE && (long long)power * 100
            < (long long)Nearby_Power(contacts, field, Coord_Cell(target->Center_Coord()), air) * 150) {
            Cancel(plan, house); return;
        }
        Fill_Routes(field, representative, Coord_Cell(representative->Center_Coord()));
        Threat_Costs(field, contacts, power, air);
        field.Search(Coord_Cell(representative->Center_Coord()));
        if (field.Approach(Coord_Cell(target->Center_Coord()), representative->What_Am_I() == RTTI_VESSEL ? 6 : 3) < 0) {
            Cancel(plan, house);
            return;
        }
        // Rebuild the remaining route when structures/walls change during the march.
        int destination = field.Approach(Coord_Cell(target->Center_Coord()), representative->What_Am_I() == RTTI_VESSEL ? 6 : 3);
        if (plan.Phase == STRIKE_ADVANCE) {
            if (!field.Valid(plan.Waypoint) || !field.Clear[plan.Waypoint] || field.Distance[plan.Waypoint] < 0) {
                plan.Waypoint = Coord_Cell(representative->Center_Coord());
            }
            field.Search(plan.Waypoint);
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
        if (candidates.empty()) continue;
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
        if (defense_cell < 0) continue;
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
    }
}

static void Hold_Reserve(HouseClass * house, std::vector<Fighter> const & army, TacticalState const & state,
    std::vector<TARGET> const & defenders)
{
    for (unsigned int i = 0; i < army.size(); ++i) {
        Fighter const & fighter = army[i];
        TARGET target = fighter.Object->As_Target();
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
    TARGET strike = state.Ground.Phase == STRIKE_ENGAGE ? state.Ground.Target
        : state.Fleet.Phase == STRIKE_ENGAGE ? state.Fleet.Target : TARGET_NONE;
    for (int i = 0; i < Aircraft.Count(); ++i) {
        AircraftClass * aircraft = Aircraft.Ptr(i);
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

} // namespace

void HouseClass::AI_Replan_Attack(void)
{
    if (Attack > TICKS_PER_SECOND * 15) Attack = TICKS_PER_SECOND * 15;
}

void HouseClass::AI_Tactics_Init(void)
{
    for (int i = 0; i < HOUSE_COUNT; ++i) TacticalStates[i] = TacticalState();
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
    if (Frame < state.NextUpdate) return state.Ground.Phase != STRIKE_HOLD || state.Fleet.Phase != STRIKE_HOLD || state.Air.Phase != STRIKE_HOLD;
    state.NextUpdate = Frame + TICKS_PER_SECOND;
    bool scan = Frame >= state.NextScan;
    if (scan) { state.NextScan = Frame + TICKS_PER_SECOND * 5; AI_CalcDynamics(); }
    std::vector<Fighter> army;
    std::vector<Contact> contacts;
    std::vector<TARGET> defenders;
    Gather(this, army, contacts);
    Defend(state, this, army, contacts, defenders);
    Update_Strike(state.Ground, this, contacts, defenders, scan);
    Update_Strike(state.Fleet, this, contacts, defenders, scan);
    Update_Strike(state.Air, this, contacts, defenders, scan);
    bool economy = AITactics::Economy_Ready(BQuantity[STRUCT_REFINERY], UQuantity[UNIT_HARVESTER], IsTiberiumShort);
    if (!economy) { Cancel(state.Ground, this); Cancel(state.Fleet, this); Cancel(state.Air, this); }
    if (scan && economy && state.ThreatPower == 0 && Frame > TICKS_PER_MINUTE && Attack == 0) {
        bool started = false;
        if (state.Air.Phase == STRIKE_HOLD && state.Ground.Phase == STRIKE_HOLD) started |= Start_Strike(state.Ground, this, army, contacts, defenders, false);
        if (state.Air.Phase == STRIKE_HOLD && state.Fleet.Phase == STRIKE_HOLD) started |= Start_Strike(state.Fleet, this, army, contacts, defenders, true);
        if (state.Ground.Phase == STRIKE_HOLD && state.Fleet.Phase == STRIKE_HOLD && state.Air.Phase == STRIKE_HOLD)
            started |= Start_Strike(state.Air, this, army, contacts, defenders, false, true);
        if (started) {
            Attack = Rule.AIAttackInterval <= 0 || Rule.AISpeedRush == 1 ? TICKS_PER_SECOND * 2
                : TICKS_PER_MINUTE * Clamp(Rule.AIAttackInterval, 1, 120);
            Update_Strike(state.Ground, this, contacts, defenders, false);
            Update_Strike(state.Fleet, this, contacts, defenders, false);
            Update_Strike(state.Air, this, contacts, defenders, false);
        }
    }
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
