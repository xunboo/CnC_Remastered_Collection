// Compile the actual harvester controller and native harvest state machine.
#include "../REDALERT/HARVESTAI.H"
#include "../REDALERT/AIEXPANSION.H"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <vector>
using std::min;
using std::max;
#define ARRAY_SIZE(a) (int)(sizeof(a)/sizeof((a)[0]))
typedef int CELL;
typedef unsigned int COORDINATE;
typedef unsigned int TARGET;
typedef int OverlayType;
typedef int MissionType;
const int MAP_CELL_W=128, MAP_CELL_H=128, MAP_CELL_TOTAL=16384, CELL_LEPTON_W=256, TICKS_PER_SECOND=15;
const TARGET TARGET_NONE=0;
const int GAME_NORMAL=0, RTTI_BUILDING=1, RTTI_UNIT=2, UNIT_HARVESTER=0, STRUCT_REFINERY=0;
const int STRUCTF_REFINERY=1, STRUCTF_CONST=2, STRUCTF_REPAIR=4;
const int MISSION_NONE=-1, MISSION_HARVEST=0, MISSION_ENTER=1, MISSION_MOVE=2, MISSION_GUARD=3;
const int MISSION_CONSTRUCTION=4, MISSION_DECONSTRUCTION=5, MISSION_REPAIR=6, MISSION_HUNT=7;
const int BSTATE_CONSTRUCTION=1, LAND_CLEAR=0, LAND_TIBERIUM=1, DIR_S=4;
const int OVERLAY_NONE=-1, OVERLAY_GOLD1=0, OVERLAY_GOLD2=1, OVERLAY_GOLD3=2, OVERLAY_GOLD4=3;
const int OVERLAY_GEMS1=4, OVERLAY_GEMS2=5, OVERLAY_GEMS3=6, OVERLAY_GEMS4=7, OVERLAY_WALL=8;
const int RADIO_HELLO=1, RADIO_ROGER=2, RADIO_NEGATIVE=3, VOX_NEED_MO_CAPACITY=1;
int Frame=0, ScenarioInit=0;
struct { int Type=1; } Session;
struct {
    bool HarvesterOptimizeEnabled=true, HarvesterLoadBalancing=true, HarvesterQueueJumpingEnabled=true;
    bool AstarPathFindingEnabled=true, IsAutoCrush=true;
    int HarvyOptimizeUnloadWaitWeight=6, AIHarvesterMemoryValue=100;
    int TiberiumLongScan=40*256, TiberiumShortScan=8*256, OreDumpRate=1;
    int GoldValue=25, GemValue=50, BailCount=28;
    double ConditionRed=0.25, ConditionYellow=0.5;
} Rule;
struct { int Normal_Delay() const { return 1; } } MissionControl[8];
struct HouseClass;
HouseClass * PlayerPtr=nullptr;
COORDINATE Cell_Coord(CELL c) { return ((unsigned int)(c/128*256+128)<<16)|(unsigned int)(c%128*256+128); }
CELL Coord_Cell(COORDINATE c) { return (int)((c&65535)/256+(c>>16)/256*128); }
int Cell_X(CELL c) { return c%128; }
int Cell_Y(CELL c) { return c/128; }
CELL XY_Cell(int x,int y) { return y*128+x; }
COORDINATE Adjacent_Cell(COORDINATE c,int) { return Cell_Coord(Coord_Cell(c)+128); }
TARGET As_Target(CELL c) { return 0x80000000u|(unsigned int)c; }
bool Target_Legal(TARGET t) { return t!=0; }
struct TechnoClass;
std::map<TARGET,TechnoClass*> Objects;
CELL As_Cell(TARGET t);
struct TechnoClass {
    HouseClass * House=nullptr;
    int Kind=RTTI_UNIT, Strength=500;
    bool IsActive=true, IsInLimbo=false, Crushable=false;
    COORDINATE Coord=0;
    TARGET Handle=0;
    TechnoClass * Contact=nullptr;
    virtual ~TechnoClass() {}
    COORDINATE Center_Coord() const { return Coord; }
    TARGET As_Target() const { return Handle; }
    int What_Am_I() const { return Kind; }
    bool In_Radio_Contact() const { return Contact!=nullptr; }
    TechnoClass * Contact_With_Whom() const { return Contact; }
};
CELL As_Cell(TARGET t) {
    if(t&0x80000000u) return (int)(t&65535);
    auto it=Objects.find(t); return it==Objects.end()?-1:Coord_Cell(it->second->Coord);
}
struct HouseClass {
    bool IsHuman=false, Allied=false, IsTiberiumShort=false;
    int ActiveBScan=STRUCTF_REFINERY|STRUCTF_CONST, Capacity=1000, Tiberium=0;
    bool Is_Ally(TechnoClass const * other) const { return other->House==this || (other->House && other->House->Allied); }
};
struct BuildingClass : TechnoClass {
    int Type=STRUCT_REFINERY, Mission=MISSION_GUARD, BState=0;
    bool Attached=false, IsLeader=false;
    operator int() const { return Type; }
    bool Is_Something_Attached() const { return Attached; }
};
BuildingClass * As_Building(TARGET t) {
    auto it=Objects.find(t); return it!=Objects.end() && it->second->Kind==RTTI_BUILDING ? static_cast<BuildingClass*>(it->second):nullptr;
}
struct UnitTypeClass {
    bool IsToHarvest=true;
    int Type=UNIT_HARVESTER, Speed=0, MZone=0, MaxStrength=500;
    int Harvester_Load_List[3]={0,1,2};
} HarvesterType;
struct FakeTeam { bool Assigned=false; operator bool() const { return Assigned; } bool Is_Valid() const { return Assigned; } };
class UnitClass : public TechnoClass {
public:
    UnitTypeClass const * Class=&HarvesterType;
    FakeTeam Team;
    bool IsTethered=false, IsDumping=false, IsHarvesting=false, IsDriving=false, AttackMove=false;
    bool IsOwnedByPlayer=false, IsUseless=false;
    int Mission=MISSION_HARVEST, MissionQueue=MISSION_NONE, Status=0, ID=0, Tiberium=0, Rate=0, Stage=0;
    TARGET NavCom=0, TarCom=0, ArchiveTarget=0;
    mutable TARGET TiberiumUnloadRefinery=0;
    operator int() const { return Class->Type; }
    void Assign_Destination(TARGET target) { NavCom=target; }
    void Assign_Target(TARGET target) { TarCom=target; }
    void Assign_Mission(int mission) { if(mission!=Mission) MissionQueue=mission; }
    void Commence() { if(MissionQueue!=MISSION_NONE) { Mission=MissionQueue; MissionQueue=MISSION_NONE; Status=0; } }
    void Set_Rate(int rate) { Rate=rate; }
    void Set_Stage(int stage) { Stage=stage; }
    int Fetch_Rate() const { return Rate; }
    int Fetch_Stage() const { return Stage; }
    void ResetAttackMove() { AttackMove=false; }
    void Clear_Navigation_List() {}
    double Tiberium_Load() const { return double(Tiberium)/Rule.BailCount; }
    double Health_Ratio() const { return Class->MaxStrength>0?double(Strength)/Class->MaxStrength:0; }
    int Pip_Count() const { return Tiberium*5/Rule.BailCount; }
    bool Harvesting() { return false; }
    CELL Nearby_Location(TechnoClass const * object) const { return Coord_Cell(object->Coord); }
    int Transmit_Message(int message,BuildingClass * refinery) {
        if(message==RADIO_HELLO && !refinery->Attached && (!refinery->Contact || refinery->Contact==this)) {
            Contact=refinery; refinery->Contact=this; return RADIO_ROGER;
        }
        return RADIO_NEGATIVE;
    }
    bool Should_Crush_It(TechnoClass const * target) const { return target->Crushable; }
    BuildingClass * Find_Docking_Bay(int,bool) const { return Harvest_Find_Refinery(); }
    BuildingClass * Find_Best_Refinery() const { return Harvest_Find_Refinery(); }
    BuildingClass * Tiberium_Unload_Refinery() const;
    int Tiberium_Check(CELL &,int,int);
    bool Goto_Tiberium(int);
    int Mission_Harvest();
    void ReconsiderRefinery();
    bool Harvest_Goto_Ore(int);
    BuildingClass * Harvest_Find_Refinery() const;
    CELL Harvest_Wait_Cell(BuildingClass const *) const;
    bool Harvest_Retreat(TechnoClass const *);
    void Harvest_Think();
    void Damage_Response(int loss,TechnoClass * source) {
        int previous_strength=Strength; Strength-=loss;
        if(Strength>0) {
#include "harvest_damage_response.inc"
        }
    }
};
// This controlled world contains harvesters only; expansion never owns them.
namespace AIExpansion { bool Controls(UnitClass const *) { return false; } }
template<class T> struct Heap {
    std::vector<T*> Data;
    int Count() const { return (int)Data.size(); }
    T * Ptr(int i) const { return Data[i]; }
    int ID(T const * object) const { return (int)(std::find(Data.begin(),Data.end(),object)-Data.begin()); }
};
Heap<UnitClass> Units;
Heap<BuildingClass> Buildings;
struct OverlayTypeClass {
    bool IsWall=false;
    static OverlayTypeClass const & As_Reference(int overlay) { static OverlayTypeClass wall{true},ore{false}; return overlay==OVERLAY_WALL?wall:ore; }
};
struct FakeCell {
    int Land=LAND_CLEAR, Overlay=OVERLAY_NONE, OverlayData=0, Zones[1]={1};
    bool Clear=true, Mapped=true;
    BuildingClass * Building=nullptr;
    TechnoClass * Occupant=nullptr;
    int Land_Type() const { return Land; }
    bool Is_Mapped(HouseClass const *) const { return Mapped; }
    bool Is_Clear_To_Move(int,bool,bool,int,int) const { return Clear; }
    BuildingClass * Cell_Building() const { return Building; }
    TechnoClass * Cell_Techno() const { return Occupant?Occupant:Building; }
};
struct {
    FakeCell Cells[MAP_CELL_TOTAL];
    int MapCellX=0, MapCellY=0, MapCellWidth=128, MapCellHeight=128;
    bool In_Radar(CELL c) const { return c>=0 && c<MAP_CELL_TOTAL && Cell_X(c)>=MapCellX && Cell_X(c)<MapCellX+MapCellWidth && Cell_Y(c)>=MapCellY && Cell_Y(c)<MapCellY+MapCellHeight; }
    FakeCell & operator[](CELL c) { assert(c>=0 && c<MAP_CELL_TOTAL); return Cells[c]; }
    FakeCell & operator[](COORDINATE c) { return (*this)[Coord_Cell(c)]; }
} Map;
bool Percent_Chance(int percent) { return percent>=100; }
int Random_Pick(int low,int) { return low; }
void Speak(int,HouseClass const *) {}
#include "harvest_unit_methods.inc"
#include "harvest_controller.inc"

struct World {
    HouseClass AI,Enemy,Ally;
    std::vector<std::unique_ptr<TechnoClass>> Owned;
    World() {
        Units.Data.clear(); Buildings.Data.clear(); Objects.clear(); HarvestAI::Reset(); Frame=0;
        Session.Type=1; PlayerPtr=&AI; Rule.HarvesterOptimizeEnabled=true; Rule.AstarPathFindingEnabled=true;
        Rule.HarvyOptimizeUnloadWaitWeight=6; Rule.AIHarvesterMemoryValue=100;
        Map.MapCellX=Map.MapCellY=0; Map.MapCellWidth=Map.MapCellHeight=128;
        for(auto & cell:Map.Cells) cell=FakeCell();
        Ally.Allied=true;
    }
    UnitClass * Truck(int x,int y) {
        std::unique_ptr<UnitClass> unit(new UnitClass);
        unit->ID=Units.Count(); unit->House=&AI; unit->Coord=Cell_Coord(XY_Cell(x,y)); unit->Handle=1000+Units.Count();
        UnitClass * ptr=unit.get(); Units.Data.push_back(ptr); Objects[ptr->Handle]=ptr;
        Map[XY_Cell(x,y)].Occupant=ptr; Owned.push_back(std::move(unit)); return ptr;
    }
    BuildingClass * Refinery(int x,int y) {
        std::unique_ptr<BuildingClass> building(new BuildingClass);
        building->Kind=RTTI_BUILDING; building->House=&AI; building->Coord=Cell_Coord(XY_Cell(x,y)); building->Handle=2000+Buildings.Count();
        BuildingClass * ptr=building.get(); Buildings.Data.push_back(ptr); Objects[ptr->Handle]=ptr;
        Map[XY_Cell(x,y)].Building=ptr; Owned.push_back(std::move(building)); return ptr;
    }
    void Ore(int x,int y,bool gems=false,int level=-1) {
        auto & cell=Map[XY_Cell(x,y)]; cell.Land=LAND_TIBERIUM; cell.Overlay=gems?OVERLAY_GEMS1:OVERLAY_GOLD1; cell.OverlayData=level>=0?level:(gems?3:12);
    }
    void Wall(int x,int y) { Map[XY_Cell(x,y)].Overlay=OVERLAY_WALL; }
    void Move(UnitClass * unit,int x,int y) { Map[Coord_Cell(unit->Coord)].Occupant=nullptr; unit->Coord=Cell_Coord(XY_Cell(x,y)); Map[XY_Cell(x,y)].Occupant=unit; }
};
int checks=0;
void check(bool ok,char const * message) { ++checks; if(!ok) { std::cerr<<"FAILED: "<<message<<"\n"; std::exit(1); } }
int main() {
    {
        HarvestAI::TravelMap route(5,5); route.Clear.assign(25,1); route.Clear[1]=route.Clear[5]=0; route.Search(0);
        check(route.Distance[6]==-1,"harvest routes cannot cut diagonal corners");
        route.Clear.assign(25,1); route.Search(4); check(route.Distance[5]==4,"routes do not wrap at the map edge");
        route.Search(-1); check(route.Distance[0]==-1,"invalid starting cells cannot seed a route");
    }
    {
        World w; auto u=w.Truck(20,20); w.Ore(23,20); w.Ore(24,20,true);
        u->Goto_Tiberium(40); check(u->NavCom==As_Target(XY_Cell(24,20)),"similar-distance gems beat ordinary ore");
    }
    {
        World w; auto u=w.Truck(20,20); w.Ore(23,20); w.Ore(30,20,true);
        u->Goto_Tiberium(40); check(u->NavCom==As_Target(XY_Cell(23,20)),"distant gems do not override a nearby ore field");
    }
    {
        World w; auto u=w.Truck(20,20); w.Ore(23,20,false,1); w.Ore(24,20,false,12);
        u->Goto_Tiberium(40); check(u->NavCom==As_Target(XY_Cell(24,20)),"dense ore wins within the nearby distance band");
    }
    {
        World w; auto u=w.Truck(20,20); w.Ore(23,20,true); w.Ore(20,24);
        for(int y=0;y<128;++y) w.Wall(22,y);
        u->Goto_Tiberium(40); check(u->NavCom==As_Target(XY_Cell(20,24)),"an unreachable rich field is rejected even in the same native zone");
    }
    {
        World w; auto u=w.Truck(20,20); w.Ore(23,20,true); w.Ore(20,24);
        for(int i=0;i<3;++i) { auto other=w.Truck(40+i,40); other->NavCom=As_Target(XY_Cell(23,20)); }
        u->Goto_Tiberium(40); check(u->NavCom==As_Target(XY_Cell(20,24)),"ore destinations already claimed by several trucks lose priority");
    }
    {
        World w; auto u=w.Truck(20,20); w.Ore(23,20,true); w.Ore(20,24,true);
        auto next=w.Truck(20,21); u->Goto_Tiberium(40); next->Goto_Tiberium(40);
        check(u->NavCom!=next->NavCom,"trucks spread between equivalent nearby rich fields");
    }
    {
        World w; auto u=w.Truck(20,20); w.Ore(23,20,true); auto occupied=w.Truck(23,20); (void)occupied; w.Ore(20,24);
        u->Goto_Tiberium(40); check(u->NavCom==As_Target(XY_Cell(20,24)),"a currently occupied ore cell is not selected");
    }
    {
        World w; auto u=w.Truck(20,20); u->NavCom=As_Target(XY_Cell(40,40)); w.Ore(23,20,true);
        u->Goto_Tiberium(40); check(u->NavCom==As_Target(XY_Cell(40,40)),"an active ore movement order is retained");
        u->NavCom=TARGET_NONE; w.Ore(20,20); check(u->Goto_Tiberium(40),"a truck already on ore starts harvesting without chasing a new field");
    }
    {
        World w; auto u=w.Truck(20,20); check(!u->Goto_Tiberium(40) && u->NavCom==TARGET_NONE,"exhausted maps do not produce fake ore destinations");
        w.Ore(23,20); u->Goto_Tiberium(2); check(u->NavCom==TARGET_NONE,"native short-scan bounds remain honored");
    }
    {
        World w; auto u=w.Truck(20,20); u->IsOwnedByPlayer=true; Session.Type=GAME_NORMAL;
        w.Ore(23,20,true); Map[XY_Cell(23,20)].Mapped=false; w.Ore(20,24);
        u->Goto_Tiberium(40); check(u->NavCom==As_Target(XY_Cell(20,24)),"campaign human harvesters do not select unexplored ore");
    }
    {
        World w; auto u=w.Truck(0,5); w.Ore(127,4,true); w.Ore(3,5);
        u->Goto_Tiberium(40); check(u->NavCom==As_Target(XY_Cell(3,5)),"ore scans near borders never wrap into the opposite map edge");
    }
    {
        World w; auto u=w.Truck(20,20); auto near=w.Refinery(23,20); w.Refinery(40,20);
        check(u->Harvest_Find_Refinery()==near,"the nearest reachable free refinery wins");
    }
    {
        World w; auto u=w.Truck(20,20); auto near=w.Refinery(23,20); near->Attached=true; auto free=w.Refinery(26,20);
        check(u->Harvest_Find_Refinery()==free,"a nearby free refinery beats an occupied unloading bay");
        free->Attached=true; check(u->Harvest_Find_Refinery()==near,"all busy refineries still allow a reachable shortest-wait choice");
    }
    {
        World w; auto u=w.Truck(20,20); auto claimed=w.Refinery(23,20); auto free=w.Refinery(26,20);
        auto other=w.Truck(40,40); other->Status=2; other->Tiberium=28; other->TiberiumUnloadRefinery=claimed->As_Target();
        check(u->Harvest_Find_Refinery()==free,"incoming truck reservations reduce refinery stampedes");
        other->Tiberium=0; check(u->Harvest_Find_Refinery()==claimed,"empty outbound trucks do not keep an unloading queue reserved");
    }
    {
        World w; auto u=w.Truck(20,20); auto blocked=w.Refinery(23,20); auto accessible=w.Refinery(20,26);
        w.Wall(23,21); check(u->Harvest_Find_Refinery()==accessible,"a blocked real docking entrance cannot fall back to straight-line distance");
        (void)blocked;
        for(int x=0;x<128;++x) w.Wall(x,25);
        check(u->Harvest_Find_Refinery()==nullptr,"no reachable refinery gives no invalid docking destination");
    }
    {
        World w; auto u=w.Truck(20,20); auto near=w.Refinery(23,20); auto ignored=w.Refinery(22,20);
        ignored->House=&w.Enemy; check(u->Harvest_Find_Refinery()==near,"enemy refineries are excluded");
        ignored->House=&w.Ally; check(u->Harvest_Find_Refinery()==near,"automatic unload selection stays with the owning house");
        ignored->House=&w.AI; ignored->Mission=MISSION_CONSTRUCTION; check(u->Harvest_Find_Refinery()==near,"unfinished refineries are excluded");
        ignored->Mission=MISSION_DECONSTRUCTION; check(u->Harvest_Find_Refinery()==near,"selling refineries are excluded");
        ignored->Mission=MISSION_GUARD; ignored->BState=BSTATE_CONSTRUCTION; check(u->Harvest_Find_Refinery()==near,"construction animation state is excluded");
        ignored->BState=0; ignored->Strength=0; check(u->Harvest_Find_Refinery()==near,"destroyed refineries are excluded");
        ignored->Strength=500; ignored->IsInLimbo=true; check(u->Harvest_Find_Refinery()==near,"limbo refineries are excluded");
        ignored->IsInLimbo=false; ignored->IsActive=false; check(u->Harvest_Find_Refinery()==near,"inactive refineries are excluded");
    }
    {
        World w; auto u=w.Truck(20,20); auto near=w.Refinery(23,20); auto far=w.Refinery(40,20); far->IsLeader=true;
        check(u->Harvest_Find_Refinery()==near,"primary-factory flags do not force a longer refinery return");
        u->Contact=far; far->Contact=u; check(u->Harvest_Find_Refinery()==far,"accepted radio docking is preserved");
    }
    {
        World w; auto u=w.Truck(20,20); w.Refinery(23,20); auto current=w.Refinery(24,20);
        u->TiberiumUnloadRefinery=current->As_Target(); check(u->Harvest_Find_Refinery()==current,"small equivalent score changes do not cause refinery thrashing");
        current->Attached=true; check(u->Harvest_Find_Refinery()!=current,"a busy remembered refinery can yield to a free nearby one");
    }
    {
        World w; auto u=w.Truck(20,20); auto near=w.Refinery(23,20); near->Attached=true; auto free=w.Refinery(26,20);
        Rule.AstarPathFindingEnabled=false; Rule.HarvyOptimizeUnloadWaitWeight=6*256;
        check(u->Harvest_Find_Refinery()==free,"queue weights stay in cells when native A-star is disabled");
    }
    {
        World w; auto u=w.Truck(20,20); auto refinery=w.Refinery(25,20);
        CELL first=u->Harvest_Wait_Cell(refinery); int gap=max(std::abs(Cell_X(first)-25),std::abs(Cell_Y(first)-21));
        check(gap>=2 && gap<=4,"waiting cells leave the refinery unloading pad clear");
        auto other=w.Truck(20,21); u->NavCom=As_Target(first); CELL second=other->Harvest_Wait_Cell(refinery);
        check(first!=second,"waiting trucks use separate holding squares");
        w.Move(u,Cell_X(first),Cell_Y(first));
        check(u->Harvest_Wait_Cell(refinery)==first,"an arrived waiting truck can remain in its own holding square");
    }
    {
        World w; auto u=w.Truck(20,20); auto refinery=w.Refinery(23,20); u->Tiberium=1; u->Strength=130;
        u->ArchiveTarget=As_Target(XY_Cell(40,40)); u->NavCom=As_Target(XY_Cell(30,30)); u->IsHarvesting=true;
        UnitClass attacker; attacker.House=&w.Enemy; attacker.Crushable=true;
        u->Damage_Response(10,&attacker);
        check(u->MissionQueue==MISSION_ENTER && u->Contact==refinery,"critical loaded trucks return before attempting to crush infantry");
        check(u->Pip_Count()==0 && u->Tiberium==1,"a partial load below one visible pip can still be saved");
        check(!u->IsHarvesting && u->NavCom==TARGET_NONE && u->ArchiveTarget==TARGET_NONE,"early return clears mining orders but retains cargo");
        u->Commence(); check(u->Harvest_Retreat(&attacker) && u->Contact==refinery,"continued hits preserve an accepted unloading trip");
    }
    {
        World w; auto u=w.Truck(20,20); auto refinery=w.Refinery(23,20); refinery->Attached=true;
        u->Tiberium=10; u->Strength=100; UnitClass attacker; attacker.House=&w.Enemy;
        check(u->Harvest_Retreat(&attacker),"critical trucks can return toward a busy refinery");
        check(u->Status==2 && u->Mission==MISSION_HARVEST && Target_Legal(u->NavCom) && !u->In_Radio_Contact(),"busy early returns use native waiting state rather than forcing entry");
        check(u->TiberiumUnloadRefinery==refinery->As_Target(),"partial early returns reserve their selected refinery");
    }
    {
        World w; auto u=w.Truck(20,20); w.Refinery(23,20); u->Tiberium=10; u->Strength=100;
        UnitClass attacker; attacker.House=&w.Enemy;
        u->Mission=MISSION_MOVE; check(!u->Harvest_Retreat(&attacker),"retreat helper preserves explicit movement orders");
        u->Mission=MISSION_REPAIR; check(!u->Harvest_Retreat(&attacker),"retreat helper preserves repair trips");
        u->Mission=MISSION_HARVEST; u->MissionQueue=MISSION_MOVE; check(!u->Harvest_Retreat(&attacker),"queued manual orders are not replaced");
        u->MissionQueue=MISSION_NONE; u->Team.Assigned=true; check(!u->Harvest_Retreat(&attacker),"scripted teams retain control");
        u->Team.Assigned=false; u->IsTethered=true; check(!u->Harvest_Retreat(&attacker),"docking animations are not interrupted");
        u->IsTethered=false; u->IsDumping=true; check(!u->Harvest_Retreat(&attacker),"active unloading is not interrupted");
        u->IsDumping=false; u->Tiberium=0; check(!u->Harvest_Retreat(&attacker),"empty trucks do not start an unload cycle");
        u->Tiberium=10; attacker.House=&w.Ally; check(!u->Harvest_Retreat(&attacker),"allied fire does not trigger enemy retreat behavior");
        attacker.House=&w.Enemy; u->Strength=300; check(!u->Harvest_Retreat(&attacker),"healthy trucks retain harvesting");
        u->Strength=100; check(!u->Harvest_Retreat(nullptr),"damage without an attacker does not trigger this combat response");
        Rule.HarvesterOptimizeEnabled=false; check(!u->Harvest_Retreat(&attacker),"the legacy optimization switch remains respected");
    }
    {
        World w; auto u=w.Truck(20,20); w.Refinery(23,20); UnitClass attacker; attacker.House=&w.Enemy; attacker.Crushable=true; attacker.Handle=777;
        u->Tiberium=10; u->Strength=130; u->Mission=MISSION_MOVE; u->NavCom=As_Target(XY_Cell(40,40));
        u->Damage_Response(10,&attacker);
        check(u->MissionQueue==MISSION_NONE && u->NavCom==As_Target(XY_Cell(40,40)),"actual damage response preserves manual movement instead of chasing infantry");
        u->Mission=MISSION_HARVEST; u->MissionQueue=MISSION_REPAIR; u->Damage_Response(10,&attacker);
        check(u->MissionQueue==MISSION_REPAIR && u->NavCom==As_Target(XY_Cell(40,40)),"actual damage response preserves a queued repair command");
        u->Mission=MISSION_REPAIR; u->MissionQueue=MISSION_NONE; u->Damage_Response(10,&attacker);
        check(u->MissionQueue==MISSION_NONE && u->NavCom==As_Target(XY_Cell(40,40)),"actual damage response preserves a current repair trip");
    }
    {
        World w; auto u=w.Truck(20,20); w.Refinery(23,20); UnitClass attacker; attacker.House=&w.Enemy;
        u->Tiberium=1; u->Strength=100; u->Damage_Response(0,&attacker);
        check(u->MissionQueue==MISSION_NONE && !u->In_Radio_Contact(),"a zero-damage hit does not request early unloading");
    }
    {
        World w; auto u=w.Truck(20,20); w.Refinery(23,20); u->Tiberium=Rule.BailCount; u->NavCom=As_Target(XY_Cell(30,30));
        u->Mission_Harvest(); check(u->Status==2 && u->NavCom==TARGET_NONE,"a full load clears its ore destination before FINDHOME");
        u->Mission_Harvest(); check(u->Status==3 && u->In_Radio_Contact(),"the native FINDHOME state accepts the selected refinery");
        u->Mission_Harvest(); check(u->MissionQueue==MISSION_ENTER,"the native HEADINGHOME state proceeds to unloading");
    }
    {
        World w; auto u=w.Truck(20,20); w.Refinery(23,20); w.Ore(20,24); w.Ore(45,45,true);
        u->ArchiveTarget=As_Target(XY_Cell(45,45)); u->Mission_Harvest();
        check(u->NavCom==As_Target(XY_Cell(20,24)),"unloading-area scans replace distant remembered ore in optimized mode");
        u->NavCom=0; u->Status=0; u->ArchiveTarget=As_Target(XY_Cell(45,45)); Rule.HarvesterOptimizeEnabled=false;
        u->Mission_Harvest(); check(u->NavCom==As_Target(XY_Cell(45,45)),"disabled optimization retains configured legacy field memory");
    }
    {
        World w; auto u=w.Truck(20,20); auto refinery=w.Refinery(23,20); refinery->Attached=true;
        u->Status=2; u->Tiberium=28; u->TiberiumUnloadRefinery=refinery->As_Target(); u->NavCom=As_Target(XY_Cell(20,21));
        u->ReconsiderRefinery(); check(u->NavCom==As_Target(XY_Cell(20,21)),"unload events do not blindly erase an unchanged busy queue route");
        refinery->Attached=false; u->ReconsiderRefinery(); check(u->NavCom==0 && u->TiberiumUnloadRefinery==refinery->As_Target(),"a newly free remembered refinery immediately retries docking");
        u->NavCom=As_Target(XY_Cell(20,21)); refinery->Mission=MISSION_DECONSTRUCTION; u->ReconsiderRefinery();
        check(u->NavCom==0 && u->TiberiumUnloadRefinery==0,"a sold queue destination is invalidated");
    }
    {
        World w; auto u=w.Truck(20,20); auto near=w.Refinery(23,20); near->Attached=true;
        u->Status=2; u->Tiberium=28; u->TiberiumUnloadRefinery=near->As_Target(); u->NavCom=As_Target(XY_Cell(20,21));
        u->Harvest_Think(); auto free=w.Refinery(26,20); Frame=3*TICKS_PER_SECOND; u->Harvest_Think();
        check(u->NavCom==0 && u->TiberiumUnloadRefinery==free->As_Target(),"waiting trucks periodically notice a new nearby free refinery");
        u->Contact=near; near->Contact=u; u->TiberiumUnloadRefinery=near->As_Target(); u->NavCom=As_Target(XY_Cell(23,21)); u->ReconsiderRefinery();
        check(u->NavCom==As_Target(XY_Cell(23,21)),"periodic reconsideration preserves accepted docking contact");
    }
    {
        World w; auto u=w.Truck(20,20); u->NavCom=As_Target(XY_Cell(30,30));
        u->Harvest_Think(); Frame=12*TICKS_PER_SECOND; u->Harvest_Think();
        check(u->NavCom==0,"a stalled autonomous ore route is released for another scan");
        u->Status=1; u->NavCom=As_Target(XY_Cell(31,31)); Frame+=3*TICKS_PER_SECOND; u->Harvest_Think();
        Frame+=12*TICKS_PER_SECOND; u->Harvest_Think();
        check(u->NavCom==0,"short-field travel in the harvesting state can also recover from a stall");
        u->Status=0;
        u->NavCom=As_Target(XY_Cell(30,30)); Frame+=TICKS_PER_SECOND; u->Harvest_Think();
        w.Move(u,21,20); Frame+=10*TICKS_PER_SECOND; u->Harvest_Think();
        check(Target_Legal(u->NavCom),"moving trucks retain progress on their ore route");
        u->MissionQueue=MISSION_MOVE; Frame+=20*TICKS_PER_SECOND; u->Harvest_Think();
        check(Target_Legal(u->NavCom),"stalled-route checks preserve queued player orders");
        u->MissionQueue=MISSION_NONE; HarvestAI::Forget(u); Frame+=TICKS_PER_SECOND; u->Harvest_Think();
        check(Target_Legal(u->NavCom),"a reused unit slot starts with fresh transient route timing");
    }
    std::cout<<checks<<" actual harvester routing, unloading, damage and state-machine scenarios passed.\n";
}
