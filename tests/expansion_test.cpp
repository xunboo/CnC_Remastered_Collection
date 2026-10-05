// Compile the actual economic controller and native MCV order hooks.
#include "../REDALERT/AIEXPANSION.H"
#include "../REDALERT/HARVESTAI.H"
#include "../REDALERT/AISTRATEGY.H"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <vector>
using std::max;
using std::min;
typedef int CELL;
typedef unsigned int COORDINATE;
typedef unsigned int TARGET;
typedef int HousesType;
typedef int UnitType;
typedef int StructType;
typedef int OverlayType;
typedef int MissionType;
typedef int RTTIType;
const int HOUSE_COUNT=20, HOUSE_NEUTRAL=18, HOUSE_JP=19, GAME_NORMAL=0;
const int MAP_CELL_W=128, MAP_CELL_H=128, MAP_CELL_TOTAL=16384, CELL_LEPTON_W=256;
const int TICKS_PER_SECOND=15, TICKS_PER_MINUTE=900, WAYPT_COUNT=8, REFRESH_EOL=32767;
const int DIR_S=128, FACING_NW=7, FACING_NONE=-1, MARK_UP=0, MARK_DOWN=1;
const int UNIT_HTANK=0, UNIT_MTANK=1, UNIT_MTANK2=2, UNIT_LTANK=3, UNIT_MCV=4, UNIT_HARVESTER=5, UNIT_COUNT=6;
const int STRUCT_CONST=0, STRUCT_REFINERY=1, STRUCT_WEAP=2, STRUCT_POWER=3, STRUCT_REPAIR=4, STRUCT_TURRET=5, STRUCT_COUNT=6, STRUCT_NONE=-1;
const int UNIT_NONE=-1, INFANTRY_NONE=-1, AIRCRAFT_NONE=-1, VESSEL_NONE=-1;
const int RTTI_UNIT=1, RTTI_BUILDING=2, RTTI_UNITTYPE=0, RTTI_INFANTRYTYPE=1, RTTI_AIRCRAFTTYPE=2, RTTI_VESSELTYPE=3, RTTI_BUILDINGTYPE=4;
const int MISSION_NONE=-1, MISSION_GUARD=0, MISSION_HUNT=1, MISSION_UNLOAD=2, MISSION_HARVEST=3;
const int MISSION_CONSTRUCTION=4, MISSION_DECONSTRUCTION=5, MISSION_MOVE=6, MISSION_ENTER=7, BSTATE_CONSTRUCTION=1;
const int OVERLAY_NONE=-1, OVERLAY_GOLD1=0, OVERLAY_GOLD2=1, OVERLAY_GOLD3=2, OVERLAY_GOLD4=3;
const int OVERLAY_GEMS1=4, OVERLAY_GEMS2=5, OVERLAY_GEMS3=6, OVERLAY_GEMS4=7, OVERLAY_WALL=8;
const int LAND_CLEAR=0, LAND_TIBERIUM=1;
const TARGET TARGET_NONE=0;
int Frame=0;
bool NewUnitsEnabled=true;
struct { int Type=1; } Session;
struct { int GoldValue=25, GemValue=50, AIAftermathfastbuild=0, AIProductionAggressiveness=1, AIBuildingsMultiplier=1; bool HarvesterOptimizeEnabled=true, IsAutoCrush=true; } Rule;
struct { int Waypoint[WAYPT_COUNT]={-1,-1,-1,-1,-1,-1,-1,-1}; } Scen;
struct { int Normal_Delay() const { return 1; } } MissionControl[8];
int Random_Pick(int low,int) { return low; }
int Cell(int x,int y) { return y*128+x; }
COORDINATE Cell_Coord(CELL c) { return ((unsigned int)(c/128*256+128)<<16)|(unsigned int)(c%128*256+128); }
CELL Coord_Cell(COORDINATE c) { return (int)((c&65535)/256+(c>>16)/256*128); }
CELL Adjacent_Cell(CELL c,int) { return c-129; }
COORDINATE Adjacent_Cell(COORDINATE c,int) { return Cell_Coord(Coord_Cell(c)+128); }
TARGET As_Target(CELL c) { return 0x80000000u|(unsigned int)c; }
CELL As_Cell(TARGET t) { return (int)(t&65535); }
bool Target_Legal(TARGET t) { return t!=0; }

struct HouseClass;
struct BuildingClass;
struct BulletTypeClass { bool IsAntiGround=true; } GroundBullet, AirBullet;
struct WeaponTypeClass { BulletTypeClass const * Bullet=&GroundBullet; int Attack=40, Range=4*256; } Cannon, AntiAir;
struct TechnoTypeClass {
    int Type=0, Cost=800, Speed=0, MZone=0;
    bool Allowed=true, IsToHarvest=false;
    WeaponTypeClass const * PrimaryWeapon=nullptr, * SecondaryWeapon=nullptr;
    int Cost_Of() const { return Cost; }
    int What_Am_I() const { return RTTI_UNITTYPE; }
    int Factory_Bonus(int,HouseClass const *) const;
};
struct UnitTypeClass : TechnoTypeClass {
    static UnitTypeClass Types[UNIT_COUNT];
    static UnitTypeClass const & As_Reference(int type) { return Types[type]; }
};
UnitTypeClass UnitTypeClass::Types[UNIT_COUNT];
struct InfantryTypeClass { static UnitTypeClass const & As_Reference(int t) { return UnitTypeClass::Types[t]; } };
struct AircraftTypeClass : InfantryTypeClass {};
struct VesselTypeClass : InfantryTypeClass {};
struct FactoryClass {
    int Balance=0; void * Object=nullptr;
#include "expansion_remaining_cost.inc"
    void * Get_Object() const { return Object; }
};
struct BuildingTypeClass : TechnoTypeClass {
    static BuildingTypeClass Types[STRUCT_COUNT];
    short const * ExitList=nullptr;
    static BuildingTypeClass const & As_Reference(int type) { return Types[type]; }
    short const * Occupy_List(bool=false) const;
    bool Legal_Placement(CELL) const;
};
BuildingTypeClass BuildingTypeClass::Types[STRUCT_COUNT];
struct TechnoClass {
    HouseClass * House=nullptr;
    bool IsActive=true, IsInLimbo=false, IsUnderAttack=false;
    int Strength=500, Kind=RTTI_UNIT, Mission=MISSION_GUARD;
    COORDINATE Coord=0;
    TARGET Handle=0;
    virtual ~TechnoClass() {}
    virtual COORDINATE Center_Coord() const { return Coord; }
    virtual TechnoTypeClass const * Techno_Type_Class() const =0;
    int What_Am_I() const { return Kind; }
    TARGET As_Target() const { return Handle; }
};
struct HouseClass {
    struct ClassFixture { int House=0; } OwnClass;
    ClassFixture * Class=&OwnClass;
    bool IsHuman=false, IsActive=true, IsDefeated=false, IsBaseBuilding=true, IsTiberiumShort=false, Allied=false;
    bool AllowExtraBaseAtWaypoint=false;
    int Power=400, Drain=100, ActLike=0, Money=20000;
    int AIMaxConYardsAndMCVs=4, AIMaxBuildings=100, AIRefineryLimit=8, AIWarFactoryLimit=4, AIMaxTanksNr=100;
    int HumanBuildingCount=10;
    int CurBuildings=0, CurUnits=10, CurInfantry=0, CurAircraft=0, AIPersonalStrategyMode=1, BuildUnit=UNIT_NONE;
    int BuildInfantry=INFANTRY_NONE, BuildAircraft=AIRCRAFT_NONE, BuildVessel=VESSEL_NONE, BuildStructure=STRUCT_NONE;
    FactoryClass * Factories[5]={nullptr};
    FactoryClass * Fetch_Factory(int type) const { return Factories[type]; }
    int BQuantity[STRUCT_COUNT]={0}, UQuantity[UNIT_COUNT]={0}, AI_Interesting_Waypoints[WAYPT_COUNT]={0};
    AIStrategy::EnemyForces Forces;
    bool Is_Ally(HouseClass const * other) const { return other==this || other->Allied; }
    bool Is_Ally(TechnoClass const * other) const { return Is_Ally(other->House); }
    bool Can_Build(TechnoTypeClass const * type,int) const { return type->Allowed; }
    int Available_Money() const { return Money; }
    int Get_Quantity(int type) { return BQuantity[type]; }
    int QuantityB(int type) { return BQuantity[type]; }
    int QuantityU(int type) { return UQuantity[type]; }
    int Factory_Count(int) const { return BQuantity[STRUCT_WEAP]; }
    AIStrategy::EnemyForces AI_Enemy_Forces() const { return Forces; }
    int Native_Economic_Priority() {
#include "expansion_building_priority.inc"
        return TICKS_PER_SECOND;
    }
    bool AI_Economic_MCV_Ready();
    int AI_Economic_Reserve() const;
    int AI_Economic_Cash() const;
    int AI_Economic_Combat_Budget() const;
    StructType AI_Economic_Building();
    COORDINATE AI_Economic_Location(BuildingClass const *) const;
};
struct DriverFixture { int Mission_Guard() { return 3; } };
struct TeamFixture {
    bool Assigned=false;
    operator bool() const { return Assigned; }
    bool Is_Valid() const { return Assigned; }
    TeamFixture & operator=(bool value) { Assigned=value; return *this; }
};
class UnitClass : public TechnoClass, public DriverFixture {
public:
    UnitTypeClass const * Class=nullptr;
    TeamFixture Team;
    bool IsTethered=false, IsDriving=false, IsDeploying=false;
    int MissionQueue=MISSION_NONE, ID=0, Tiberium=0;
    TARGET NavCom=0;
    int Marks=0, Scatters=0;
    operator int() const { return Class->Type; }
    TechnoTypeClass const * Techno_Type_Class() const override { return Class; }
    void Assign_Destination(TARGET target) { NavCom=target; }
    void Assign_Mission(int mission) { MissionQueue=mission; }
    void Mark(int mark) { Marks+=mark==MARK_UP?1:-1; }
    void Scatter(int) { ++Scatters; }
    bool Harvest_Retreat(TechnoClass const *) { return false; } // MCVs do not harvest.
    bool Native_Crush_Guard(TechnoClass const * source) {
        int previous_strength=Strength+1;
#include "expansion_damage_guard.inc"
    }
    bool MCV_Goto_Resource();
    bool Goto_Clear_Spot();
    int Mission_Guard();
    bool Native_Deploy_Check() {
#include "expansion_deploy_check.inc"
        bool legal=BuildingTypeClass::As_Reference(STRUCT_CONST).Legal_Placement(cell);
        Mark(MARK_DOWN);
        return legal;
    }
};
typedef DriverFixture DriveClass;
struct BuildingClass : TechnoClass {
    BuildingTypeClass const * Class=nullptr;
    int BState=0;
    operator int() const { return Class->Type; }
    TechnoTypeClass const * Techno_Type_Class() const override { return Class; }
    COORDINATE Center_Coord() const override { return Cell_Coord(Coord_Cell(Coord)+129); }
    COORDINATE Exit_Coord() const { return Cell_Coord(Coord_Cell(Coord)+257); }
};
template<class T> struct Heap {
    std::vector<T *> Data;
    int Count() const { return (int)Data.size(); }
    T * Ptr(int i) const { return Data[i]; }
    int ID(T const * object) const { return (int)(std::find(Data.begin(),Data.end(),object)-Data.begin()); }
};
Heap<UnitClass> Units;
Heap<BuildingClass> Buildings;
Heap<TechnoClass> Infantry, Aircraft, Vessels;
struct OverlayTypeClass {
    bool IsWall=false;
    static OverlayTypeClass const & As_Reference(int overlay) { static OverlayTypeClass wall{true},ore{false}; return overlay==OVERLAY_WALL?wall:ore; }
};
struct FakeCell {
    bool Clear=true, Buildable=true;
    int Overlay=OVERLAY_NONE, OverlayData=0;
    BuildingClass * Building=nullptr;
    int Land_Type() const { return Overlay>=0 && Overlay<8 ? LAND_TIBERIUM : LAND_CLEAR; }
    BuildingClass * Cell_Building() const { return Building; }
    bool Is_Clear_To_Move(int,bool,bool,int,int) const { return Clear && !Building && Overlay!=OVERLAY_WALL; }
};
struct MapFixture {
    FakeCell Cells[MAP_CELL_TOTAL];
    int MapCellX=0, MapCellY=0, MapCellWidth=128, MapCellHeight=128;
    bool In_Radar(CELL c) const { return c>=0 && c<MAP_CELL_TOTAL && c%128>=MapCellX && c%128<MapCellX+MapCellWidth && c/128>=MapCellY && c/128<MapCellY+MapCellHeight; }
    FakeCell & operator[](CELL c) { assert(c>=0 && c<16384); return Cells[c]; }
    bool Passes_Proximity_Check(BuildingTypeClass const *,int,short const *,CELL) const;
} Map;
short const * BuildingTypeClass::Occupy_List(bool placement) const {
    static short yard[]={0,1,2,128,129,130,256,257,258,REFRESH_EOL};
    static short yard_bib[]={0,1,2,128,129,130,256,257,258,384,385,386,REFRESH_EOL};
    static short proc[]={1,128,129,130,256,REFRESH_EOL};
    static short proc_bib[]={1,128,129,130,256,257,258,384,385,386,REFRESH_EOL};
    static short weap[]={0,1,2,128,129,130,REFRESH_EOL};
    static short weap_bib[]={0,1,2,128,129,130,256,257,258,REFRESH_EOL};
    static short power[]={0,1,128,129,REFRESH_EOL};
    static short power_bib[]={0,1,128,129,256,257,REFRESH_EOL};
    if(Type==STRUCT_CONST) return placement?yard_bib:yard;
    if(Type==STRUCT_REFINERY) return placement?proc_bib:proc;
    if(Type==STRUCT_WEAP) return placement?weap_bib:weap;
    return placement?power_bib:power;
}
bool BuildingTypeClass::Legal_Placement(CELL cell) const {
    for(short const * ptr=Occupy_List(true);*ptr!=REFRESH_EOL;++ptr) {
        int next=cell+*ptr;
        if(!Map.In_Radar(next) || !Map[next].Buildable || !Map[next].Clear || Map[next].Building || Map[next].Overlay==OVERLAY_WALL) return false;
    }
    return true;
}
bool MapFixture::Passes_Proximity_Check(BuildingTypeClass const *,int house,short const * list,CELL cell) const {
    for(int i=0;i<Buildings.Count();++i) {
        BuildingClass const * b=Buildings.Ptr(i);
        if(!b->IsActive || b->IsInLimbo || b->Strength<=0 || b->House->Class->House!=house || b->Mission==MISSION_DECONSTRUCTION) continue;
        for(short const * a=list;*a!=REFRESH_EOL;++a) {
            for(short const * p=b->Class->Occupy_List(true);*p!=REFRESH_EOL;++p) {
                int v=cell+*a,w=Coord_Cell(b->Coord)+*p;
                if(max(abs(v%128-w%128),abs(v/128-w/128))<=1) return true;
            }
        }
    }
    return false;
}
#include "expansion_controller.inc"
#include "expansion_unit_methods.inc"
int TechnoTypeClass::Factory_Bonus(int time,HouseClass const * hptr) const {
#define FIXIT_CSII 1
#pragma warning(push)
#pragma warning(disable:4244)
#include "expansion_factory_bonus.inc"
#pragma warning(pop)
    return time;
}
static int checks=0;
void check(bool condition,char const * text) {
    ++checks;
    if(!condition) { std::cerr<<"FAIL: "<<text<<'\n'; std::exit(1); }
}
struct World {
    HouseClass AI,Enemy,Ally;
    std::vector<std::unique_ptr<TechnoClass>> Owned;
    World() {
        Units.Data.clear(); Buildings.Data.clear(); Infantry.Data.clear(); Vessels.Data.clear(); Aircraft.Data.clear();
        AIExpansion::Reset(); Frame=0; Session.Type=1; NewUnitsEnabled=true; Rule.AIAftermathfastbuild=0; Rule.AIProductionAggressiveness=1;
        Map.MapCellX=Map.MapCellY=0; Map.MapCellWidth=Map.MapCellHeight=128;
        for(auto & cell:Map.Cells) cell=FakeCell();
        for(int i=0;i<UNIT_COUNT;++i) { UnitTypeClass::Types[i]=UnitTypeClass(); UnitTypeClass::Types[i].Type=i; }
        UnitTypeClass::Types[UNIT_MCV].Cost=2500; UnitTypeClass::Types[UNIT_HARVESTER].Cost=1400;
        UnitTypeClass::Types[UNIT_HARVESTER].IsToHarvest=true;
        for(int i=0;i<STRUCT_COUNT;++i) { BuildingTypeClass::Types[i]=BuildingTypeClass(); BuildingTypeClass::Types[i].Type=i; }
        BuildingTypeClass::Types[STRUCT_REFINERY].Cost=2000; BuildingTypeClass::Types[STRUCT_WEAP].Cost=2000;
        BuildingTypeClass::Types[STRUCT_POWER].Cost=300;
        static short weap_exit[]={257,REFRESH_EOL}; BuildingTypeClass::Types[STRUCT_WEAP].ExitList=weap_exit;
        Enemy.OwnClass.House=1; Ally.OwnClass.House=2; Ally.Allied=true; AirBullet.IsAntiGround=false; AntiAir.Bullet=&AirBullet;
    }
    BuildingClass * Build(int type,int x,int y,HouseClass * owner=nullptr) {
        auto b=std::unique_ptr<BuildingClass>(new BuildingClass); b->House=owner?owner:&AI; b->Class=&BuildingTypeClass::Types[type];
        b->Kind=RTTI_BUILDING; b->Coord=Cell_Coord(Cell(x,y)); b->Handle=2000+Buildings.Count();
        auto ptr=b.get(); Buildings.Data.push_back(ptr); ++ptr->House->BQuantity[type]; ++ptr->House->CurBuildings;
        for(auto p=ptr->Class->Occupy_List();*p!=REFRESH_EOL;++p) Map[Cell(x,y)+*p].Building=ptr;
        Owned.push_back(std::move(b)); return ptr;
    }
    UnitClass * Unit(int type,int x,int y,HouseClass * owner=nullptr) {
        auto u=std::unique_ptr<UnitClass>(new UnitClass); u->House=owner?owner:&AI; u->Class=&UnitTypeClass::Types[type];
        u->Coord=Cell_Coord(Cell(x,y)); u->Handle=1000+Units.Count(); u->ID=Units.Count();
        u->Mission=type==UNIT_HARVESTER?MISSION_HARVEST:MISSION_HUNT;
        auto ptr=u.get(); Units.Data.push_back(ptr); ++ptr->House->UQuantity[type]; Owned.push_back(std::move(u)); return ptr;
    }
    void Ore(int x,int y,int radius=3,bool gems=false) {
        for(int dy=-radius;dy<=radius;++dy) for(int dx=-radius;dx<=radius;++dx) {
            auto & c=Map[Cell(x+dx,y+dy)]; c.Overlay=gems?OVERLAY_GEMS1:OVERLAY_GOLD1; c.OverlayData=gems?3:12;
        }
    }
    void Base(bool ore=true) {
        Build(STRUCT_CONST,12,12); Build(STRUCT_WEAP,17,12); Build(STRUCT_REFINERY,12,18);
        Unit(UNIT_HARVESTER,15,21); Unit(UNIT_HARVESTER,17,21);
        if(ore) Ore(20,25);
    }
};
int main() {
    {
        World w; w.Base(); w.Ore(50,28);
        check(w.AI.AI_Economic_MCV_Ready(),"funded economy plans an MCV for a reachable distant field");
        check(!w.AI.AI_Economic_MCV_Ready(),"new MCV orders observe the expansion cooldown");
        Frame=2*TICKS_PER_MINUTE;
        check(w.AI.AI_Economic_MCV_Ready(),"a completed cooldown permits later economic expansion");
        auto mcv=w.Unit(UNIT_MCV,21,15);
        check(!mcv->Goto_Clear_Spot() && Target_Legal(mcv->NavCom),"native hunt selects a distant ore deployment goal");
        int goal=Deployments[mcv->As_Target()].Goal;
        check(Separation(goal,Cell(50,28))<=10 && Separation(goal,Cell(13,13))>=12,"MCV goal favors the remote ore rather than another yard in the core");
        check(mcv->Marks==0 && mcv->Scatters==0,"new MCV planning restores map occupancy and does not randomly scatter");
        mcv->Coord=Cell_Coord(goal); mcv->NavCom=0;
        check(mcv->Goto_Clear_Spot(),"arrival at a safe deployment goal allows native deployment");
        check(mcv->Native_Deploy_Check() && mcv->Marks==0,"native deployment safety hook accepts the checked arrival");
        w.Build(STRUCT_CONST,goal%128-1,goal/128-1); --w.AI.UQuantity[UNIT_MCV]; mcv->IsInLimbo=true;
        check(w.AI.AI_Economic_Building()==STRUCT_REFINERY,"new ore-side yard prioritizes a refinery before extra production");
        BuildingClass preview; preview.Class=&BuildingTypeClass::Types[STRUCT_REFINERY];
        int plot=Coord_Cell(w.AI.AI_Economic_Location(&preview));
        check(Separation(New_Entrance(plot),Cell(50,28))<=8,"new refinery door lies within a short mining route");
        check(Map.Passes_Proximity_Check(preview.Class,0,preview.Class->Occupy_List(true),plot),"expansion refinery respects normal construction proximity");
    }
    {
        World w; w.Base(); w.Ore(50,28); w.AI.Money=6399;
        check(!w.AI.AI_Economic_MCV_Ready(),"MCV budget reserves its refinery, power and fighting units");
        w.AI.Money=6400;
        check(w.AI.AI_Economic_MCV_Ready(),"exact funded expansion budget can be used");
        w.AI.UQuantity[UNIT_MCV]=1;
        check(!w.AI.AI_Economic_MCV_Ready(),"only one expansion MCV is pending at a time");
    }
    {
        World w; w.Base(); w.Ore(50,28); w.AI.AIMaxConYardsAndMCVs=1;
        check(!w.AI.AI_Economic_MCV_Ready(),"configured construction-yard cap is respected");
        w.AI.AIMaxConYardsAndMCVs=4; w.AI.CurBuildings=98;
        check(!w.AI.AI_Economic_MCV_Ready(),"expansion leaves space under the optional building cap");
        w.AI.CurBuildings=3; w.AI.Power=90;
        check(!w.AI.AI_Economic_MCV_Ready(),"low power defers optional MCV spending");
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"low power defers optional factory and refinery spending");
        w.AI.Power=400; w.AI.UQuantity[UNIT_HARVESTER]=1;
        check(!w.AI.AI_Economic_MCV_Ready(),"one truck does not support optional expansion");
        w.AI.UQuantity[UNIT_HARVESTER]=2; w.AI.IsTiberiumShort=true;
        check(!w.AI.AI_Economic_MCV_Ready(),"economic shortage suspends optional expansion");
        w.AI.IsTiberiumShort=false; w.AI.Forces.Armor=30;
        check(!w.AI.AI_Economic_MCV_Ready(),"an outmatched army preserves its economy before expanding");
    }
    {
        World w; w.Base();
        check(!w.AI.AI_Economic_MCV_Ready(),"a local field already served by a refinery does not require an MCV");
        w.AI.Money=3899;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"a second refinery keeps a military and power reserve");
        w.AI.Money=3900;
        check(w.AI.AI_Economic_Building()==STRUCT_REFINERY,"a stable developed single-refinery base can add a funded second bay");
    }
    {
        World w; w.Base(false); w.Ore(50,28);
        for(int y=0;y<128;++y) Map[Cell(35,y)].Clear=false;
        check(!w.AI.AI_Economic_MCV_Ready(),"water or terrain dividing the map makes remote expansion unreachable");
        auto mcv=w.Unit(UNIT_MCV,21,15);
        check(!mcv->Goto_Clear_Spot() && !Target_Legal(mcv->NavCom),"an MCV is held when every resource plot is unreachable");
        Map[Cell(35,28)].Clear=true; Frame+=5*TICKS_PER_SECOND;
        check(!mcv->Goto_Clear_Spot() && Target_Legal(mcv->NavCom),"opening a real crossing makes the distant plot reachable");
    }
    {
        World w; w.Base(false); w.Ore(50,28);
        for(int y=0;y<128;++y) Map[Cell(35,y)].Overlay=OVERLAY_WALL;
        check(!w.AI.AI_Economic_MCV_Ready(),"player-built walls block resource expansion routes");
        Map[Cell(35,28)].Overlay=OVERLAY_NONE; Frame=10*TICKS_PER_SECOND;
        check(w.AI.AI_Economic_MCV_Ready(),"an opening in the wall permits resource expansion");
    }
    {
        World w; w.Base(false); w.Ore(50,28); w.Ore(50,65);
        UnitTypeClass::Types[UNIT_HTANK].PrimaryWeapon=&Cannon;
        w.Unit(UNIT_HTANK,50,28,&w.Enemy);
        auto mcv=w.Unit(UNIT_MCV,21,15); mcv->Goto_Clear_Spot();
        check(Target_Legal(mcv->NavCom) && Separation(Deployments[mcv->As_Target()].Goal,Cell(50,65))<=10,"enemy armor diverts the MCV to a safer field");
        int old_goal=Deployments[mcv->As_Target()].Goal;
        w.Unit(UNIT_HTANK,old_goal%128,old_goal/128,&w.Enemy); Frame+=5*TICKS_PER_SECOND;
        check(!mcv->Goto_Clear_Spot() && (!Target_Legal(mcv->NavCom) || Deployments[mcv->As_Target()].Goal!=old_goal),"new enemy contact cancels the old unsafe deployment route");
        mcv->Coord=Cell_Coord(old_goal); mcv->IsDeploying=true;
        check(!mcv->Native_Deploy_Check() && !mcv->IsDeploying && mcv->Marks==0,"native turning completion cannot deploy into a newly threatened field");
    }
    {
        World w; w.Base(false); w.Ore(50,28);
        BuildingTypeClass::Types[STRUCT_TURRET].PrimaryWeapon=&Cannon;
        w.Build(STRUCT_TURRET,49,27,&w.Enemy);
        check(!w.AI.AI_Economic_MCV_Ready(),"enemy ground defenses make a mineral site unsafe");
    }
    {
        World w; w.Base(false); w.Ore(50,28);
        UnitTypeClass::Types[UNIT_HTANK].PrimaryWeapon=&AntiAir;
        UnitTypeClass::Types[UNIT_HTANK].SecondaryWeapon=&Cannon;
        w.Unit(UNIT_HTANK,50,28,&w.Enemy);
        check(!w.AI.AI_Economic_MCV_Ready(),"secondary ground weapons also protect an enemy-controlled field");
    }
    {
        World w; w.Base(false); w.Ore(50,28);
        UnitTypeClass::Types[UNIT_HTANK].PrimaryWeapon=&Cannon;
        w.Unit(UNIT_HTANK,50,28,&w.Ally);
        check(w.AI.AI_Economic_MCV_Ready(),"allied units do not make friendly mining land unsafe");
    }
    {
        World w; w.Base(false); Map[Cell(50,28)].Overlay=OVERLAY_GOLD1; Map[Cell(50,28)].OverlayData=12;
        check(!w.AI.AI_Economic_MCV_Ready(),"a tiny leftover mineral deposit does not justify a complete expansion");
        w.Ore(50,28);
        for(int y=16;y<=40;++y) for(int x=38;x<=62;++x) if(Map[Cell(x,y)].Overlay==OVERLAY_NONE) Map[Cell(x,y)].Buildable=false;
        Frame=10*TICKS_PER_SECOND;
        check(!w.AI.AI_Economic_MCV_Ready(),"a field with no deployable yard and followup plots is rejected");
    }
    {
        World w; w.Base(false); w.Ore(36,25); w.Build(STRUCT_POWER,28,22);
        check(w.AI.AI_Economic_Building()==STRUCT_REFINERY,"long haul with legal nearby construction chooses a refinery extension");
        check(!w.AI.AI_Economic_MCV_Ready(),"a legal ore-side refinery extension avoids paying for an MCV");
        BuildingClass preview; preview.Class=&BuildingTypeClass::Types[STRUCT_REFINERY];
        int plot=Coord_Cell(w.AI.AI_Economic_Location(&preview));
        check(Separation(New_Entrance(plot),Cell(36,25))<=8,"extended refinery unload door is close to the distant resource field");
        w.AI.AIRefineryLimit=1;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"ore-side refinery production respects its configured cap");
    }
    {
        World w; w.Base(); w.Build(STRUCT_REFINERY,21,18);
        check(w.AI.AI_Economic_Building()==STRUCT_WEAP,"two funded harvesting/refinery streams can support a second tank factory");
        w.AI.Money=4699;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"extra tank factory cannot consume the continuing tank-production budget");
        w.AI.Money=4700;
        check(w.AI.AI_Economic_Building()==STRUCT_WEAP,"exact price plus military reserve supports an extra factory");
        w.AI.UQuantity[UNIT_MCV]=1;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"travelling MCV retains its first-refinery budget ahead of factories");
        w.AI.UQuantity[UNIT_MCV]=0; w.AI.Money=20000; w.AI.AIWarFactoryLimit=1;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"configured war-factory limit is respected");
        w.AI.AIWarFactoryLimit=4; w.AI.AIMaxTanksNr=13;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"a nearly full fighting force does not need faster optional tank production");
        w.AI.AIMaxTanksNr=100; w.AI.AIPersonalStrategyMode=4;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"air-focused economy does not add tank factories without ground pressure");
        w.AI.Forces.Armor=15;
        check(w.AI.AI_Economic_Building()==STRUCT_WEAP,"massed enemy tanks can justify increased heavy-unit capacity");
        BuildingClass preview; preview.Class=&BuildingTypeClass::Types[STRUCT_WEAP];
        int plot=Coord_Cell(w.AI.AI_Economic_Location(&preview));
        check(Map.Passes_Proximity_Check(preview.Class,0,preview.Class->Occupy_List(true),plot),"new tank factory respects construction proximity");
        check(Map[New_Entrance(plot)].Is_Clear_To_Move(0,true,true,-1,0),"new factory leaves a passable southern vehicle exit");
    }
    {
        World w; w.Base(); w.Build(STRUCT_REFINERY,21,18); w.Build(STRUCT_REFINERY,25,18);
        w.Unit(UNIT_HARVESTER,27,21); w.Build(STRUCT_WEAP,17,8);
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"Aftermath's native two-factory speed cap prevents useless capacity spending");
        Rule.AIAftermathfastbuild=1;
        check(w.AI.AI_Economic_Building()==STRUCT_WEAP,"uncapped native production bonus permits a funded third factory");
        TechnoTypeClass tank;
        check(tank.Factory_Bonus(1200,&w.AI)==600,"actual native production bonus halves time with two factories");
        w.AI.BQuantity[STRUCT_WEAP]=4; Rule.AIAftermathfastbuild=0;
        check(tank.Factory_Bonus(1200,&w.AI)==600,"native Aftermath bonus remains capped at two factories");
        Rule.AIAftermathfastbuild=1;
        check(tank.Factory_Bonus(1200,&w.AI)==300,"native uncapped production time scales with four factories");
        w.AI.BQuantity[STRUCT_WEAP]=0;
        check(tank.Factory_Bonus(1200,&w.AI)==1200,"native factory bonus never divides by zero");
    }
    {
        World w; auto mcv=w.Unit(UNIT_MCV,40,40);
        check(mcv->Goto_Clear_Spot(),"the first MCV can establish a yard on a sparse resource map");
        mcv->Mission=MISSION_GUARD; mcv->Mission_Guard();
        check(mcv->MissionQueue==MISSION_HUNT,"native guard wakes a skirmish MCV into the economic deployment planner");
        mcv->Team=true;
        check(!AIExpansion::Controls(mcv),"scripted MCV teams retain control");
        mcv->Team=false; mcv->MissionQueue=MISSION_MOVE;
        check(!AIExpansion::Controls(mcv),"explicit queued movement is preserved");
        mcv->MissionQueue=MISSION_NONE; w.AI.IsHuman=true;
        check(!AIExpansion::Controls(mcv) && !w.AI.AI_Economic_MCV_Ready(),"human houses keep manual MCV and economic decisions");
        w.AI.IsHuman=false; Session.Type=GAME_NORMAL;
        check(!AIExpansion::Controls(mcv) && w.AI.AI_Economic_Building()==STRUCT_NONE,"campaign MCV scripts and base choices keep their native path");
    }
    {
        World w; w.Base(false); w.Ore(50,28); auto mcv=w.Unit(UNIT_MCV,21,15);
        mcv->Goto_Clear_Spot(); int goal=Deployments[mcv->As_Target()].Goal;
        Frame=20*TICKS_PER_SECOND; mcv->Goto_Clear_Spot();
        check(Target_Legal(mcv->NavCom) && Separation(Deployments[mcv->As_Target()].Goal,goal)>3,"stalled MCV changes its deployment plot and temporarily avoids the failed one");
        AIExpansion::Forget(mcv);
        check(Deployments.find(mcv->As_Target())==Deployments.end(),"unit-slot reuse clears deployment timing and failed plots");
        mcv->Goto_Clear_Spot(); Frame=0; mcv->Goto_Clear_Spot();
        check(Deployments[mcv->As_Target()].LastFrame==0,"rewound simulation time rebuilds transient deployment state");
        AIExpansion::Reset();
        check(Deployments.empty() && Economies[0].Owner==nullptr,"scenario initialization clears all transient expansion state");
    }
    {
        World w; w.Base(false); w.Ore(50,28); w.AI.Money=6400;
        FactoryClass building; building.Balance=1000; building.Object=&building;
        w.AI.Factories[RTTI_BUILDINGTYPE]=&building;
        check(!w.AI.AI_Economic_MCV_Ready(),"unpaid building construction bills are reserved before optional MCV orders");
        building.Balance=0;
        check(w.AI.AI_Economic_MCV_Ready(),"completed paid construction releases funds for an economic expansion");
    }
    {
        World w; w.Base(); w.Build(STRUCT_REFINERY,21,18); w.AI.Money=4700;
        FactoryClass tank; tank.Balance=800; tank.Object=&tank; w.AI.Factories[RTTI_UNITTYPE]=&tank;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"an existing tank's unpaid balance takes priority over another factory");
        tank.Balance=0;
        check(w.AI.AI_Economic_Building()==STRUCT_WEAP,"a fully funded second factory becomes available after the tank is paid");
    }
    {
        World w; w.Base(); w.AI.Money=10000;
        w.AI.BuildUnit=UNIT_MTANK; w.AI.BuildInfantry=UNIT_LTANK; w.AI.BuildAircraft=UNIT_MTANK2;
        w.AI.BuildVessel=UNIT_LTANK; w.AI.BuildStructure=STRUCT_WEAP;
        check(w.AI.AI_Economic_Cash()==4800,"same-tick requests across all five production categories reserve their prices");
        FactoryClass running[5];
        for(int i=0;i<5;++i) { running[i].Balance=100; running[i].Object=&running[i]; w.AI.Factories[i]=&running[i]; }
        check(w.AI.AI_Economic_Cash()==9500,"running production uses unpaid balances without charging the original request twice");
        w.AI.Money=100;
        check(w.AI.AI_Economic_Cash()==0,"overcommitted production yields a zero optional budget instead of borrowing future income");
        w.AI.IsHuman=true;
        check(w.AI.AI_Economic_Cash()==100,"human production retains its existing spending control");
        w.AI.IsHuman=false; Session.Type=GAME_NORMAL;
        check(w.AI.AI_Economic_Cash()==100,"campaign production retains its existing spending control");
    }
    {
        World w; w.Base(); w.AI.BuildUnit=UNIT_MCV;
        check(w.AI.AI_Economic_Combat_Budget()==15200,"queued MCV reserves both its purchase and its refinery/power followup");
        check(w.AI.AI_Economic_Reserve()==3900,"optional base spending also retains the travelling MCV's first-refinery reserve");
        w.AI.BuildUnit=UNIT_NONE; w.AI.UQuantity[UNIT_MCV]=1; w.AI.Money=2299;
        check(w.AI.AI_Economic_Combat_Budget()==0,"all combat categories keep the expansion refinery and power budget intact");
        w.AI.Money=3100;
        check(w.AI.AI_Economic_Combat_Budget()==800,"surplus funds still support a continuing tank order during expansion");
    }
    {
        World w; w.Base(); w.Ore(50,28);
        UnitTypeClass::Types[UNIT_MCV].Allowed=false;
        check(!w.AI.AI_Economic_MCV_Ready(),"unavailable MCV technology blocks economic expansion");
        UnitTypeClass::Types[UNIT_MCV].Allowed=true;
        Units.Data[0]->Mission=MISSION_GUARD; Units.Data[1]->Mission=MISSION_GUARD;
        check(!w.AI.AI_Economic_MCV_Ready(),"idle trucks do not count as a running mining economy");
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"idle trucks also defer optional refinery and factory investment");
        Units.Data[0]->Mission=Units.Data[1]->Mission=MISSION_HARVEST; w.AI.CurUnits=7;
        check(!w.AI.AI_Economic_MCV_Ready(),"optional expansion retains a minimum defending fighting force");
    }
    {
        World w; w.Base(false); w.Ore(50,28); Map[Cell(18,14)].Clear=false;
        check(!w.AI.AI_Economic_MCV_Ready(),"a blocked real factory exit cannot produce a reachable MCV expansion route");
        Map[Cell(18,14)].Clear=true; Frame=10*TICKS_PER_SECOND;
        check(w.AI.AI_Economic_MCV_Ready(),"opening the actual southern factory exit permits expansion planning");
    }
    {
        World w; w.Base(false); w.Ore(70,70); w.Build(STRUCT_CONST,42,68);
        for(int y=0;y<128;++y) Map[Cell(35,y)].Clear=false;
        check(!w.AI.AI_Economic_MCV_Ready(),"a yard on another landmass cannot make the producing factory's MCV route reachable");
    }
    {
        World w; w.Base(); auto mcv=w.Unit(UNIT_MCV,21,15); auto enemy=w.Unit(UNIT_HTANK,25,15,&w.Enemy);
        check(!mcv->Native_Crush_Guard(enemy),"an economic MCV keeps its deployment/defense orders rather than chasing an attacker to crush");
        w.AI.IsHuman=true;
        check(mcv->Native_Crush_Guard(enemy),"human MCV retaliation retains the native auto-crush rule");
        w.AI.IsHuman=false; Session.Type=GAME_NORMAL;
        check(mcv->Native_Crush_Guard(enemy),"campaign MCV retaliation retains its native rule");
    }
    {
        World w; w.Base(); w.Build(STRUCT_REFINERY,21,18);
        for(int type=UNIT_HTANK;type<=UNIT_LTANK;++type) UnitTypeClass::Types[type].Allowed=false;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"extra factories require an available main-tank production line");
    }
    {
        World w; w.Base(); w.Ore(50,28); w.AI.CurUnits=2; w.AI.CurInfantry=24; w.AI.Forces.Infantry=20;
        check(w.AI.AI_Economic_MCV_Ready(),"a sufficient infantry garrison can support safe economic expansion without six tanks");
    }
    {
        World w; w.Base(); w.Ore(50,28); w.AI.CurUnits=6; w.AI.CurAircraft=4;
        check(w.AI.AI_Economic_MCV_Ready(),"a mixed ground and air defense can support safe economic expansion");
    }
    {
        World w; w.Base(false); w.Ore(50,28); UnitTypeClass::Types[UNIT_HTANK].PrimaryWeapon=&Cannon;
        w.Unit(UNIT_HTANK,35,20,&w.Enemy); auto mcv=w.Unit(UNIT_MCV,21,15);
        mcv->Goto_Clear_Spot(); int goal=Deployments[mcv->As_Target()].Goal;
        check(goal>=0 && As_Cell(mcv->NavCom)!=goal,"safe MCV movement uses short route checkpoints instead of sending the final goal to native pathfinding");
        bool safe=true, reached=false; int steps=0;
        for(;steps<60;++steps) {
            if(mcv->Goto_Clear_Spot()) { reached=true; break; }
            if(!Target_Legal(mcv->NavCom)) { safe=false; break; }
            int source=Coord_Cell(mcv->Coord), next=As_Cell(mcv->NavCom);
            TravelMap field(128,128); std::vector<unsigned char> unsafe; Terrain(field,unsafe,&w.AI);
            if(Separation(source,next)>4) safe=false;
            for(int y=min(source/128,next/128);y<=max(source/128,next/128);++y)
                for(int x=min(source%128,next%128);x<=max(source%128,next%128);++x) if(!field.Clear[Cell(x,y)]) safe=false;
            mcv->Coord=Cell_Coord(next); mcv->NavCom=0; Frame+=TICKS_PER_SECOND;
        }
        check(safe && reached && steps>1,"native movement checkpoints reach the ore site without cutting across an enemy firing area");
    }
    {
        World w; w.Base(false); w.Ore(50,28); UnitTypeClass::Types[UNIT_HTANK].PrimaryWeapon=&Cannon;
        auto mcv=w.Unit(UNIT_MCV,50,28); w.Unit(UNIT_HTANK,50,28,&w.Enemy);
        check(!mcv->Goto_Clear_Spot() && Target_Legal(mcv->NavCom) && Deployments[mcv->As_Target()].Retreat,"MCV in a newly threatened field can leave the danger area toward a safe home location");
        check(!mcv->Native_Deploy_Check() && !mcv->IsDeploying,"emergency retreat checkpoints cannot trigger a construction-yard deployment");
    }
    {
        World w; w.Build(STRUCT_CONST,12,12); w.Ore(50,28); auto mcv=w.Unit(UNIT_MCV,21,15);
        check(!mcv->Goto_Clear_Spot() && !Target_Legal(mcv->NavCom),"an extra starting MCV waits until its main base has a working economy");
        w.Build(STRUCT_REFINERY,12,18); w.Unit(UNIT_HARVESTER,15,21); w.Unit(UNIT_HARVESTER,17,21);
        Frame=5*TICKS_PER_SECOND;
        check(!mcv->Goto_Clear_Spot() && Target_Legal(mcv->NavCom),"a held starting MCV can expand after the main mining economy comes online");
        w.AI.Money=2299; Frame+=5*TICKS_PER_SECOND;
        check(!mcv->Goto_Clear_Spot() && !Target_Legal(mcv->NavCom),"remote deployment orders wait when the first refinery and power are unfunded");
    }
    {
        World w; w.Base(false); w.Ore(50,28); w.Ore(50,45); Units.Data[0]->NavCom=As_Target(Cell(50,45));
        auto mcv=w.Unit(UNIT_MCV,21,15); mcv->Goto_Clear_Spot();
        check(Separation(Deployments[mcv->As_Target()].Goal,Cell(50,45))<=10,"active distant mining routes receive resource-expansion priority");
    }
    {
        World w; w.Base(); w.AI.CurBuildings=30; w.AI.HumanBuildingCount=1;
        w.AI.Native_Economic_Priority();
        check(w.AI.BuildStructure==STRUCT_REFINERY,"native building priority can fund useful mining infrastructure above the relative human base-size limit");
        w.AI.BuildStructure=STRUCT_NONE; w.Build(STRUCT_REFINERY,21,18); w.AI.CurBuildings=30;
        w.AI.Native_Economic_Priority();
        check(w.AI.BuildStructure==STRUCT_WEAP,"native building priority can fund sustained tank capacity above the relative human base-size limit");
        w.AI.BuildStructure=STRUCT_NONE; w.AI.CurBuildings=w.AI.AIMaxBuildings;
        w.AI.Native_Economic_Priority();
        check(w.AI.BuildStructure==STRUCT_NONE,"economic priority still respects the configured overall base-size cap");
    }
    {
        World w; w.Base(); BuildingClass * lost=Buildings.Data[1]; lost->IsInLimbo=true;
        for(auto & cell:Map.Cells) if(cell.Building==lost) cell.Building=nullptr;
        w.AI.BQuantity[STRUCT_WEAP]=0; w.AI.CurBuildings=30; w.AI.HumanBuildingCount=1;
        w.AI.Native_Economic_Priority();
        check(w.AI.BuildStructure==STRUCT_WEAP,"a destroyed primary war factory can recover even at the relative base-size limit");
        w.AI.BuildStructure=STRUCT_NONE; w.AI.UQuantity[UNIT_HARVESTER]=0; Units.Data[0]->IsInLimbo=Units.Data[1]->IsInLimbo=true;
        w.AI.Money=3399;
        check(w.AI.AI_Economic_Building()==STRUCT_NONE,"factory recovery with no trucks reserves the replacement harvester's purchase");
        w.AI.Money=3400;
        check(w.AI.AI_Economic_Building()==STRUCT_WEAP,"a destroyed factory and harvesting economy can recover at their combined funded price");
    }
    std::cout<<checks<<" actual economic expansion, MCV, refinery and production scenarios passed.\n";
}
