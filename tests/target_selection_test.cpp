// Compiles the actual Greatest_Threat implementation with a small fake map.
// Object scoring is controlled by the test; traversal and selection are game code.
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>
using std::max;
typedef int TARGET;
typedef int CELL;
typedef int COORDINATE;
typedef int ThreatType;
const int TARGET_NONE = 0, ICON_LEPTON_W = 1, LAYER_GROUND = 0, LAYER_AIR = 1;
const int RTTI_UNIT = 1, RTTI_BUILDING = 2, RTTI_AIRCRAFT = 3, RTTI_VESSEL = 4, RTTI_INFANTRY = 5;
const int THREAT_RANGE = 1 << 0, THREAT_AREA = 1 << 1, THREAT_INFANTRY = 1 << 2;
const int THREAT_CIVILIANS = 1 << 3, THREAT_AIR = 1 << 4, THREAT_CAPTURE = 1 << 5;
const int THREAT_BUILDINGS = 1 << 6, THREAT_FACTORIES = 1 << 7, THREAT_POWER = 1 << 8;
const int THREAT_FAKES = 1 << 9, THREAT_BASE_DEFENSE = 1 << 10, THREAT_TIBERIUM = 1 << 11;
const int THREAT_VEHICLES = 1 << 12, THREAT_BOATS = 1 << 13;
#define BStart(x) ((void)0)
#define BEnd(x) ((void)0)

int Cell_X(CELL cell) { return cell % 12; }
int Cell_Y(CELL cell) { return cell / 12; }
CELL XY_Cell(int x, int y) { return y * 12 + x; }
CELL Coord_Cell(COORDINATE coord) { return coord; }
TARGET As_Target(CELL cell) { return 1000 + cell; }

struct ObjectClass {
    int Target, Score, Kind, Layer, Position;
    ObjectClass(int target = 0, int score = 0, int position = 0, int kind = RTTI_UNIT, int layer = LAYER_GROUND)
        : Target(target), Score(score), Kind(kind), Layer(layer), Position(position) {}
    TARGET As_Target() const { return Target; }
    bool Is_Techno() const { return true; }
    int What_Am_I() const { return Kind; }
    int In_Which_Layer() const { return Layer; }
};
struct TechnoType { int MZone; bool IsDog; TechnoType() : MZone(0), IsDog(false) {} };
struct TechnoClass : ObjectClass {
    bool IsActive;
    int Range, Damage;
    TechnoType Type;
    TechnoClass(int target = 0, int score = 0, int position = 0, int kind = RTTI_UNIT, int layer = LAYER_GROUND)
        : ObjectClass(target, score, position, kind, layer), IsActive(true), Range(8), Damage(10) {}
    TARGET Greatest_Threat(ThreatType) const;
    COORDINATE Center_Coord() const { return Position; }
    COORDINATE Fire_Coord(int) const { return Position; }
    TechnoType const * Techno_Type_Class() const { return &Type; }
    int Combat_Damage() const { return Damage; }
    int Threat_Range(int) const { return Range; }
    int Weapon_Range(int) const { return Range; }
    bool Evaluate_Object(ThreatType, int, int, TechnoClass const *, int &, int = -1) const;
    bool Evaluate_Cell(ThreatType, int, CELL, int, TechnoClass const **, int &, int) const;
    int Evaluate_Just_Cell(CELL) const { return 0; }
};
struct InfantryClass : TechnoClass {
    TechnoType * Class;
    InfantryClass() : Class(&Type) {}
};
template<class T> struct ObjectList {
    std::vector<T *> Objects;
    int Count() const { return (int)Objects.size(); }
    T * Ptr(int index) const { return Objects[index]; }
    T * operator[](int index) const { return Objects[index]; }
};
struct FakeCell { int Zones[1]; TechnoClass * Object; FakeCell() : Object(NULL) { Zones[0] = 0; } };
struct FakeMap {
    int MapCellX, MapCellY, MapCellWidth, MapCellHeight;
    FakeCell Cells[144];
    ObjectList<ObjectClass> Layer[2];
    FakeMap() : MapCellX(0), MapCellY(0), MapCellWidth(12), MapCellHeight(12) {}
    FakeCell & operator[](int cell) { assert(cell >= 0 && cell < 144); return Cells[cell]; }
} Map;
ObjectList<TechnoClass> Aircraft;
int TargetScan = 0;

bool TechnoClass::Evaluate_Object(ThreatType, int mask, int range, TechnoClass const * object, int & value, int) const
{
    if (object == NULL || !(mask & (1 << object->Kind))) return false;
    if (range >= 0 && max(std::abs(Cell_X(Position) - Cell_X(object->Position)), std::abs(Cell_Y(Position) - Cell_Y(object->Position))) > range) return false;
    value = object->Score;
    return value > 0;
}
bool TechnoClass::Evaluate_Cell(ThreatType threat, int mask, CELL cell, int range, TechnoClass const ** object, int & value, int zone) const
{
    *object = Map[cell].Object;
    return Evaluate_Object(threat, mask, range, *object, value, zone);
}

#include "greatest_threat.inc"

static void reset()
{
    Map = FakeMap();
    Aircraft.Objects.clear();
}
static int checks = 0;
static void check(bool condition, char const * message)
{
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main()
{
    TechnoClass shooter(99, 1, XY_Cell(5, 5));
    // The strongest target is followed by a weaker one on each scan edge.
    int positions[][2] = {
        {XY_Cell(4, 4), XY_Cell(5, 4)}, {XY_Cell(4, 6), XY_Cell(5, 6)},
        {XY_Cell(3, 4), XY_Cell(3, 5)}, {XY_Cell(7, 4), XY_Cell(7, 5)}
    };
    char const * messages[] = {"top edge retains the highest-value target", "bottom edge retains the highest-value target",
        "left edge retains the highest-value target", "right edge retains the highest-value target"};
    for (int edge = 0; edge < 4; ++edge) {
        reset();
        TechnoClass strong(1, 100, positions[edge][0]);
        TechnoClass weak(2, 10, positions[edge][1]);
        Map[strong.Position].Object = &strong;
        Map[weak.Position].Object = &weak;
        check(shooter.Greatest_Threat(THREAT_RANGE | THREAT_VEHICLES) == strong.Target, messages[edge]);
    }
    reset();
    TechnoClass plane(3, 30, XY_Cell(5, 4), RTTI_AIRCRAFT, LAYER_AIR);
    TechnoClass strong(1, 100, XY_Cell(4, 4));
    TechnoClass weak(2, 40, XY_Cell(5, 4));
    Aircraft.Objects.push_back(&plane);
    Map[strong.Position].Object = &strong;
    Map[weak.Position].Object = &weak;
    check(shooter.Greatest_Threat(THREAT_RANGE | THREAT_VEHICLES | THREAT_AIR) == strong.Target, "a weaker later target cannot replace the strongest ground target after an air scan");
    reset();
    check(shooter.Greatest_Threat(THREAT_RANGE | THREAT_VEHICLES) == TARGET_NONE, "empty maps have no target");
    reset();
    Map.Layer[LAYER_GROUND].Objects.push_back(&strong);
    Map.Layer[LAYER_GROUND].Objects.push_back(&weak);
    check(shooter.Greatest_Threat(THREAT_VEHICLES) == strong.Target, "full-map target selection still retains the best score");
    reset();
    shooter.Position = XY_Cell(0, 0);
    TechnoClass border(4, 70, XY_Cell(1, 1));
    Map[border.Position].Object = &border;
    check(shooter.Greatest_Threat(THREAT_RANGE | THREAT_VEHICLES) == border.Target, "map-edge scans stay in bounds");
    std::cout << checks << " actual target-selection scenarios passed.\n";
}
