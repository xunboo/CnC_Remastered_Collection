// Copyright 2026. Distributed under the project's GPLv3 license.
#ifndef LLMPROTOCOL_H
#define LLMPROTOCOL_H

#include <cstring>
#include <climits>
#include <map>
#include <string>

// The bridge parses JSON function arguments. The DLL receives this bounded,
// little-endian packet and independently validates every reference and action.
namespace LLM {
enum {
    ProtocolVersion = 2, MaxOrders = 3, MaxPlanTicks = 450,
    MaxSnapshotAgeMillis = 125000, MaxSnapshotHistory = 1024,
    MappingVersion = 2,
    HeaderBytes = 64, SnapshotBytes = 262144, CommandBytes = 512, LogPathBytes = 4096,
    LogPathOffset = HeaderBytes + SnapshotBytes + CommandBytes,
    MappingBytes = LogPathOffset + LogPathBytes,
    MappingMagic = 0x314d4c41, PlanMagic = 0x314e4c50
};
enum Group { GROUND, NAVAL, AIR };
enum Action { HOLD, ATTACK_TARGET, DEFEND_AREA, RETREAT_TO, HARASS_ECONOMY, RAID_POWER };
struct Order {
    int GroupID;
    int ActionID;
    unsigned int Target;
    unsigned int Generation;
    int X;
    int Y;
    int CommitPercent;
    int WithdrawPercent;
};
struct Plan {
    unsigned int Magic;
    unsigned int Version;
    unsigned int MatchLow;
    unsigned int MatchHigh;
    int SnapshotSeq;
    int House;
    int ValidForTicks;
    int Count;
    Order Orders[MaxOrders];
};
static_assert(sizeof(int) == 4 && sizeof(Order) == 32 && sizeof(Plan) == 128,
    "LLM wire protocol requires 32-bit words");

inline char const * Group_Name(int group)
{
    return group == GROUND ? "ground" : group == NAVAL ? "naval" : "air";
}
inline char const * Action_Name(int action)
{
    static char const * names[] = {"hold", "attack_target", "defend_area", "retreat_to", "harass_economy", "raid_power"};
    return action >= HOLD && action <= RAID_POWER ? names[action] : "unknown";
}
inline bool Target_Action(int action)
{
    return action == ATTACK_TARGET || action == HARASS_ECONOMY || action == RAID_POWER;
}
inline bool Decode(void const * bytes, unsigned int length, Plan & plan)
{
    if (length < 32 || length > sizeof(Plan)) return false;
    std::memset(&plan, 0, sizeof(plan));
    std::memcpy(&plan, bytes, length);
    if (plan.Magic != PlanMagic || plan.Version != ProtocolVersion || plan.Count < 0
        || plan.Count > MaxOrders || length != 32u + 32u * plan.Count
        || plan.SnapshotSeq <= 0 || plan.House < 0
        || plan.ValidForTicks < 1 || plan.ValidForTicks > MaxPlanTicks) return false;
    unsigned int groups = 0;
    for (int i = 0; i < plan.Count; ++i) {
        Order const & order = plan.Orders[i];
        if (order.GroupID < GROUND || order.GroupID > AIR || (groups & (1u << order.GroupID))
            || order.ActionID < HOLD || order.ActionID > RAID_POWER
            || order.CommitPercent < 10 || order.CommitPercent > 100
            || order.WithdrawPercent < 0 || order.WithdrawPercent > 90) return false;
        groups |= 1u << order.GroupID;
        if (Target_Action(order.ActionID)) {
            if (!order.Target || !order.Generation || order.X != -1 || order.Y != -1) return false;
            if (order.ActionID == RAID_POWER && order.GroupID != AIR) return false;
        } else {
            if (order.Target || order.Generation) return false;
            if (order.ActionID == HOLD) {
                if (order.X != -1 || order.Y != -1) return false;
            } else if (order.X < 0 || order.X >= 128 || order.Y < 0 || order.Y >= 128) return false;
        }
    }
    return true;
}
inline bool Fresh(Plan const & plan, unsigned int low, unsigned int high, int house,
    int frame, int captured_frame, int last_applied_seq, unsigned int snapshot_age_millis)
{
    return plan.MatchLow == low && plan.MatchHigh == high && plan.House == house
        && plan.SnapshotSeq > last_applied_seq && captured_frame >= 0 && captured_frame <= frame
        && snapshot_age_millis <= MaxSnapshotAgeMillis
        && plan.ValidForTicks >= 1 && plan.ValidForTicks <= MaxPlanTicks
        && (long long)frame + plan.ValidForTicks <= INT_MAX;
}

// Construction hooks catch even death/reallocation within one simulation frame.
// This registry is transient; no serialized game object layout is changed.
class Identities {
    std::map<unsigned int, unsigned int> Generations;
public:
    void Clear() { Generations.clear(); }
    void Created(unsigned int target) { unsigned int & value = Generations[target]; if (++value == 0) ++value; }
    unsigned int Generation(unsigned int target) {
        unsigned int & value = Generations[target];
        if (!value) value = 1; // Objects restored using NoInit constructors.
        return value;
    }
};
}

#endif
