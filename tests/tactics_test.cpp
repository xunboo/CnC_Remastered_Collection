// Compile the actual AITACTICS.CPP controller against a controlled world.
#define FIXIT_CSII
#include "../REDALERT/AITACTICS.H"
#include "../REDALERT/AISTRATEGY.H"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

typedef int CELL;
typedef unsigned int COORDINATE;
typedef unsigned int TARGET;
typedef int HousesType;
typedef int UnitType;
typedef int StructType;
typedef int MPHType;
typedef int OverlayType;
typedef int DirType;
typedef int FireErrorType;
const int FIRE_OK=0, FIRE_AMMO=1, FIRE_FACING=2, FIRE_REARM=3, FIRE_ROTATING=4,
    FIRE_ILLEGAL=5, FIRE_CANT=6, FIRE_MOVING=7, FIRE_RANGE=8, FIRE_CLOAKED=9;
const int HOUSE_COUNT=20, HOUSE_NEUTRAL=18, HOUSE_JP=19, GAME_NORMAL=0;
const int MAP_CELL_W=128, MAP_CELL_H=128, MAP_CELL_TOTAL=16384;
const int TICKS_PER_SECOND=15, TICKS_PER_MINUTE=900, CELL_LEPTON_W=256;
const TARGET TARGET_NONE=0;
const int RTTI_UNIT=1, RTTI_INFANTRY=2, RTTI_VESSEL=3, RTTI_AIRCRAFT=4, RTTI_BUILDING=5;
const int UNIT_HTANK=0, UNIT_HARVESTER=1, UNIT_MCV=2, UNIT_MINELAYER=3, UNIT_MAD=4, UNIT_DEMOTRUCK=5;
const int UNIT_MTANK=6, UNIT_MTANK2=7, UNIT_LTANK=8, UNIT_CHRONOTANK=9, UNIT_TESLATANK=10;
const int STRUCT_CONST=0, STRUCT_REFINERY=1, STRUCT_WEAP=2, STRUCT_POWER=3, STRUCT_ADVANCED_POWER=4;
const int STRUCT_TESLA=5, STRUCT_TURRET=6, STRUCT_FLAME_TURRET=7, STRUCT_PILLBOX=8, STRUCT_SAM=9, STRUCT_AAGUN=10, STRUCT_NONE=-1;
const int MISSION_GUARD=0, MISSION_MOVE=1, MISSION_ATTACK=2, MISSION_HUNT=3, MISSION_ENTER=4;
const int MISSION_RETREAT=5, MISSION_CAPTURE=6, MISSION_UNLOAD=7;
const int MPH_IMMOBILE=0, MPH_LIGHT_SPEED=1000, OVERLAY_NONE=-1, SPEED_FLOAT=1, SPEED_WINGED=2;
int Frame=0;
int RandomResult=99, RandomCalls=0;
int Random_Pick(int low,int high) { ++RandomCalls; return (std::max)(low,(std::min)(high,RandomResult)); }
struct { int Type=1; } Session;
struct { int AIAttackInterval=5, AISpeedRush=0; } Rule;

COORDINATE Cell_Coord(CELL cell) { return ((unsigned int)(cell / 128 * 256 + 128) << 16) | (unsigned int)(cell % 128 * 256 + 128); }
CELL Coord_Cell(COORDINATE coord) { return (int)((coord & 65535) / 256 + (coord >> 16) / 256 * 128); }
int Cell(int x,int y) { return y*128+x; }
int Distance(COORDINATE a,COORDINATE b)
{
    return (std::max)(std::abs((int)(a & 65535)-(int)(b & 65535)), std::abs((int)(a >> 16)-(int)(b >> 16)));
}
DirType Direction(COORDINATE a,COORDINATE b)
{
    int dx=(int)(b & 65535)-(int)(a & 65535), dy=(int)(b >> 16)-(int)(a >> 16);
    if (std::abs(dx) >= std::abs(dy)) return dx >= 0 ? 1 : 3;
    return dy >= 0 ? 2 : 0;
}
COORDINATE Coord_Move(COORDINATE a,DirType d,int distance)
{
    int x=(int)(a & 65535),y=(int)(a >> 16);
    if (d==0) y-=distance; if (d==1) x+=distance; if (d==2) y+=distance; if (d==3) x-=distance;
    return ((unsigned int)y<<16)|(unsigned int)x;
}
TARGET As_Target(CELL cell) { return 0x80000000u|(unsigned int)cell; }
bool Target_Legal(TARGET target) { return target!=TARGET_NONE; }

struct HouseClass;
struct BuildingClass;
struct BulletTypeClass { bool IsAntiGround=true, IsAntiAircraft=false, IsSubSurface=false; } GroundBullet, AirBullet;
struct WarheadTypeClass { double Modifier[5]={1,1,1,1,1}; } StandardWarhead;
struct WeaponTypeClass { BulletTypeClass * Bullet=&GroundBullet; WarheadTypeClass * WarheadPtr=&StandardWarhead; int Attack=40, Range=4*256; } Cannon, AntiAir;
struct TechnoTypeClass {
    WeaponTypeClass const * PrimaryWeapon=&Cannon;
    WeaponTypeClass const * SecondaryWeapon=nullptr;
    int Armor=0, Cost=1000, MaxStrength=500, MaxSpeed=10, Speed=0, MZone=0, Type=0, MaxAmmo=5;
    bool Allowed=true, IsFixedWing=false;
    int Cost_Of() const { return Cost; }
    bool Legal_Placement(CELL cell) const;
};
struct BuildingTypeClass : TechnoTypeClass {
    static BuildingTypeClass Types[11];
    static BuildingTypeClass const & As_Reference(StructType type) { return Types[type]; }
};
BuildingTypeClass BuildingTypeClass::Types[11];
std::map<TARGET,struct TechnoClass *> Targets;
struct TechnoClass {
    HouseClass * House=nullptr;
    TechnoTypeClass const * Class=nullptr;
    bool IsActive=true, IsInLimbo=false, AttackMove=false, IsTethered=false;
    int Strength=500, Kind=RTTI_UNIT, Mission=MISSION_GUARD, FireStatus=FIRE_OK, Ammo=5;
    bool Hidden=false;
    COORDINATE Coord=0;
    TARGET Handle=0, TarCom=0, NavCom=0;
    virtual ~TechnoClass() {}
    TechnoTypeClass const * Techno_Type_Class() const { return Class; }
    COORDINATE Center_Coord() const { return Coord; }
    bool Is_Foot() const { return Kind != RTTI_BUILDING; }
    int What_Am_I() const { return Kind; }
    TARGET As_Target() const { return Handle; }
    bool In_Range(TARGET,int=0,bool=true) const;
    FireErrorType Can_Fire(TARGET,int=0) const;
};
TechnoClass * As_Techno(TARGET target) { auto it=Targets.find(target); return it==Targets.end()?nullptr:it->second; }
bool TechnoClass::In_Range(TARGET target,int which,bool) const {
    auto enemy=As_Techno(target); if(!enemy) return false;
    auto weapon=which==0?Class->PrimaryWeapon:Class->SecondaryWeapon;
    return weapon && ::Distance(Coord,enemy->Coord)<=weapon->Range;
}
FireErrorType TechnoClass::Can_Fire(TARGET target,int which) const {
    auto enemy=As_Techno(target); if(!enemy) return FIRE_ILLEGAL;
    auto weapon=which==0?Class->PrimaryWeapon:Class->SecondaryWeapon;
    if(enemy->Hidden || !weapon || (enemy->Kind==RTTI_AIRCRAFT?!weapon->Bullet->IsAntiAircraft:!weapon->Bullet->IsAntiGround)) return FIRE_CANT;
    if(!In_Range(target,which,false)) return FIRE_RANGE;
    if(Ammo==0) return FIRE_AMMO;
    return FireStatus;
}
struct FootClass : TechnoClass {
    void * Team=nullptr;
    bool IsFormationMove=false;
    int FormationSpeed=0, FormationMaxSpeed=0, Height=0;
    void ResetAttackMove() { AttackMove=false; }
    void Clear_Navigation_List() {}
    void Assign_Target(TARGET target) { TarCom=target; }
    void Assign_Destination(TARGET target) { NavCom=target; }
    void Assign_Mission(int mission) { Mission=mission; }
    int Distance(COORDINATE target) const { return ::Distance(Coord,target); }
};
struct UnitClass : FootClass {};
struct InfantryClass : FootClass {};
struct VesselClass : FootClass {};
struct AircraftClass : FootClass {};
struct BuildingClass : TechnoClass { short const * Occupy_List(bool) const { static short list[]={0,-1}; return list; } };
template<class T> struct Heap {
    std::vector<T *> Data;
    int Count() const { return (int)Data.size(); }
    T * Ptr(int i) const { return Data[i]; }
};
Heap<UnitClass> Units;
Heap<InfantryClass> Infantry;
Heap<VesselClass> Vessels;
Heap<AircraftClass> Aircraft;
Heap<BuildingClass> Buildings;
struct OverlayTypeClass { bool IsWall=true; static OverlayTypeClass const & As_Reference(int) { static OverlayTypeClass wall; return wall; } };
struct FakeCell {
    bool Clear=true, Water=false;
    int Overlay=OVERLAY_NONE, Zones[4]={1,1,1,1};
    BuildingClass * Building=nullptr;
    BuildingClass * Cell_Building() const { return Building; }
    bool Is_Clear_To_Move(int speed,bool,bool,int,int) const { return Clear && (speed==SPEED_FLOAT ? Water : !Water); }
};
struct FakeMap {
    FakeCell Cells[MAP_CELL_TOTAL];
    bool In_Radar(CELL c) const { return c>=0 && c<MAP_CELL_TOTAL; }
    FakeCell & operator[](CELL c) { assert(In_Radar(c)); return Cells[c]; }
    bool Passes_Proximity_Check(TechnoTypeClass const *,int,short const *,CELL) const { return true; }
    CELL Nearby_Location(CELL cell,int,int,int) const { return In_Radar(cell)?cell:0; }
} Map;
bool TechnoTypeClass::Legal_Placement(CELL cell) const { return Map.In_Radar(cell)&&Map[cell].Clear&&!Map[cell].Building; }
struct HouseClass {
    struct HouseType { int House; } OwnClass;
    HouseType * Class=&OwnClass;
    bool IsHuman=false, IsActive=true, IsDefeated=false, IsTiberiumShort=false;
    int BQuantity[11]={}, UQuantity[6]={}, Attack=0, Radius=3*256, Power=2000, Drain=500, ActLike=0, LAType=0;
    int AITeslaGunLimit=5, AITurretGunLimit=5, AIFlameTurretLimit=4, AIPillBoxLimit=4, AIAirdefenseLimit=6;
    COORDINATE Center=Cell_Coord(Cell(20,43));
    AIStrategy::EnemyForces Forces;
    HouseClass(int id) { OwnClass.House=id; }
    int AlliedHouse=-1;
    bool Is_Ally(HouseClass const * other) const { return this==other || other->Class->House==AlliedHouse; }
    void AI_CalcDynamics() {}
    bool Can_Build(TechnoTypeClass const * type,int) const { return type->Allowed; }
    int Which_Zone(CELL cell) const { return ::Distance(Center,Cell_Coord(cell))<=10*256?0:-1; }
    AIStrategy::EnemyForces AI_Enemy_Forces() const { return Forces; }
    static void AI_Tactics_Init();
    void AI_Tactical_Attacked(BuildingClass const *);
    bool AI_Update_Tactics();
    void AI_Replan_Attack();
    COORDINATE AI_Defense_Location(BuildingClass const *) const;
    StructType AI_Threat_Defense() const;
};
const int ZONE_NONE=-1;
#include "tactics_controller.inc"

static int checks=0;
static void check(bool condition,char const * text)
{
    ++checks;
    if (!condition) { std::cerr<<"FAIL: "<<text<<'\n'; std::exit(1); }
}
struct World {
    HouseClass AI{0}, Enemy{1};
    std::vector<std::unique_ptr<TechnoClass>> Objects;
    std::vector<std::unique_ptr<TechnoTypeClass>> Types;
    World()
    {
        HouseClass::AI_Tactics_Init(); Frame=901; Session.Type=1; RandomResult=99; RandomCalls=0;
        Map=FakeMap(); Targets.clear(); Units.Data.clear(); Buildings.Data.clear();
        Infantry.Data.clear(); Vessels.Data.clear(); Aircraft.Data.clear();
        AI.BQuantity[STRUCT_REFINERY]=1; AI.UQuantity[UNIT_HARVESTER]=1;
        AirBullet.IsAntiAircraft=true; AntiAir.Bullet=&AirBullet;
        for (int i=0;i<11;++i) { BuildingTypeClass::Types[i]=BuildingTypeClass(); BuildingTypeClass::Types[i].Type=i; }
    }
    template<class T> T * Add(HouseClass & house,int x,int y,int kind,int type,int cost=1000,bool armed=true)
    {
        auto specification=std::make_unique<TechnoTypeClass>(); specification->Type=type; specification->Cost=cost;
        if (!armed) specification->PrimaryWeapon=nullptr;
        auto unit=std::make_unique<T>(); T * result=unit.get();
        result->House=&house; result->Class=specification.get(); result->Kind=kind; result->Coord=Cell_Coord(Cell(x,y));
        result->Handle=(TARGET)Objects.size()+1; Targets[result->Handle]=result;
        Objects.push_back(std::move(unit)); Types.push_back(std::move(specification)); return result;
    }
    UnitClass * Tank(HouseClass & house,int x,int y)
    {
        UnitClass * result=Add<UnitClass>(house,x,y,RTTI_UNIT,UNIT_HTANK);
        Units.Data.push_back(result); return result;
    }
    BuildingClass * Building(HouseClass & house,int x,int y,int type,int cost=2000,bool armed=false)
    {
        BuildingClass * result=Add<BuildingClass>(house,x,y,RTTI_BUILDING,type,cost,armed);
        Buildings.Data.push_back(result); Map[Cell(x,y)].Building=result; return result;
    }
    void Army(int number)
    {
        for (int i=0;i<number;++i) Tank(AI,20+i%5,40+i/5);
        Building(AI,19,43,STRUCT_REFINERY);
    }
    void Tick(bool arrive=false,int seconds=1)
    {
        if (arrive) for (auto & object:Objects) {
            if (object->House==&AI && object->Mission==MISSION_MOVE && (object->NavCom&0x80000000u))
                object->Coord=Cell_Coord((CELL)(object->NavCom&0x7fffffffu));
        }
        Frame+=seconds*TICKS_PER_SECOND;
        if (AI.Attack>0) AI.Attack=(std::max)(0,AI.Attack-seconds*TICKS_PER_SECOND);
        AI.AI_Update_Tactics();
    }
};

int main()
{

    {
        World w; w.Army(31); auto target=w.Building(w.Enemy,80,43,STRUCT_CONST);
        Frame=30; Tactical_State(&w.AI).NextAttack=Frame+5*TICKS_PER_MINUTE;
        TacticalStates[0].NextAllOutRoll=Frame+5*TICKS_PER_MINUTE;
        TacticalStates[0].NextAllOutStrike=Frame+5*TICKS_PER_MINUTE;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.AllOut && TacticalStates[0].Ground.Members.size()==31,
            "a lead of thirty-one combat units immediately commits all ready ground troops despite ordinary timers and failed random rolls");
        check(TacticalStates[0].Ground.Target==target->As_Target() && RandomCalls==0,
            "the numerical full assault targets an enemy base without consuming native random state");
    }
    {
        World w; w.Army(30); w.Building(w.Enemy,80,43,STRUCT_CONST); w.AI.AI_Update_Tactics();
        check(!TacticalStates[0].Ground.AllOut && TacticalStates[0].Ground.Members.size()<30,
            "a lead of exactly thirty units does not trigger the strict numerical assault threshold");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,75,80,STRUCT_REFINERY);
        auto power=w.Building(w.Enemy,80,43,STRUCT_POWER,800);
        for(int i=0;i<2;++i) {
            auto jet=w.Add<AircraftClass>(w.AI,20+i,40,RTTI_AIRCRAFT,0,1200);
            const_cast<TechnoTypeClass *>(jet->Class)->Speed=SPEED_WINGED;
            const_cast<TechnoTypeClass *>(jet->Class)->IsFixedWing=true;
            jet->IsTethered=true; Aircraft.Data.push_back(jet);
        }
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase!=STRIKE_HOLD && TacticalStates[0].Air.Target==power->As_Target(),
            "two ready jets harass enemy power while a ground wave is already active");
        check(Aircraft.Data[0]->Mission==MISSION_ATTACK && Aircraft.Data[1]->TarCom==power->As_Target(),
            "power harassment issues common native attack orders without hover waypoints");
    }
    {
        World w; w.Army(21); auto important=w.Building(w.Enemy,55,43,STRUCT_CONST);
        auto economy=w.Building(w.Enemy,80,43,STRUCT_POWER,800); w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Target==economy->As_Target() && TacticalStates[0].Ground.Target!=important->As_Target(),
            "more than twenty ready tanks prioritize a reachable economic sector over another undefended base building");
    }

    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY); w.AI.AI_Update_Tactics();
        auto members=TacticalStates[0].Ground.Members;
        for(unsigned int i=0;i<members.size();++i) As_Techno(members[i])->Coord=Cell_Coord(Cell(45+(int)i%3,43+(int)i/3));
        auto enemy=w.Tank(w.Enemy,49,44);
        Frame+=3; w.AI.AI_Update_Tactics();
        int firing=0;
        for(auto member:members) if(As_Techno(member)->Mission==MISSION_ATTACK && As_Techno(member)->TarCom==enemy->As_Target()) ++firing;
        check(firing==(int)members.size(),"a marching group immediately fights a close enemy before the next one-second strategic update");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=0; w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Members.size()==20,"a successful large-tank-army random roll commits the whole available ground force");
    }

    {
        World w; w.Army(4); w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase!=STRIKE_HOLD,"four ready tanks can concentrate against an undefended reachable sector");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        w.AI.Attack=5*TICKS_PER_MINUTE;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase!=STRIKE_HOLD,"ready attack forces do not wait for the legacy five-minute attack timer");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int i=0;i<30;++i) w.Tank(w.Enemy,110+i%5,110+i/5);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase!=STRIKE_HOLD,"distant enemy armor does not veto a locally superior raid on a weak sector");
    }
    using namespace AITactics;
    check(Combat_Power(1000,250,500)==500,"wounded units contribute less to the superiority estimate");
    check(Combat_Power(INT_MAX,INT_MAX,INT_MAX)==10000,"combat estimates clamp malformed costs safely");
    check(Combat_Power(1000,1,0)==0,"zero maximum health cannot divide by zero");
    check(!Can_Launch(20,10000,12000,1000),"nearby reachable reinforcements require a sufficient strike force");
    check(!Can_Launch(20,10000,1000,8000),"fortified targets require a local advantage");
    check(Can_Launch(12,12000,10000,6000),"a superior concentrated force can attack a weak sector");
    check(!Cohesion_Ready(20,4,20000,4000),"the first few arrivals cannot start an attack");
    check(!Cohesion_Ready(20,19,20000,14000),"many light scouts cannot replace the missing heavy force");
    check(Cohesion_Ready(20,16,20000,16000),"a concentrated majority can advance");
    check(!Can_Launch(INT_MAX,INT_MAX,INT_MAX,INT_MAX),"superiority multiplication cannot overflow");
    check(Should_Withdraw(9000,10000,1000),"reinforcements can turn an attack into a withdrawal");
    check(!Economy_Ready(1,0,false),"lost harvesting capacity suspends new offensives");
    check(Economy_Ready(0,0,true),"ore exhaustion permits the surviving army to finish a match");
    check(Weak_Point_Score(2000,0,50)>Weak_Point_Score(3500,6000,20),"weak defenses matter more than a nearby valuable fortress");
    {
        RouteMap map(12,12); map.Clear.assign(144,1);
        for(int y=0;y<12;++y) map.Clear[y*12+6]=0;
        map.Search(3*12+2);
        check(map.Approach(3*12+10,2)<0,"an island or closed corridor is not a reachable weak sector");
        map.Clear[9*12+6]=1; map.Search(3*12+2);
        std::vector<int> path=map.Path(3*12+10);
        check(!path.empty() && path.size()>10,"a distant opening produces a real detour, not a straight-line route");
        bool used_opening=false; for(unsigned int i=0;i<path.size();++i) if(path[i]==9*12+6) used_opening=true;
        check(used_opening,"the computed march uses the available corridor");
        map.Clear.assign(144,0); map.Clear[0]=map.Clear[13]=1; map.Search(0);
        check(map.Distance[13]<0,"diagonal corners do not create a phantom route");
        map.Clear.assign(144,1); map.Search(11);
        check(map.Distance[12]>1,"route traversal cannot wrap across a map edge");
        map.Danger.assign(144,0);
        for(int y=0;y<9;++y) map.Danger[y*12+6]=40;
        map.Search(3*12+2); path=map.Path(3*12+10);
        bool safe=true;
        for(unsigned int i=0;i<path.size();++i) if(map.Danger[path[i]]>0) safe=false;
        check(safe, "route planning takes a longer safe flank instead of crossing a fortified strip");
    }
    {
        World w; w.Army(20);
        BuildingClass * weak=w.Building(w.Enemy,70,32,STRUCT_REFINERY);
        w.Building(w.Enemy,70,76,STRUCT_CONST);
        for(int i=0;i<5;++i) w.Building(w.Enemy,67+i,74,STRUCT_TESLA,1500,true);
        for(int i=0;i<6;++i) w.Tank(w.Enemy,70+i,78);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Target==weak->As_Target(),"the actual controller chooses the accessible weak economic flank");
        check(TacticalStates[0].Ground.Members.size()<20,"an attack retains a base reserve");
        bool attacking=false; for(UnitClass * unit:Units.Data) if(unit->House==&w.AI && unit->Mission==MISSION_ATTACK) attacking=true;
        check(!attacking,"an attack starts with regrouping rather than individual hunt orders");
        check(w.AI.Attack>0,"only a real attack consumes the configured attack interval");
        int old_step=TacticalStates[0].Ground.Step;
        w.Tick(false);
        check(TacticalStates[0].Ground.Step<=old_step+4,"the march advances in bounded steps");
        for(int i=0;i<30 && TacticalStates[0].Ground.Phase!=STRIKE_ENGAGE;++i) w.Tick(true);
        check(TacticalStates[0].Ground.Phase==STRIKE_ENGAGE,"the regrouped force reaches the chosen attack region");
        int committed=0; for(UnitClass * unit:Units.Data) if(unit->House==&w.AI && unit->TarCom==weak->As_Target()) ++committed;
        check(committed>=12,"the concentrated army attacks one common target");
        BuildingClass * next=w.Building(w.Enemy,80,32,STRUCT_WEAP);
        weak->Strength=0; w.Tick();
        check(TacticalStates[0].Ground.Target==next->As_Target(), "a successful assault continues onto another weak target");
        check(TacticalStates[0].Ground.Home%128>=60, "the successful wave regroups at the front instead of marching home");
        for(int i=0;i<30;++i) w.Tank(w.Enemy,60+i%8,30+i/8);
        w.Tick();
        check(TacticalStates[0].Ground.Fighting,"reinforcements already in firing range trigger immediate battle instead of a march or retreat order");
        check(TacticalStates[0].Ground.Target==next->As_Target(),"a close battle preserves the strategic target for later continuation");
    }
    {
        World w; w.Army(8); w.Building(w.Enemy,80,40,STRUCT_REFINERY);
        for(int i=0;i<20;++i) w.Tank(w.Enemy,80+i%5,70+i/5);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase!=STRIKE_HOLD,"distant enemy armor leaves an undefended refinery open to a local strike");
        check(w.AI.Attack<=45*TICKS_PER_SECOND,"a launched wave uses a short configurable opportunity interval");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,40,STRUCT_REFINERY);
        for(int y=0;y<128;++y) Map[Cell(50,y)].Clear=false;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"the actual controller rejects a disconnected enemy base");
        for(int y=60;y<66;++y) Map[Cell(50,y)].Clear=true;
        w.Tick(false,5);
        check(TacticalStates[0].Ground.Phase!=STRIKE_HOLD,"opening a real route allows a later tactical scan to attack");
        for(int y=60;y<66;++y) Map[Cell(50,y)].Clear=false;
        w.Tick(false,5);
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"closing the corridor during a march stops the assault");
    }
    {
        World w; w.Army(12); BuildingClass * enemy=w.Building(w.Enemy,80,40,STRUCT_REFINERY);
        w.AI.AI_Update_Tactics();
        w.Tick(false);
        w.Tick(false,41);
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"a stalled rally times out without releasing a scattered wave");
        check(enemy->Strength>0,"a timed-out rally never receives a premature attack order");
    }
    {
        World w; w.Army(16);
        UnitClass * harvester=w.Add<UnitClass>(w.AI,20,25,RTTI_UNIT,UNIT_HARVESTER,1400,false); Units.Data.push_back(harvester);
        harvester->Mission=MISSION_MOVE; harvester->NavCom=As_Target(Cell(21,25));
        UnitClass * near1=w.Tank(w.AI,20,26), * near2=w.Tank(w.AI,21,26), * near3=w.Tank(w.AI,22,26), * near4=w.Tank(w.AI,23,26);
        UnitClass * attacker=w.Tank(w.Enemy,23,25); w.Tank(w.Enemy,24,25); w.Tank(w.Enemy,25,25);
        TARGET old_nav=harvester->NavCom;
        w.AI.AI_Update_Tactics();
        check(near1->TarCom==attacker->As_Target() && near2->TarCom==attacker->As_Target()
            && near3->TarCom==attacker->As_Target() && near4->TarCom==attacker->As_Target(),"the closest reachable army defends an outlying harvester");
        check(Units.Data[0]->TarCom!=attacker->As_Target(),"sufficient nearby defenders leave distant troops available");
        check(harvester->NavCom==old_nav && harvester->Mission==MISSION_MOVE,"defense orders never commandeer a harvester");
        check(TacticalStates[0].ThreatPower==3000,"the defensive response measures the whole local enemy force");
        check(w.AI.AI_Threat_Defense()!=STRUCT_NONE,"a threatened economic area requests a legal defensive building");
    }
    {
        World w; w.Army(16);
        UnitClass * mcv=w.Add<UnitClass>(w.AI,20,25,RTTI_UNIT,UNIT_MCV,2500,false); Units.Data.push_back(mcv);
        mcv->Mission=MISSION_MOVE; mcv->NavCom=As_Target(Cell(21,25));
        UnitClass * near1=w.Tank(w.AI,20,26), * near2=w.Tank(w.AI,21,26), * near3=w.Tank(w.AI,22,26), * near4=w.Tank(w.AI,23,26);
        UnitClass * attacker=w.Tank(w.Enemy,23,25); w.Tank(w.Enemy,24,25); w.Tank(w.Enemy,25,25);
        TARGET old_nav=mcv->NavCom;
        w.AI.AI_Update_Tactics();
        check(near1->TarCom==attacker->As_Target() && near2->TarCom==attacker->As_Target()
            && near3->TarCom==attacker->As_Target() && near4->TarCom==attacker->As_Target(),"the closest reachable army defends an expanding MCV");
        check(Units.Data[0]->TarCom!=attacker->As_Target(),"sufficient nearby defenders leave distant troops available");
        check(mcv->NavCom==old_nav && mcv->Mission==MISSION_MOVE,"defense orders never commandeer an expanding MCV");
        check(TacticalStates[0].ThreatPower==3000,"the defensive response measures the whole local enemy force");
        check(w.AI.AI_Threat_Defense()!=STRUCT_NONE,"a threatened economic area requests a legal defensive building");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,70,43,STRUCT_REFINERY);
        w.AI.AI_Update_Tactics();
        BuildingClass * tower=w.Building(w.AI,20,43,STRUCT_TURRET,600,true);
        COORDINATE location=w.AI.AI_Defense_Location(tower);
        check(location!=0 && Coord_Cell(location)%128>20,"defensive towers face the likely enemy approach");
        check(Map[Coord_Cell(location)].Cell_Building()==nullptr,"tower placement respects occupied cells");
        w.AI.UQuantity[UNIT_HARVESTER]=0; w.Tick();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"economic losses stop an active offensive and preserve the base");
        HouseClass::AI_Tactics_Init(); w.Tick();
        bool formation=false; for(UnitClass * unit:Units.Data) if(unit->House==&w.AI && unit->IsFormationMove) formation=true;
        check(!formation,"scenario reset clears temporary formations without changing saved object layouts");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,70,43,STRUCT_REFINERY);
        UnitClass * first=Units.Data[0]; first->Mission=MISSION_HUNT;
        Session.Type=GAME_NORMAL; w.AI.AI_Update_Tactics();
        check(first->Mission==MISSION_HUNT,"campaign scripts retain their original unit orders");
        Session.Type=1; w.AI.IsHuman=true; w.AI.AI_Update_Tactics();
        check(first->Mission==MISSION_HUNT,"human players retain direct unit control");
    }
    {
        World w; w.Army(20);
        UnitClass * mcv=w.Add<UnitClass>(w.Enemy,80,43,RTTI_UNIT,UNIT_MCV,2500,false); Units.Data.push_back(mcv);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Target==mcv->As_Target(), "an opponent's last unarmed MCV remains a target after its buildings are gone");
    }
    {
        World w; w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int i=0;i<8;++i) {
            AircraftClass * jet=w.Add<AircraftClass>(w.AI,20+i%4,40+i/4,RTTI_AIRCRAFT,0,1200);
            auto type=const_cast<TechnoTypeClass *>(jet->Class);
            type->Speed=SPEED_WINGED; type->IsFixedWing=true; type->MaxAmmo=-1;
            jet->Ammo=-1; jet->IsTethered=true; Aircraft.Data.push_back(jet);
        }
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Air.Phase==STRIKE_ENGAGE, "the engine's unlimited-ammo sentinel remains a usable air force");
    }
    {
        World w; w.Army(8);
        for(UnitClass * tank:Units.Data) if(tank->House==&w.AI) const_cast<TechnoTypeClass *>(tank->Class)->SecondaryWeapon=&AntiAir;
        AircraftClass * raid=w.Add<AircraftClass>(w.Enemy,22,43,RTTI_AIRCRAFT,0,1200);
        const_cast<TechnoTypeClass *>(raid->Class)->Speed=SPEED_WINGED; Aircraft.Data.push_back(raid);
        w.AI.AI_Update_Tactics();
        int defenders=0; for(UnitClass * tank:Units.Data) if(tank->House==&w.AI && tank->TarCom==raid->As_Target()) ++defenders;
        check(defenders>=2, "Mammoth-style secondary anti-air missiles are recognized when defending the base");
    }
    {
        World w; w.Army(80); w.AI.Center=Cell_Coord(Cell(20,20));
        for(int y=0;y<128;++y) for(int x=0;x<128;++x) if(x<20 || y<20) Map[Cell(x,y)].Clear=false;
        w.Building(w.Enemy,90,50,STRUCT_REFINERY);
        w.AI.AI_Update_Tactics();
        int rally=TacticalStates[0].Ground.Home;
        check(rally%128>=23 && rally/128>=23, "corner bases select a rally area with room for the army inside the map");
    }
    {
        World w; w.Army(200); w.Building(w.Enemy,90,43,STRUCT_REFINERY);
        w.AI.AI_Update_Tactics();
        std::vector<TARGET> slots;
        for(TARGET handle:TacticalStates[0].Ground.Members) {
            TechnoClass * unit=As_Techno(handle);
            if(unit && unit->Mission==MISSION_MOVE) slots.push_back(unit->NavCom);
        }
        std::sort(slots.begin(),slots.end());
        auto unique=std::unique(slots.begin(),slots.end());
        check((int)(unique-slots.begin())>100, "large attack forces spread over distinct rally cells instead of reusing a small 25-cell grid");
        w.Tick(true);
        check(TacticalStates[0].Ground.Phase==STRIKE_ADVANCE, "a large army can complete regrouping and begin a coherent advance");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int i=0;i<5;++i) w.Building(w.Enemy,45+i,43,STRUCT_TESLA,1500,true);
        w.AI.AI_Update_Tactics();
        bool crossed=false;
        for(int cell:TacticalStates[0].Ground.Route) if(std::abs(cell%128-47)<=4 && std::abs(cell/128-43)<=4) crossed=true;
        check(!crossed && !TacticalStates[0].Ground.Route.empty(), "the actual ground assault goes around concentrated defenses en route to the weak flank");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        w.AI.AI_Update_Tactics();
        for(int i=0;i<10;++i) w.Building(w.Enemy,77+i,45,STRUCT_TESLA,2000,true);
        w.Tick(false,5);
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD, "a newly fortified target is rejected before the army reaches it");
        check(w.AI.Attack<=15*TICKS_PER_SECOND, "an aborted assault can reassess after regrouping without a full interval penalty");
    }
    {
        World w;
        w.Building(w.AI,24,50,STRUCT_REFINERY);
        for(int y=0;y<128;++y) Map[Cell(25,y)].Clear=false;
        UnitClass * blocked=w.Tank(w.AI,26,50);
        UnitClass * reachable=w.Tank(w.AI,20,54);
        for(int i=0;i<4;++i) w.Tank(w.AI,20+i,55);
        UnitClass * attacker=w.Tank(w.Enemy,23,50); w.Tank(w.Enemy,22,50); w.Tank(w.Enemy,21,50);
        w.AI.AI_Update_Tactics();
        check(blocked->TarCom!=attacker->As_Target(),"a nearby defender behind a closed wall is not treated as reachable");
        check(reachable->TarCom==attacker->As_Target(),"defense searches another actual component even when native zone IDs match");
    }
    {
        World w; w.Army(12);
        for(int side=0;side<2;++side) {
            int y=side?85:25;
            UnitClass * harvester=w.Add<UnitClass>(w.AI,20,y,RTTI_UNIT,UNIT_HARVESTER,1400,false); Units.Data.push_back(harvester);
            for(int i=0;i<4;++i) w.Tank(w.AI,20+i,y+1);
            for(int i=0;i<3;++i) w.Tank(w.Enemy,23+i,y);
        }
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].DefenseTargets.size()==2,"simultaneous raids on separate ore fields receive distinct defense responses");
        int first=0,second=0;
        for(UnitClass * unit:Units.Data) if(unit->House==&w.AI) {
            if(unit->TarCom==TacticalStates[0].DefenseTargets[0]) ++first;
            if(unit->TarCom==TacticalStates[0].DefenseTargets[1]) ++second;
        }
        check(first>=4 && second>=4,"nearby forces cover both raids instead of all responding to one side");
    }
    {
        World w;
        for(int y=60;y<128;++y) for(int x=0;x<128;++x) Map[Cell(x,y)].Water=true;
        BulletTypeClass torpedo; torpedo.IsSubSurface=true;
        WeaponTypeClass torpedo_weapon; torpedo_weapon.Bullet=&torpedo;
        for(int i=0;i<12;++i) {
            VesselClass * sub=w.Add<VesselClass>(w.AI,20+i%4,70+i/4,RTTI_VESSEL,0);
            const_cast<TechnoTypeClass *>(sub->Class)->Speed=SPEED_FLOAT;
            const_cast<TechnoTypeClass *>(sub->Class)->PrimaryWeapon=&torpedo_weapon;
            Vessels.Data.push_back(sub);
        }
        w.Building(w.Enemy,70,59,STRUCT_REFINERY);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Fleet.Phase==STRIKE_HOLD,"torpedo boats do not receive impossible land-building attacks");
        VesselClass * target=nullptr;
        for(int i=0;i<3;++i) {
            VesselClass * vessel=w.Add<VesselClass>(w.Enemy,70+i,70,RTTI_VESSEL,0);
            const_cast<TechnoTypeClass *>(vessel->Class)->Speed=SPEED_FLOAT; Vessels.Data.push_back(vessel);
            if(!target) target=vessel;
        }
        w.Tick(false,5);
        check(TacticalStates[0].Fleet.Target==target->As_Target(),"a submarine fleet can concentrate on enemy ships even while enemy land buildings exist");
        check(TacticalStates[0].Fleet.Members.size()>=8,"a naval assault also retains a concentrated force");
    }
    {
        World w;
        BuildingClass * target=w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int i=0;i<8;++i) {
            AircraftClass * plane=w.Add<AircraftClass>(w.AI,20+i%4,40+i/4,RTTI_AIRCRAFT,0,1200);
            const_cast<TechnoTypeClass *>(plane->Class)->Speed=SPEED_WINGED; Aircraft.Data.push_back(plane);
        }
        for(int y=0;y<128;++y) Map[Cell(50,y)].Clear=false;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Air.Target==target->As_Target(),"air-only strategies retain coordinated attacks without a land route");
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"planes never count as members of a ground column");
        AircraftClass * rearming=Aircraft.Data[2]; rearming->Mission=MISSION_ENTER; rearming->Ammo=0;
        w.Tick();
        check(rearming->Mission==MISSION_ENTER && !rearming->IsFormationMove,"air-wave updates preserve rearming and release formation speed overrides");
    }
    {
        World w; BuildingClass * target=w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int i=0;i<8;++i) {
            AircraftClass * jet=w.Add<AircraftClass>(w.AI,20+i%4,40+i/4,RTTI_AIRCRAFT,0,1200);
            const_cast<TechnoTypeClass *>(jet->Class)->Speed=SPEED_WINGED;
            const_cast<TechnoTypeClass *>(jet->Class)->IsFixedWing=true; jet->IsTethered=true; Aircraft.Data.push_back(jet);
        }
        w.AI.AI_Update_Tactics();
        int sorties=0;
        for(AircraftClass * jet:Aircraft.Data) if(jet->Mission==MISSION_ATTACK && jet->TarCom==target->As_Target()) ++sorties;
        check(sorties>=5, "ready fixed-wing jets receive a synchronized common sortie instead of hover commands");
        check(TacticalStates[0].Air.Members.size()>=5, "fully reloaded jets can launch while still docked to an airfield");
    }
    {
        World w; w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int i=0;i<8;++i) {
            AircraftClass * jet=w.Add<AircraftClass>(w.AI,20+i%4,40+i/4,RTTI_AIRCRAFT,0,1200);
            const_cast<TechnoTypeClass *>(jet->Class)->Speed=SPEED_WINGED;
            const_cast<TechnoTypeClass *>(jet->Class)->IsFixedWing=true;
            jet->IsTethered=true; jet->Ammo=1; Aircraft.Data.push_back(jet);
        }
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Air.Phase==STRIKE_HOLD, "partly reloaded aircraft stay at their pads instead of launching a weak sortie");
    }
    {
        World w;
        BulletTypeClass aa; aa.IsAntiGround=false; aa.IsAntiAircraft=true;
        WeaponTypeClass aa_weapon; aa_weapon.Bullet=&aa;
        w.Building(w.Enemy,70,40,STRUCT_CONST);
        for(int i=0;i<6;++i) {
            BuildingClass * sam=w.Building(w.Enemy,68+i,43,STRUCT_SAM,750,true);
            const_cast<TechnoTypeClass *>(sam->Class)->PrimaryWeapon=&aa_weapon;
        }
        BuildingClass * weak=w.Building(w.Enemy,70,80,STRUCT_REFINERY);
        for(int i=0;i<8;++i) {
            AircraftClass * plane=w.Add<AircraftClass>(w.AI,20+i%4,40+i/4,RTTI_AIRCRAFT,0,1200);
            const_cast<TechnoTypeClass *>(plane->Class)->Speed=SPEED_WINGED; Aircraft.Data.push_back(plane);
        }
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Air.Target==weak->As_Target(),"air waves favor weak anti-air coverage over a valuable SAM-protected base");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,70,43,STRUCT_REFINERY);
        UnitClass * scripted=Units.Data[0]; scripted->Team=scripted; scripted->Mission=MISSION_HUNT;
        UnitClass * exiting=Units.Data[1]; exiting->IsTethered=true; exiting->Mission=MISSION_MOVE;
        w.AI.AI_Update_Tactics();
        check(scripted->Mission==MISSION_HUNT && !scripted->IsFormationMove,"scripted teams retain their existing control");
        check(exiting->Mission==MISSION_MOVE && !exiting->IsFormationMove,"factory exits are not commandeered while tethered");
    }
    {
        World w; w.Army(3); w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"fewer than four ready combat units still wait for a concentrated force");
    }
    {
        World w; w.Army(8); auto strong=w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        auto weak=w.Building(w.Enemy,20,90,STRUCT_WEAP);
        for(int i=0;i<30;++i) w.Tank(w.Enemy,78+i%5,40+i/5);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Target==weak->As_Target() && TacticalStates[0].Ground.Target!=strong->As_Target(),
            "a smaller army chooses the undefended flank and avoids the enemy main army");
    }
    {
        World w; w.Army(12); auto original=w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        auto alternate=w.Building(w.Enemy,80,95,STRUCT_REFINERY);
        std::vector<UnitClass *> enemy;
        for(int i=0;i<30;++i) enemy.push_back(w.Tank(w.Enemy,110+i%5,110+i/5));
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Target==original->As_Target(),"the initial strike chooses the closer undefended sector");
        auto members=TacticalStates[0].Ground.Members;
        for(int i=0;i<30;++i) enemy[i]->Coord=Cell_Coord(Cell(78+i%5,40+i/5));
        w.Tick(false,3);
        check(TacticalStates[0].Ground.Target==alternate->As_Target(),"moving the enemy main army to the target changes strategy at the next three-second assessment");
        check(TacticalStates[0].Ground.Members.size()==members.size(),"retargeting preserves the concentrated surviving task force");
        check(TacticalStates[0].Ground.Phase!=STRIKE_HOLD,"a new reachable weak sector keeps the wave active");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int i=0;i<25;++i) w.Tank(w.Enemy,95+i%3,41+i/3);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"reachable reinforcements near a nominally undefended target prevent a suicidal strike");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,70,STRUCT_REFINERY);
        auto raider=w.Tank(w.Enemy,28,43); w.AI.AI_Update_Tactics();
        int defenders=0;
        for(auto unit:Units.Data) if(unit->House==&w.AI && unit->TarCom==raider->As_Target()) ++defenders;
        check(defenders>=2,"nearest forces still cover a raid before spare forces attack");
        check(TacticalStates[0].Ground.Phase!=STRIKE_HOLD,"a contained small raid does not freeze all offensive operations");
        for(auto member:TacticalStates[0].Ground.Members)
            check(!Contains(TacticalStates[0].Defenders,member),"a defending unit cannot simultaneously join the strike");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); w.AI.AI_Update_Tactics();
        for(int i=0;i<30;++i) w.Tank(w.Enemy,45+i%5,40+i/5);
        w.Tick(false,3);
        bool crossed=false;
        for(auto cell:TacticalStates[0].Ground.Route)
            if(std::abs(cell%128-47)<=5 && std::abs(cell/128-43)<=5) crossed=true;
        check(!crossed && TacticalStates[0].Ground.Phase!=STRIKE_HOLD,"a main army moving onto the march route triggers a detour toward the weak target");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int y=0;y<128;++y) Map[Cell(50,y)].Clear=false;
        for(int y=40;y<=46;++y) Map[Cell(50,y)].Clear=true;
        for(int i=0;i<30;++i) w.Tank(w.Enemy,48+i%5,40+i/5);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"an undefended target beyond a chokepoint held by the main army does not cause a frontal assault");
    }
    {
        World w; w.Army(12); auto base=w.Building(w.Enemy,80,43,STRUCT_REFINERY); w.AI.AI_Update_Tactics();
        auto members=TacticalStates[0].Ground.Members;
        for(unsigned int i=0;i<members.size();++i) As_Techno(members[i])->Coord=Cell_Coord(Cell(i<4?45:75,i<4?40:80));
        auto left=w.Tank(w.Enemy,49,40); auto right=w.Tank(w.Enemy,79,80);
        Frame+=3; w.AI.AI_Update_Tactics();
        bool nearest=true;
        for(unsigned int i=0;i<members.size();++i) nearest&=As_Techno(members[i])->TarCom==(i<4?left:right)->As_Target();
        check(nearest,"simultaneous contacts on both sides are assigned to nearby shooters rather than one remote fight");
        check(TacticalStates[0].Ground.Target==base->As_Target(),"local target assignments retain the original strategic objective");
        left->Strength=right->Strength=0; w.Tick();
        check(!TacticalStates[0].Ground.Fighting && TacticalStates[0].Ground.Phase!=STRIKE_HOLD,"ending an encounter resumes the surviving offensive");
        check(TacticalStates[0].Ground.Home%128>=40,"the surviving army regroups at the battle site instead of the old base checkpoint");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY); w.AI.AI_Update_Tactics();
        auto members=TacticalStates[0].Ground.Members;
        for(auto member:members) As_Techno(member)->Coord=Cell_Coord(Cell(45,43));
        auto weak=w.Tank(w.Enemy,48,43); auto other=w.Tank(w.Enemy,48,44); weak->Strength=1;
        Frame+=3; w.AI.AI_Update_Tactics();
        int finishing=0, supporting=0;
        for(auto member:members) {
            if(As_Techno(member)->TarCom==weak->As_Target()) ++finishing;
            if(As_Techno(member)->TarCom==other->As_Target()) ++supporting;
        }
        check(finishing==1 && supporting>0,"one nearby nearly-dead opponent is finished without wasting every tank's volley");
        other->Coord=weak->Coord=Cell_Coord(Cell(110,110)); Frame+=3; w.AI.AI_Update_Tactics();
        bool stopped=true; for(auto member:members) stopped&=As_Techno(member)->Mission!=MISSION_ATTACK;
        check(stopped && TacticalStates[0].Ground.Resume,"a fleeing enemy outside combat range does not pull the task force into an indefinite chase");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY); w.AI.AI_Update_Tactics();
        auto members=TacticalStates[0].Ground.Members;
        for(auto member:members) As_Techno(member)->Coord=Cell_Coord(Cell(45,43));
        auto hidden=w.Tank(w.Enemy,46,43); hidden->Hidden=true;
        Frame+=3; w.AI.AI_Update_Tactics();
        bool ignored=true; for(auto member:members) ignored&=As_Techno(member)->TarCom!=hidden->As_Target();
        check(ignored && !TacticalStates[0].Ground.Fighting,"native fire restrictions prevent focus orders against an untargetable cloaked contact");
        hidden->Hidden=false; As_Techno(members[0])->Ammo=0; As_Techno(members[1])->FireStatus=FIRE_REARM;
        Frame+=3; w.AI.AI_Update_Tactics();
        check(As_Techno(members[0])->TarCom!=hidden->As_Target(),"an empty magazine does not create a false firing assignment");
        check(As_Techno(members[1])->TarCom==hidden->As_Target(),"normal weapon cooldown keeps the useful combat target while native firing waits");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY); w.AI.AI_Update_Tactics();
        auto members=TacticalStates[0].Ground.Members;
        for(auto member:members) As_Techno(member)->Coord=Cell_Coord(Cell(45,43));
        auto aircraft=w.Add<AircraftClass>(w.Enemy,47,43,RTTI_AIRCRAFT,0); Aircraft.Data.push_back(aircraft);
        auto gun=const_cast<TechnoTypeClass *>(As_Techno(members[0])->Class); gun->SecondaryWeapon=&AntiAir;
        Frame+=3; w.AI.AI_Update_Tactics();
        check(As_Techno(members[0])->TarCom==aircraft->As_Target(),"a secondary anti-air weapon can immediately engage a nearby aircraft");
        check(As_Techno(members[1])->TarCom!=aircraft->As_Target(),"ground-only tanks do not receive impossible anti-air orders");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY); w.AI.AI_Update_Tactics();
        auto members=TacticalStates[0].Ground.Members;
        for(auto member:members) As_Techno(member)->Coord=Cell_Coord(Cell(45,43));
        auto artillery=w.Tank(w.Enemy,52,43); auto weapon=std::make_unique<WeaponTypeClass>(Cannon); weapon->Range=8*CELL_LEPTON_W;
        const_cast<TechnoTypeClass *>(artillery->Class)->PrimaryWeapon=weapon.get();
        Frame+=3; w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Fighting && As_Techno(members[0])->TarCom==artillery->As_Target(),"incoming longer-range fire triggers immediate grouped retaliation");
        w.AI.UQuantity[UNIT_HARVESTER]=0; w.Tick();
        check(TacticalStates[0].Ground.Fighting && As_Techno(members[0])->Mission==MISSION_ATTACK,"an economic loss does not erase defensive firing orders during a close battle");
    }
    {
        World w; auto reserve=w.Tank(w.AI,45,43); auto enemy=w.Tank(w.Enemy,48,43); w.AI.AI_Update_Tactics();
        check(reserve->TarCom==enemy->As_Target() && reserve->Mission==MISSION_ATTACK,"an uncommitted reserve in direct firing range responds instead of walking past an enemy");
    }
    {
        World w; w.Army(19); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=0; w.AI.AI_Update_Tactics();
        check(!TacticalStates[0].Ground.AllOut && RandomCalls==0,"an army below the tank threshold does not consume an all-out random roll");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=24; w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.AllOut && TacticalStates[0].Ground.Members.size()==20,"the lower quarter of native random rolls launches a full ground assault");
        check(RandomCalls==1,"one eligible opportunity makes one synchronized native random roll");
        auto harvester=w.Add<UnitClass>(w.AI,20,43,RTTI_UNIT,UNIT_HARVESTER,1400,false); Units.Data.push_back(harvester);
        auto mcv=w.Add<UnitClass>(w.AI,21,43,RTTI_UNIT,UNIT_MCV,2500,false); Units.Data.push_back(mcv);
        check(!Contains(TacticalStates[0].Ground.Members,harvester->As_Target()) && !Contains(TacticalStates[0].Ground.Members,mcv->As_Target()),"full assaults preserve economic and construction vehicles");
        w.Tick(false,30);
        check(RandomCalls==1,"a successful all-out strike observes its cooldown rather than repeatedly rerolling");
        check(TacticalStates[0].NextAllOutStrike>Frame,"the full-assault cooldown persists while an attack is active");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=25; w.AI.AI_Update_Tactics();
        check(!TacticalStates[0].Ground.AllOut && TacticalStates[0].Ground.Members.size()<20,"a failed random roll uses the ordinary defensive reserve");
        w.Tick(false,1); check(RandomCalls==1,"normal tactical ticks cannot reroll the same full-assault opportunity");
        RandomResult=0; w.Tick(false,29);
        check(TacticalStates[0].Ground.AllOut && TacticalStates[0].Ground.Members.size()==20,"a later successful roll can promote a marching wave with its remaining reserves");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=0;
        for(auto unit:Units.Data) unit->Strength=249;
        w.AI.AI_Update_Tactics();
        check(!TacticalStates[0].Ground.AllOut && RandomCalls==0,"twenty severely damaged tanks do not qualify as a strong full-assault army");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=0;
        Units.Data[0]->Team=Units.Data[0]; w.AI.AI_Update_Tactics();
        check(!TacticalStates[0].Ground.AllOut && RandomCalls==0,"scripted tanks are excluded from the available full-assault threshold");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=0; w.Tank(w.Enemy,28,43);
        w.AI.AI_Update_Tactics();
        check(!TacticalStates[0].Ground.AllOut && RandomCalls==0,"a threatened base postpones emptying its remaining garrison");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=0;
        for(int y=0;y<128;++y) Map[Cell(50,y)].Clear=false;
        for(int i=10;i<20;++i) Units.Data[i]->Coord=Cell_Coord(Cell(70+i%5,40+i/5));
        w.AI.AI_Update_Tactics();
        check(!TacticalStates[0].Ground.AllOut,"twenty tanks split between disconnected islands cannot pretend to be one full-assault force");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=0;
        for(int y=0;y<128;++y) Map[Cell(50,y)].Clear=false;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD && !TacticalStates[0].Ground.AllOut,"a full-assault random success never overrides missing ground access");
    }
    {
        World w; w.Army(20); w.Building(w.Enemy,80,43,STRUCT_REFINERY); RandomResult=0;
        for(auto unit:Units.Data) const_cast<TechnoTypeClass *>(unit->Class)->Type=UNIT_TESLATANK;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.AllOut,"Aftermath Tesla tanks count toward the available tank force");
    }
    {
        World w; w.Army(20); auto first=w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        auto second=w.Building(w.Enemy,100,43,STRUCT_WEAP); RandomResult=0; w.AI.AI_Update_Tactics();
        first->Strength=0; w.Tick();
        check(TacticalStates[0].Ground.AllOut && TacticalStates[0].Ground.Target==second->As_Target(),"a successful full assault retains its full-force policy on the next strategic target");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY); w.AI.AI_Update_Tactics();
        auto members=TacticalStates[0].Ground.Members;
        for(auto member:members) As_Techno(member)->Coord=Cell_Coord(Cell(45,43));
        auto enemy=w.Tank(w.Enemy,48,43); const_cast<TechnoTypeClass *>(enemy->Class)->Armor=1;
        WarheadTypeClass organic; organic.Modifier[1]=0;
        auto weapon=std::make_unique<WeaponTypeClass>(Cannon); weapon->WarheadPtr=&organic;
        auto first=const_cast<TechnoTypeClass *>(As_Techno(members[0])->Class); first->PrimaryWeapon=weapon.get();
        Frame+=3; w.AI.AI_Update_Tactics();
        check(As_Techno(members[0])->TarCom!=enemy->As_Target(),"a weapon that cannot damage the target's armor does not waste a focus-fire order");
        first->SecondaryWeapon=&Cannon; Frame+=3; w.AI.AI_Update_Tactics();
        check(As_Techno(members[0])->TarCom==enemy->As_Target(),"a useful secondary weapon can engage when the primary cannot damage that armor");
    }
    {
        World w; w.Building(w.AI,19,43,STRUCT_REFINERY);
        auto near=w.Tank(w.AI,22,43); auto side=w.Tank(w.AI,27,47);
        auto first=w.Tank(w.Enemy,23,43); auto second=w.Tank(w.Enemy,27,48);
        w.AI.AI_Update_Tactics();
        check(near->TarCom==first->As_Target() && side->TarCom==second->As_Target(),"defenders engage the enemy nearest their own firing position instead of all chasing the raid anchor");
    }

    check(!Numerical_Advantage(40,10) && Numerical_Advantage(41,10),
        "the numerical policy uses a strict difference of more than thirty");
    check(Numerical_Advantage(INT_MAX,0) && !Numerical_Advantage(0,INT_MAX),
        "combat count comparison cannot overflow");
    check(Attack_Delay(5,false)==10 && Attack_Delay(INT_MAX,false)==15 && Attack_Delay(1,false)==4,
        "normal attack opportunities are shortened with safe configuration bounds");
    {
        World w; w.Army(30); w.Building(w.Enemy,80,43,STRUCT_CONST);
        Tactical_State(&w.AI).NextAttack=Frame+5*TICKS_PER_MINUTE; w.AI.AI_Update_Tactics();
        w.Tank(w.AI,24,43); Frame+=3; w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Forced && TacticalStates[0].Ground.Members.size()==31,
            "crossing the numerical threshold launches at the next three-tick contact check without waiting for a strategic scan");
    }
    {
        World w; w.Army(31); w.Building(w.Enemy,80,43,STRUCT_CONST); Frame=30;
        w.AI.BQuantity[STRUCT_REFINERY]=0; w.AI.UQuantity[UNIT_HARVESTER]=0;
        auto harvester=w.Add<UnitClass>(w.AI,20,44,RTTI_UNIT,UNIT_HARVESTER,1400,false);
        harvester->Mission=MISSION_MOVE; harvester->NavCom=As_Target(Cell(30,44)); Units.Data.push_back(harvester);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Forced && TacticalStates[0].Ground.Members.size()==31,
            "numerical superiority bypasses the ordinary economy and opening-minute attack gates");
        check(harvester->Mission==MISSION_MOVE && harvester->NavCom==As_Target(Cell(30,44)),
            "an immediate full assault preserves an economic vehicle's existing route");
    }
    {
        World w; w.Army(62); HouseClass second(2), ally(3), neutral(HOUSE_NEUTRAL);
        w.AI.AlliedHouse=3; w.Building(w.Enemy,80,43,STRUCT_CONST);
        for(int i=0;i<15;++i) w.Tank(w.Enemy,110+i%5,100+i/5);
        for(int i=0;i<16;++i) w.Tank(second,110+i%5,110+i/5);
        for(int i=0;i<20;++i) { w.Tank(ally,90+i%5,115+i/5); w.Tank(neutral,100+i%5,115+i/5); }
        std::vector<Fighter> army; std::vector<Contact> contacts; int own=0,enemy=0;
        Gather(&w.AI,army,contacts,&own,&enemy);
        check(own==62 && enemy==31,"numerical counts combine all hostile armies and exclude allied and neutral forces");
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Forced,"a lead of thirty-one over multiple enemies still triggers a full assault");
        w.Tank(second,115,115); w.Tick();
        check(!TacticalStates[0].Ground.Forced && !TacticalStates[0].NumericalAssault,
            "a lead falling back to exactly thirty restores normal local safety decisions");
    }
    {
        World w; w.Army(31); w.Building(w.Enemy,80,43,STRUCT_CONST);
        for(int i=0;i<32;++i) {
            w.Building(w.Enemy,78+i%8,45+i/8,STRUCT_TESLA,2000,true);
            auto truck=w.Add<UnitClass>(w.Enemy,100+i%5,90+i/5,RTTI_UNIT,UNIT_HARVESTER,1400,false);
            Units.Data.push_back(truck);
        }
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Forced && TacticalStates[0].Ground.Members.size()==31,
            "enemy towers and unarmed economic vehicles do not count as attack units or veto the explicit numerical assault");
        w.Tick(false,2);
        check(TacticalStates[0].Ground.Forced && TacticalStates[0].Ground.Phase!=STRIKE_HOLD,
            "a numerical assault is not immediately cancelled by the old local power veto at its next scan");
    }
    {
        World w; w.Army(31); w.Building(w.Enemy,80,43,STRUCT_CONST);
        Units.Data[0]->Strength=0; Units.Data[1]->IsInLimbo=true; Units.Data[2]->IsActive=false;
        w.AI.AI_Update_Tactics();
        check(!TacticalStates[0].NumericalAssault,"dead, limbo and inactive units cannot manufacture numerical superiority");
    }
    {
        World w; w.Army(31); w.Building(w.Enemy,80,43,STRUCT_CONST); w.AI.AI_Update_Tactics();
        auto original=TacticalStates[0].Ground.Target; w.Tick(true);
        for(int i=0;i<3;++i) w.Tank(w.AI,22+i,43);
        w.Tick(true,3);
        check(TacticalStates[0].Ground.Members.size()==34 && TacticalStates[0].Ground.Target==original,
            "new ready reinforcements join an ongoing numerical wave without replacing its objective");
        check(TacticalStates[0].Ground.Phase==STRIKE_ADVANCE,
            "reinforcement recruitment does not repeatedly restart an advancing wave at the home rally");
    }
    {
        World w; w.Army(31); w.Building(w.Enemy,80,43,STRUCT_CONST);
        for(int y=0;y<128;++y) Map[Cell(50,y)].Clear=false;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"numerical superiority cannot create a path through a closed map");
        for(int y=40;y<48;++y) Map[Cell(50,y)].Clear=true;
        w.Tick(false,3);
        check(TacticalStates[0].Ground.Forced,"a previously blocked numerical assault retries promptly when a real corridor opens");
    }
    {
        World w; w.Army(22); auto target=w.Building(w.Enemy,80,64,STRUCT_REFINERY);
        for(int y=65;y<128;++y) for(int x=0;x<128;++x) Map[Cell(x,y)].Water=true;
        for(int y=0;y<65;++y) Map[Cell(50,y)].Clear=false;
        for(int i=0;i<5;++i) {
            auto ship=w.Add<VesselClass>(w.AI,20+i,70,RTTI_VESSEL,0);
            const_cast<TechnoTypeClass *>(ship->Class)->Speed=SPEED_FLOAT; Vessels.Data.push_back(ship);
            auto jet=w.Add<AircraftClass>(w.AI,20+i,40,RTTI_AIRCRAFT,0,1200);
            const_cast<TechnoTypeClass *>(jet->Class)->Speed=SPEED_WINGED;
            const_cast<TechnoTypeClass *>(jet->Class)->IsFixedWing=true; Aircraft.Data.push_back(jet);
        }
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].NumericalAssault && TacticalStates[0].Ground.Phase==STRIKE_HOLD,
            "a combined army advantage retains terrain restrictions on its isolated ground troops");
        check(TacticalStates[0].Fleet.Forced && TacticalStates[0].Air.Forced
            && TacticalStates[0].Fleet.Target==target->As_Target() && TacticalStates[0].Air.Target==target->As_Target(),
            "ready compatible ships and aircraft launch parallel full assaults even when the ground route is blocked");
        check(TacticalStates[0].Fleet.Members.size()==5 && TacticalStates[0].Air.Members.size()==5,
            "the numerical assault includes all ready compatible naval and air units without ordinary reserves");
    }
    {
        World w; auto target=w.Building(w.Enemy,80,43,STRUCT_POWER,800);
        auto jet=w.Add<AircraftClass>(w.AI,20,40,RTTI_AIRCRAFT,0,1200);
        const_cast<TechnoTypeClass *>(jet->Class)->Speed=SPEED_WINGED;
        const_cast<TechnoTypeClass *>(jet->Class)->IsFixedWing=true; Aircraft.Data.push_back(jet);
        Tactical_State(&w.AI).NextAttack=Frame+5*TICKS_PER_MINUTE;
        w.AI.AI_Update_Tactics();
        check(jet->Mission==MISSION_ATTACK && jet->TarCom==target->As_Target(),
            "one fully loaded aircraft can harass an exposed power plant independently of the ground timer");
        jet->Ammo=0; jet->Mission=MISSION_ENTER; w.Tick();
        check(jet->Mission==MISSION_ENTER && TacticalStates[0].Air.Phase==STRIKE_HOLD,
            "a harassment sortie releases an empty aircraft to native rearming");
        jet->Ammo=5; jet->Mission=MISSION_GUARD; w.Tick(false,6);
        check(TacticalStates[0].Air.Phase==STRIKE_HOLD,"air harassment observes its own short cooldown while reloading");
        w.Tick(false,2);
        check(jet->Mission==MISSION_ATTACK && jet->TarCom==target->As_Target(),
            "a reloaded aircraft repeats power harassment after eight simulation seconds without the legacy attack delay");
    }
    {
        World w; auto unsafe=w.Building(w.Enemy,70,43,STRUCT_POWER,800);
        auto safe=w.Building(w.Enemy,70,90,STRUCT_ADVANCED_POWER,1500);
        for(int i=0;i<6;++i) {
            auto sam=w.Building(w.Enemy,68+i,45,STRUCT_SAM,750,true);
            const_cast<TechnoTypeClass *>(sam->Class)->PrimaryWeapon=&AntiAir;
        }
        for(int i=0;i<2;++i) {
            auto jet=w.Add<AircraftClass>(w.AI,20+i,40,RTTI_AIRCRAFT,0,1200);
            const_cast<TechnoTypeClass *>(jet->Class)->Speed=SPEED_WINGED;
            const_cast<TechnoTypeClass *>(jet->Class)->IsFixedWing=true; Aircraft.Data.push_back(jet);
        }
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Air.Target==safe->As_Target() && TacticalStates[0].Air.Target!=unsafe->As_Target(),
            "ordinary harassment selects a reachable power plant with weaker anti-air coverage");
    }
    {
        World w; w.Army(20); auto important=w.Building(w.Enemy,55,43,STRUCT_CONST);
        auto economy=w.Building(w.Enemy,80,43,STRUCT_POWER,800); w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Target==important->As_Target(),
            "exactly twenty tanks do not activate the strict economic-sector preference");
        w.Tank(w.AI,24,43); w.Tick(false,6);
        check(TacticalStates[0].Ground.Target==economy->As_Target(),
            "an existing wave redirects toward the economic sector when the live tank count exceeds twenty");
    }
    {
        World w; w.Army(21); auto weak=w.Building(w.Enemy,40,80,STRUCT_CONST);
        auto fortified=w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int i=0;i<15;++i) w.Building(w.Enemy,77+i%5,45+i/5,STRUCT_TESLA,2000,true);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Target==weak->As_Target() && TacticalStates[0].Ground.Target!=fortified->As_Target(),
            "ordinary economic preference retains local safety and falls back to an accessible weak building");
    }
    {
        World w; w.Army(21); auto weak=w.Building(w.Enemy,40,80,STRUCT_CONST);
        w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int y=0;y<128;++y) Map[Cell(50,y)].Clear=false;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Target==weak->As_Target(),
            "an unreachable refinery does not distract a large tank wave from a reachable target");
    }
    {
        World w; w.Army(21); w.Building(w.Enemy,55,43,STRUCT_CONST);
        auto target=w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(auto unit:Units.Data) unit->Strength=249;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Intent==STRIKE_ECONOMY && TacticalStates[0].Ground.Target==target->As_Target(),
            "the economic preference counts ready tanks independently of the old healthy-tank random threshold");
    }
    {
        World w; w.Army(12); w.Building(w.Enemy,80,43,STRUCT_REFINERY);
        for(int i=0;i<20;++i) w.Tank(w.Enemy,78+i%5,40+i/5);
        auto exposed=w.Tank(w.Enemy,70,95); w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Target==exposed->As_Target(),
            "normal waves attack an exposed enemy unit while enemy base buildings remain strongly defended");
        check(w.AI.Attack==10*TICKS_PER_SECOND && TacticalStates[0].NextScan==Frame+2*TICKS_PER_SECOND,
            "normal unit attacks use the faster wave interval and two-second opportunity scan");
    }
    {
        World w; w.Army(4); w.Building(w.Enemy,80,43,STRUCT_CONST);
        WarheadTypeClass harmless; harmless.Modifier[0]=0;
        WeaponTypeClass weapon=Cannon; weapon.WarheadPtr=&harmless;
        for(auto unit:Units.Data) const_cast<TechnoTypeClass *>(unit->Class)->PrimaryWeapon=&weapon;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,
            "strategic target selection never dispatches a cohort whose weapons cannot damage the target armor");
    }
    {
        World w; w.Army(31); w.Building(w.Enemy,80,43,STRUCT_CONST); w.AI.IsHuman=true;
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD && !TacticalStates[0].NumericalAssault,
            "numerical and harassment policies do not commandeer a human army");
        w.AI.IsHuman=false; Session.Type=GAME_NORMAL; w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"campaign control retains its existing behavior");
    }
    std::cout<<checks<<" tactical route, policy and actual controller scenarios passed.\n";
}
