// Copyright 2026. Distributed under the project's GPLv3 license.
// Included inside AITACTICS.CPP's private namespace; uses the real route,
// weapon, defense and strike helpers rather than a second unit controller.
struct LLMMember {
    TARGET Target;
    unsigned int Generation;
};
struct LLMSnapshot {
    int Seq;
    int FrameNumber;
    std::vector<LLMMember> Groups[3];
    std::vector<LLMMember> Targets;
};
struct LLMDirective {
    bool Active;
    int ExpiresAt;
    int SnapshotSeq;
    int RetreatCell;
    LLM::Order Order;
    std::vector<LLMMember> Members;
    bool DefenseReported;
    LLMDirective() : Active(false), ExpiresAt(0), SnapshotSeq(0), RetreatCell(0), Order(), DefenseReported(false) {}
};
struct LLMResult {
    int Seq;
    int Group;
    std::string Status;
};
struct LLMHouseState {
    int NextSnapshot;
    int LastApplied;
    std::deque<LLMSnapshot> History;
    std::deque<LLMResult> Results;
    LLMDirective Directives[3];
    LLMHouseState() : NextSnapshot(0), LastApplied(0) {}
};
LLMHouseState LLMStates[HOUSE_COUNT];
unsigned int LLMMatchLow = 0, LLMMatchHigh = 0;
int LLMSnapshotSeq = 0;

static StrikePlan & LLM_Strike(TacticalState & state, int group)
{
    return group == LLM::GROUND ? state.Ground : group == LLM::NAVAL ? state.Fleet : state.Air;
}
static int LLM_Group(TechnoClass const * object)
{
    return object->What_Am_I() == RTTI_AIRCRAFT ? LLM::AIR
        : object->What_Am_I() == RTTI_VESSEL ? LLM::NAVAL : LLM::GROUND;
}
static bool LLM_Group_Controlled(HouseClass const * house, int group)
{
    LLMDirective const & directive = LLMStates[house->Class->House].Directives[group];
    return directive.Active && Frame < directive.ExpiresAt;
}
static bool LLM_Has_Member(HouseClass const * house, TARGET target)
{
    for (int group = 0; group < 3; ++group) {
        if (!LLM_Group_Controlled(house, group)) continue;
        std::vector<LLMMember> const & members = LLMStates[house->Class->House].Directives[group].Members;
        for (unsigned int i = 0; i < members.size(); ++i)
            if (members[i].Target == target && members[i].Generation == LLMBridge::Object_Generation(target)) return true;
    }
    return false;
}
static void LLM_Reset()
{
    for (int house = 0; house < HOUSE_COUNT; ++house) LLMStates[house] = LLMHouseState();
    LLMSnapshotSeq = 0;
    LLMBridge::Reset();
    LLMBridge::New_Match(LLMMatchLow, LLMMatchHigh);
}
static void LLM_Result(LLMHouseState & state, int seq, int group, char const * status)
{
    LLMResult result = {seq, group, status};
    state.Results.push_back(result);
    if (state.Results.size() > 16) state.Results.pop_front();
    AILog::Record(Frame, static_cast<int>(&state - LLMStates), "llm_execution_result",
        AILog::Fields().Number("snapshot_seq", seq).Number("group", group).Text("status", status));
}
static char const * LLM_Category(TechnoClass const * object)
{
    if (object->What_Am_I() == RTTI_BUILDING) {
        StructType type = ((BuildingClass const *)object)->Class->Type;
        if (type == STRUCT_POWER || type == STRUCT_ADVANCED_POWER) return "power";
        if (type == STRUCT_REFINERY) return "refinery";
        return "building";
    }
    if (object->What_Am_I() == RTTI_UNIT && ((UnitClass const *)object)->Class->Type == UNIT_HARVESTER) return "harvester";
    return "military";
}
static std::string LLM_Quoted(char const * value)
{
    std::ostringstream text;
    text << '"';
    for (unsigned char const * c = reinterpret_cast<unsigned char const *>(value); *c; ++c) {
        if (*c == '"' || *c == '\\') text << '\\' << static_cast<char>(*c);
        else if (*c < 32 || *c >= 127) text << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (unsigned int)*c << std::dec;
        else text << static_cast<char>(*c);
    }
    text << '"';
    return text.str();
}
static void LLM_ID(std::ostream & json, LLMMember const & member)
{
    json << '"' << member.Target << ':' << member.Generation << '"';
}
static LLMMember LLM_Reference(TechnoClass const * object)
{
    LLMMember member = {object->As_Target(), LLMBridge::Object_Generation(object->As_Target())};
    return member;
}

static void LLM_Publish(HouseClass * house, std::vector<Fighter> const & army,
    std::vector<Contact> const & contacts, std::vector<TARGET> const & defenders)
{
    LLMHouseState & state = LLMStates[house->Class->House];
    if (Frame < state.NextSnapshot || !LLMBridge::Can_Control(house->Class->House)) return;
    state.NextSnapshot = Frame + TICKS_PER_SECOND;
    LLMSnapshot snapshot;
    snapshot.Seq = ++LLMSnapshotSeq; snapshot.FrameNumber = Frame;
    FootClass const * representatives[3] = {};
    int priority[3] = {-1, -1, -1};
    for (unsigned int i = 0; i < army.size(); ++i) {
        Fighter const & fighter = army[i];
        int group = LLM_Group(fighter.Object);
        if (Contains(defenders, fighter.Object->As_Target()) || snapshot.Groups[group].size() >= 256) continue;
        snapshot.Groups[group].push_back(LLM_Reference(fighter.Object));
        int score = fighter.Power + (fighter.Object->What_Am_I() == RTTI_UNIT ? 100000 : 0);
        if (score > priority[group]) { representatives[group] = fighter.Object; priority[group] = score; }
    }
    RouteMap fields[3] = {RouteMap(MAP_CELL_W, MAP_CELL_H), RouteMap(MAP_CELL_W, MAP_CELL_H), RouteMap(MAP_CELL_W, MAP_CELL_H)};
    for (int group = 0; group < 3; ++group)
        if (representatives[group]) Fill_Routes(fields[group], representatives[group], Coord_Cell(representatives[group]->Center_Coord()));
    std::ostringstream json;
    json << "{\"protocol_version\":1,\"match_id\":\"" << std::hex << std::setw(8) << std::setfill('0')
        << LLMMatchHigh << std::setw(8) << LLMMatchLow << std::dec << "\",\"snapshot_seq\":" << snapshot.Seq
        << ",\"sim_frame\":" << Frame << ",\"controlled_house_id\":" << (int)house->Class->House
        << ",\"visibility_mode\":\"omniscient\",\"ticks_per_second\":" << TICKS_PER_SECOND
        << ",\"map_bounds\":{\"x\":" << Map.MapCellX << ",\"y\":" << Map.MapCellY
        << ",\"width\":" << Map.MapCellWidth << ",\"height\":" << Map.MapCellHeight << "}"
        << ",\"self\":{\"credits\":" << house->Available_Money() << ",\"power\":" << house->Power
        << ",\"drain\":" << house->Drain << ",\"base_cell\":[" << Coord_Cell(house->Center) % MAP_CELL_W
        << ',' << Coord_Cell(house->Center) / MAP_CELL_W << "]},\"groups\":[";
    for (int group = 0; group < 3; ++group) {
        if (group) json << ',';
        int power = 0, hp = 0;
        for (unsigned int i = 0; i < snapshot.Groups[group].size(); ++i) {
            TechnoClass const * object = As_Techno(snapshot.Groups[group][i].Target);
            power += Power(object);
            hp += Clamp((int)((long long)object->Strength * 100 / (std::max)(1, (int)object->Techno_Type_Class()->MaxStrength)), 0, 100);
        }
        json << "{\"id\":\"" << LLM::Group_Name(group) << "\",\"count\":" << snapshot.Groups[group].size()
            << ",\"power\":" << power << ",\"avg_hp_percent\":" << (snapshot.Groups[group].empty() ? 0 : hp / snapshot.Groups[group].size())
            << ",\"units\":[";
        for (unsigned int i = 0; i < snapshot.Groups[group].size(); ++i) {
            if (i) json << ',';
            LLMMember const & member = snapshot.Groups[group][i];
            TechnoClass const * object = As_Techno(member.Target);
            int cell = Coord_Cell(object->Center_Coord());
            json << "{\"id\":"; LLM_ID(json, member);
            json << ",\"type\":" << LLM_Quoted(object->Techno_Type_Class()->IniName)
                << ",\"cell\":[" << cell % MAP_CELL_W << ',' << cell / MAP_CELL_W << ']'
                << ",\"health\":" << object->Strength << ",\"max_health\":" << object->Techno_Type_Class()->MaxStrength
                << ",\"ammo\":" << object->Ammo << ",\"mission\":" << (int)object->Mission << '}';
        }
        json << "]}";
    }
    json << "],\"target_candidates\":[";
    // A bounded nearest-target set keeps requests and game-thread work bounded.
    std::vector<Contact const *> candidates;
    for (unsigned int i = 0; i < contacts.size(); ++i) if (!contacts[i].Air) candidates.push_back(&contacts[i]);
    int origin = Coord_Cell(house->Center);
    std::stable_sort(candidates.begin(), candidates.end(), [origin](Contact const * a, Contact const * b) {
        int ad = abs(a->Cell % MAP_CELL_W - origin % MAP_CELL_W) + abs(a->Cell / MAP_CELL_W - origin / MAP_CELL_W);
        int bd = abs(b->Cell % MAP_CELL_W - origin % MAP_CELL_W) + abs(b->Cell / MAP_CELL_W - origin / MAP_CELL_W);
        return ad < bd;
    });
    for (unsigned int i = 0; i < candidates.size() && snapshot.Targets.size() < 96; ++i) {
        Contact const & contact = *candidates[i];
        std::vector<int> reachable;
        for (int group = 0; group < 3; ++group) {
            if (!representatives[group] || (contact.Mobile && group != LLM::AIR && contact.Naval != (group == LLM::NAVAL))) continue;
            int approach = fields[group].Approach(contact.Cell, group == LLM::AIR ? 1 : group == LLM::NAVAL ? 6 : 3);
            if (approach < 0) continue;
            for (unsigned int j = 0; j < snapshot.Groups[group].size(); ++j) {
                FootClass const * object = static_cast<FootClass const *>(As_Techno(snapshot.Groups[group][j].Target));
                if (Can_Hit(object, contact)) { reachable.push_back(group); break; }
            }
        }
        if (reachable.empty()) continue;
        if (!snapshot.Targets.empty()) json << ',';
        LLMMember member = LLM_Reference(contact.Object);
        snapshot.Targets.push_back(member);
        json << "{\"id\":"; LLM_ID(json, member);
        json << ",\"owner\":" << contact.Owner << ",\"type\":" << LLM_Quoted(contact.Object->Techno_Type_Class()->IniName)
            << ",\"category\":\"" << LLM_Category(contact.Object) << "\",\"cell\":[" << contact.Cell % MAP_CELL_W << ',' << contact.Cell / MAP_CELL_W
            << "],\"health\":" << contact.Object->Strength << ",\"power\":" << contact.Power << ",\"reachable_by\":[";
        for (unsigned int j = 0; j < reachable.size(); ++j) { if (j) json << ','; json << '"' << LLM::Group_Name(reachable[j]) << '"'; }
        RouteMap const & field = fields[reachable[0]];
        json << "],\"ground_threat\":" << Nearby_Power(contacts, field, contact.Cell)
            << ",\"anti_air_threat\":" << Nearby_Power(contacts, field, contact.Cell, true) << '}';
    }
    json << "],\"active_plan\":[";
    bool comma = false;
    for (int group = 0; group < 3; ++group) {
        LLMDirective const & directive = state.Directives[group];
        if (!directive.Active) continue;
        if (comma) json << ',';
        comma = true;
        LLM::Order const & order = directive.Order;
        int members = 0;
        for (unsigned int i = 0; i < directive.Members.size(); ++i) {
            LLMMember const & member = directive.Members[i];
            if (member.Generation == LLMBridge::Object_Generation(member.Target)
                && !Contains(defenders, member.Target) && Member(member.Target, house)) ++members;
        }
        json << "{\"group_id\":\"" << LLM::Group_Name(group) << "\",\"action\":\"" << LLM::Action_Name(order.ActionID)
            << "\",\"based_on_snapshot_seq\":" << directive.SnapshotSeq << ",\"expires_at_frame\":" << directive.ExpiresAt
            << ",\"member_count\":" << members << ",\"commit_percent\":" << order.CommitPercent
            << ",\"withdraw_avg_hp_percent\":" << order.WithdrawPercent << ",\"target_id\":";
        if (LLM::Target_Action(order.ActionID)) { LLMMember member = {(TARGET)order.Target, order.Generation}; LLM_ID(json, member); }
        else json << "null";
        json << ",\"destination_cell\":";
        if (order.ActionID == LLM::DEFEND_AREA || order.ActionID == LLM::RETREAT_TO) json << '[' << order.X << ',' << order.Y << ']';
        else json << "null";
        json << '}';
    }
    json << "],\"last_plan_results\":[";
    for (unsigned int i = 0; i < state.Results.size(); ++i) {
        if (i) json << ',';
        LLMResult const & result = state.Results[i];
        json << "{\"snapshot_seq\":" << result.Seq << ",\"group_id\":";
        if (result.Group < 0) json << "null"; else json << '"' << LLM::Group_Name(result.Group) << '"';
        json << ",\"status\":" << LLM_Quoted(result.Status.c_str()) << '}';
    }
    json << "]}";
    if (LLMBridge::Publish(json.str())) {
        AILog::Record(Frame, house->Class->House, "llm_snapshot", json.str());
        state.History.push_back(snapshot);
        if (state.History.size() > 32) state.History.pop_front();
    }
}

static void LLM_Prune(StrikePlan & strike, LLMDirective const & directive)
{
    if (!strike.External) return;
    strike.Members.erase(std::remove_if(strike.Members.begin(), strike.Members.end(),
        [&directive](TARGET target) {
            for (unsigned int i = 0; i < directive.Members.size(); ++i)
                if (directive.Members[i].Target == target
                    && directive.Members[i].Generation == LLMBridge::Object_Generation(target)) return false;
            return true;
        }), strike.Members.end());
}
static void LLM_Release(HouseClass * house, TacticalState & tactical, int group, char const * status)
{
    LLMHouseState & state = LLMStates[house->Class->House];
    LLMDirective & directive = state.Directives[group];
    if (!directive.Active) return;
    LLM_Result(state, directive.SnapshotSeq, group, status);
    StrikePlan & strike = LLM_Strike(tactical, group);
    if (strike.External) {
        // Native plans use slot handles. Never let their cancellation touch a
        // replacement object that was not part of this external directive.
        LLM_Prune(strike, directive);
        Cancel(strike, house, &tactical.Defenders);
    } else if (!LLM::Target_Action(directive.Order.ActionID)) {
        for (unsigned int i = 0; i < directive.Members.size(); ++i) {
            LLMMember const & member = directive.Members[i];
            if (Contains(tactical.Defenders, member.Target)
                || member.Generation != LLMBridge::Object_Generation(member.Target)) continue;
            FootClass * unit = Member(member.Target, house);
            if (unit) Guard(unit);
        }
    }
    directive = LLMDirective();
}

static bool LLM_Apply(HouseClass * house, TacticalState & tactical, std::vector<Fighter> const & army,
    std::vector<Contact> const & contacts, std::vector<TARGET> const & defenders, LLM::Plan const & plan)
{
    LLMHouseState & state = LLMStates[house->Class->House];
    LLMSnapshot const * snapshot = NULL;
    for (unsigned int i = 0; i < state.History.size(); ++i)
        if (state.History[i].Seq == plan.SnapshotSeq) { snapshot = &state.History[i]; break; }
    if (!snapshot || !LLM::Fresh(plan, LLMMatchLow, LLMMatchHigh, house->Class->House,
        Frame, snapshot->FrameNumber, state.LastApplied)) {
        AILog::Record(Frame, house->Class->House, "llm_plan_rejected", AILog::Fields().Number("snapshot_seq", plan.SnapshotSeq)
            .Number("expires_at_frame", plan.ExpiresAt).Number("last_applied", state.LastApplied)
            .Text("reason", snapshot ? "envelope_or_ttl" : "snapshot_not_in_history"));
        return false;
    }
    // Prepare every order before mutating any unit. A bad order rejects the whole plan.
    StrikePlan prepared[3];
    LLMDirective directives[3];
    for (int i = 0; i < plan.Count; ++i) {
        LLM::Order const & order = plan.Orders[i];
        int group = order.GroupID;
        std::vector<Fighter> available;
        for (unsigned int j = 0; j < army.size(); ++j) {
            Fighter const & fighter = army[j];
            if (LLM_Group(fighter.Object) != group || Contains(defenders, fighter.Object->As_Target())) continue;
            for (unsigned int k = 0; k < snapshot->Groups[group].size(); ++k) {
                LLMMember const & member = snapshot->Groups[group][k];
                if (member.Target == fighter.Object->As_Target() && member.Generation == LLMBridge::Object_Generation(member.Target)) {
                    available.push_back(fighter); break;
                }
            }
        }
        if (available.empty()) return false;
        LLMDirective & directive = directives[group];
        directive.Active = true; directive.ExpiresAt = plan.ExpiresAt; directive.Order = order; directive.SnapshotSeq = plan.SnapshotSeq;
        directive.RetreatCell = group == LLM::NAVAL || !house->Center ? available[0].Cell : Coord_Cell(house->Center);
        if (LLM::Target_Action(order.ActionID)) {
            bool known = false;
            for (unsigned int j = 0; j < snapshot->Targets.size(); ++j)
                if (snapshot->Targets[j].Target == order.Target && snapshot->Targets[j].Generation == order.Generation) known = true;
            TechnoClass const * target = As_Techno(order.Target);
            if (!known || !Alive(target) || !Opponent(house, target->House)
                || order.Generation != LLMBridge::Object_Generation(order.Target)) return false;
            char const * category = LLM_Category(target);
            if (order.ActionID == LLM::RAID_POWER && std::strcmp(category, "power") != 0) return false;
            if (order.ActionID == LLM::HARASS_ECONOMY && std::strcmp(category, "power") != 0
                && std::strcmp(category, "refinery") != 0 && std::strcmp(category, "harvester") != 0) return false;
            StrikeIntent intent = order.ActionID == LLM::RAID_POWER ? STRIKE_POWER
                : order.ActionID == LLM::HARASS_ECONOMY ? STRIKE_ECONOMY : STRIKE_NORMAL;
            if (!Start_Strike(prepared[group], house, available, contacts, defenders, group == LLM::NAVAL,
                group == LLM::AIR, false, false, -1, intent, false, order.Target, order.CommitPercent)) return false;
            prepared[group].External = true;
            directive.RetreatCell = prepared[group].Home;
            for (unsigned int j = 0; j < prepared[group].Members.size(); ++j) {
                LLMMember member = {prepared[group].Members[j], LLMBridge::Object_Generation(prepared[group].Members[j])};
                directive.Members.push_back(member);
            }
        } else {
            if (order.ActionID != LLM::HOLD) {
                CELL destination = (CELL)(order.Y * MAP_CELL_W + order.X);
                if (!Map.In_Radar(destination)) return false;
                RouteMap field(MAP_CELL_W, MAP_CELL_H);
                Fill_Routes(field, available[0].Object, available[0].Cell);
                int approach = field.Approach(destination, 2);
                if (approach < 0) return false;
                directive.Order.X = approach % MAP_CELL_W;
                directive.Order.Y = approach / MAP_CELL_W;
            }
            int total = 0, committed = 0;
            for (unsigned int j = 0; j < available.size(); ++j) total += available[j].Power;
            for (unsigned int j = 0; j < available.size(); ++j) {
                if (order.ActionID != LLM::HOLD && !directive.Members.empty()
                    && (long long)committed * 100 >= (long long)total * order.CommitPercent) break;
                directive.Members.push_back(LLM_Reference(available[j].Object));
                committed += available[j].Power;
            }
        }
    }
    for (int group = 0; group < 3; ++group) {
        StrikePlan & strike = LLM_Strike(tactical, group);
        if (state.Directives[group].Active) LLM_Release(house, tactical, group, "superseded");
        if (directives[group].Active) {
            if (strike.Phase != STRIKE_HOLD) Cancel(strike, house, &tactical.Defenders);
            state.Directives[group] = directives[group];
            if (prepared[group].External) strike = prepared[group];
            LLM_Result(state, plan.SnapshotSeq, group, "accepted");
        }
    }
    state.LastApplied = plan.SnapshotSeq;
    std::string orders = "[";
    for (int i = 0; i < plan.Count; ++i) {
        LLM::Order const & order = plan.Orders[i];
        if (i) orders += ',';
        orders += AILog::Fields().Text("action", LLM::Action_Name(order.ActionID)).Text("group", LLM::Group_Name(order.GroupID))
            .Number("target", order.Target).Number("generation", order.Generation).Number("x", order.X).Number("y", order.Y)
            .Number("commit_percent", order.CommitPercent).Number("withdraw_percent", order.WithdrawPercent).Json();
    }
    AILog::Record(Frame, house->Class->House, "llm_plan_applied", AILog::Fields().Number("snapshot_seq", plan.SnapshotSeq)
        .Number("expires_at_frame", plan.ExpiresAt).Raw("orders", orders + ']'));
    if (!plan.Count) LLM_Result(state, plan.SnapshotSeq, -1, "native_control_restored");
    return true;
}

static void LLM_Tick(HouseClass * house, TacticalState & tactical, std::vector<Fighter> const & army,
    std::vector<Contact> const & contacts, std::vector<TARGET> const & defenders)
{
    LLMHouseState & state = LLMStates[house->Class->House];
    bool connected = LLMBridge::Can_Control(house->Class->House);
    for (int group = 0; group < 3; ++group) {
        if (!connected || Frame >= state.Directives[group].ExpiresAt)
            LLM_Release(house, tactical, group, connected ? "expired" : "bridge_disconnected");
    }
    if (!connected) return;
    LLM::Plan plan = {};
    bool invalid = false;
    if (LLMBridge::Receive(plan, invalid) && (invalid || !LLM_Apply(house, tactical, army, contacts, defenders, plan)))
        LLM_Result(state, invalid ? 0 : plan.SnapshotSeq, -1, "rejected_stale_invalid_or_unsafe");
    for (int group = 0; group < 3; ++group) {
        LLMDirective & directive = state.Directives[group];
        if (!directive.Active) continue;
        LLM::Order & order = directive.Order;
        StrikePlan & strike = LLM_Strike(tactical, group);
        LLM_Prune(strike, directive);
        if (LLM::Target_Action(order.ActionID)) {
            TechnoClass const * target = As_Techno(order.Target);
            if (!Alive(target)) { LLM_Release(house, tactical, group, "target_removed"); continue; }
            if (LLMBridge::Object_Generation(order.Target) != order.Generation || !Opponent(house, target->House)) {
                LLM_Release(house, tactical, group, "target_instance_or_owner_changed"); continue;
            }
            if (!strike.External || strike.Phase == STRIKE_HOLD) {
                LLM_Release(house, tactical, group, "interrupted_native_safety"); continue;
            }
        }
        std::vector<Fighter> members;
        int hp = 0;
        for (unsigned int i = 0; i < directive.Members.size(); ++i) {
            LLMMember const & member = directive.Members[i];
            if (member.Generation != LLMBridge::Object_Generation(member.Target)) continue;
            FootClass * unit = Member(member.Target, house);
            if (!unit) continue;
            if (Contains(defenders, member.Target)) {
                if (!directive.DefenseReported) {
                    LLM_Result(state, directive.SnapshotSeq, group, "preempted_for_defense"); directive.DefenseReported = true;
                }
                continue;
            }
            Fighter fighter = {unit, Coord_Cell(unit->Center_Coord()), Power(unit), group == LLM::NAVAL, group == LLM::AIR};
            members.push_back(fighter);
            hp += Clamp((int)((long long)unit->Strength * 100 / (std::max)(1, (int)unit->Techno_Type_Class()->MaxStrength)), 0, 100);
        }
        if (members.empty()) { LLM_Release(house, tactical, group, "no_available_members"); continue; }
        if (order.ActionID != LLM::HOLD && order.ActionID != LLM::RETREAT_TO
            && hp / (int)members.size() < order.WithdrawPercent) {
            int cell = directive.RetreatCell;
            if (strike.External) Cancel(strike, house, &tactical.Defenders);
            order.ActionID = LLM::RETREAT_TO;
            order.X = cell % MAP_CELL_W; order.Y = cell / MAP_CELL_W;
            LLM_Result(state, directive.SnapshotSeq, group, "withdrawing_low_health");
        }
        if (LLM::Target_Action(order.ActionID)) continue; // The native strike updater executes it.
        TARGET battle = TARGET_NONE;
        if (order.ActionID != LLM::RETREAT_TO && Fight_Close_Contacts(members, contacts, NULL, false, battle)) continue;
        for (unsigned int i = 0; i < members.size(); ++i) {
            FootClass * unit = members[i].Object;
            if (order.ActionID == LLM::HOLD) Guard(unit);
            else {
                int destination = order.Y * MAP_CELL_W + order.X;
                if (!Map.In_Radar((CELL)destination)) { LLM_Release(house, tactical, group, "invalidated_destination"); break; }
                if (unit->Distance(Cell_Coord((CELL)destination)) <= CELL_LEPTON_W * 3) Guard(unit);
                else Move(unit, destination);
            }
        }
    }
    LLM_Publish(house, army, contacts, defenders);
}
