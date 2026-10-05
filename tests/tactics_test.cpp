// Compile the actual AITACTICS.CPP controller against a controlled world.
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
const int HOUSE_COUNT=20, HOUSE_NEUTRAL=18, HOUSE_JP=19, GAME_NORMAL=0;
const int MAP_CELL_W=128, MAP_CELL_H=128, MAP_CELL_TOTAL=16384;
const int TICKS_PER_SECOND=15, TICKS_PER_MINUTE=900, CELL_LEPTON_W=256;
const TARGET TARGET_NONE=0;
const int RTTI_UNIT=1, RTTI_INFANTRY=2, RTTI_VESSEL=3, RTTI_AIRCRAFT=4, RTTI_BUILDING=5;
const int UNIT_HTANK=0, UNIT_HARVESTER=1, UNIT_MCV=2, UNIT_MINELAYER=3, UNIT_MAD=4, UNIT_DEMOTRUCK=5;
const int STRUCT_CONST=0, STRUCT_REFINERY=1, STRUCT_WEAP=2, STRUCT_POWER=3, STRUCT_ADVANCED_POWER=4;
const int STRUCT_TESLA=5, STRUCT_TURRET=6, STRUCT_FLAME_TURRET=7, STRUCT_PILLBOX=8, STRUCT_SAM=9, STRUCT_AAGUN=10, STRUCT_NONE=-1;
const int MISSION_GUARD=0, MISSION_MOVE=1, MISSION_ATTACK=2, MISSION_HUNT=3, MISSION_ENTER=4;
const int MISSION_RETREAT=5, MISSION_CAPTURE=6, MISSION_UNLOAD=7;
const int MPH_IMMOBILE=0, MPH_LIGHT_SPEED=1000, OVERLAY_NONE=-1, SPEED_FLOAT=1, SPEED_WINGED=2;
int Frame=0;
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
struct WeaponTypeClass { BulletTypeClass * Bullet=&GroundBullet; int Attack=40, Range=4*256; } Cannon, AntiAir;
struct TechnoTypeClass {
    WeaponTypeClass const * PrimaryWeapon=&Cannon;
    WeaponTypeClass const * SecondaryWeapon=nullptr;
    int Cost=1000, MaxStrength=500, MaxSpeed=10, Speed=0, MZone=0, Type=0, MaxAmmo=5;
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
    int Strength=500, Kind=RTTI_UNIT, Mission=MISSION_GUARD;
    COORDINATE Coord=0;
    TARGET Handle=0, TarCom=0, NavCom=0;
    virtual ~TechnoClass() {}
    TechnoTypeClass const * Techno_Type_Class() const { return Class; }
    COORDINATE Center_Coord() const { return Coord; }
    bool Is_Foot() const { return Kind != RTTI_BUILDING; }
    int What_Am_I() const { return Kind; }
    TARGET As_Target() const { return Handle; }
};
TechnoClass * As_Techno(TARGET target) { auto it=Targets.find(target); return it==Targets.end()?nullptr:it->second; }
struct FootClass : TechnoClass {
    void * Team=nullptr;
    bool IsFormationMove=false;
    int FormationSpeed=0, FormationMaxSpeed=0, Ammo=5, Height=0;
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
    bool Is_Ally(HouseClass const * other) const { return this==other; }
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
        HouseClass::AI_Tactics_Init(); Frame=901; Session.Type=1;
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
    using namespace AITactics;
    check(Combat_Power(1000,250,500)==500,"wounded units contribute less to the superiority estimate");
    check(Combat_Power(INT_MAX,INT_MAX,INT_MAX)==10000,"combat estimates clamp malformed costs safely");
    check(Combat_Power(1000,1,0)==0,"zero maximum health cannot divide by zero");
    check(!Can_Launch(20,10000,12000,1000),"a weak local target does not override global inferiority");
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
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"the controller cancels an attack when enemy reinforcements remove superiority");
        check(!TacticalStates[0].Ground.Target,"a withdrawn plan cannot keep a stale target");
    }
    {
        World w; w.Army(8); w.Building(w.Enemy,80,40,STRUCT_REFINERY);
        for(int i=0;i<20;++i) w.Tank(w.Enemy,80+i%5,70+i/5);
        w.AI.AI_Update_Tactics();
        check(TacticalStates[0].Ground.Phase==STRIKE_HOLD,"a globally inferior AI stays defensive despite an undefended refinery");
        check(w.AI.Attack==0,"an unready army retries without losing a full attack interval");
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
    std::cout<<checks<<" tactical route, policy and actual controller scenarios passed.\n";
}
