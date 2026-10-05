#include "../REDALERT/AISTRATEGY.H"
#include <climits>
#include <cstdlib>
#include <iostream>

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
    using namespace AIStrategy;
    check(Combat_Unit_Count(53, 10, 3) == 40, "harvesters and MCVs do not occupy the combat cap");
    check(Combat_Unit_Count(1, 2, 1) == 0, "transitional unit counts cannot create a negative army");
    check(!Strategy_Available(3, false, true, true, true, true), "landlocked bases cannot choose naval rush");
    check(!Strategy_Available(2, false, true, true, true, true), "proactive naval strategy requires local naval access");
    check(!Strategy_Available(4, false, false, true, true, true), "low-tech games cannot choose air rush");
    check(!Strategy_Available(6, false, false, true, false, true), "low-tech games cannot choose tank rush");
    check(!Strategy_Available(5, true, true, true, true, false), "pure naval wars exclude infantry-only rush");
    check(Strategy_Available(6, true, true, true, true, true), "mixed wars retain tank strategies");
    check(Strategy_Available(1, false, false, false, false, false), "a dynamic fallback always exists");
    check(!Strategy_Available(99, true, true, true, true, true), "invalid configured strategies are rejected");

    check(Naval_Production_Limit(50, 0, 1, 5, false) == 5, "reactive AI can establish a fleet before the human builds ships");
    check(Naval_Production_Limit(50, 0, 1, 5, true) == 50, "proactive naval strategy can pressure an opponent without a fleet");
    check(Naval_Production_Limit(50, 20, 2, 5, false) == 45, "naval multiplier and bonus are applied");
    check(Naval_Production_Limit(50, 100, 2, 5, false) == 50, "naval production respects the configured hard cap");
    check(Naval_Production_Limit(0, 100, 2, 5, true) == 0, "disabled naval production remains disabled");
    check(Naval_Production_Limit(50, INT_MAX, INT_MAX, INT_MAX, false) == 50, "large INI values cannot overflow the fleet cap");
    check(Naval_Production_Limit(50, -1, -1, -1, false) == 0, "negative INI values cannot create negative fleet limits");

    EnemyForces armor;
    armor.Armor = 20;
    EnemyForces infantry;
    infantry.Infantry = 40;
    EnemyForces air;
    air.Aircraft = 6;
    EnemyForces fortified;
    fortified.Defenses = 10;
    EnemyForces empty;
    check(Production_Weight(ROLE_ARMOR, 24, 0, 20, armor) > Production_Weight(ROLE_ANTI_INFANTRY, 8, 0, 20, armor), "tank armies favor armor over anti-infantry vehicles");
    check(Production_Weight(ROLE_ANTI_INFANTRY, 8, 0, 20, infantry) > Production_Weight(ROLE_ANTI_INFANTRY, 8, 0, 20, empty), "infantry armies increase anti-infantry production");
    check(Production_Weight(ROLE_ARMOR, 24, 0, 20, air, true) > Production_Weight(ROLE_ARMOR, 24, 0, 20, air), "air attacks increase production of units able to shoot aircraft");
    check(Production_Weight(ROLE_SIEGE, 8, 0, 20, fortified) > Production_Weight(ROLE_SIEGE, 8, 0, 20, empty), "fortified bases increase siege production");
    check(Production_Weight(ROLE_SIEGE, 8, 6, 20, fortified) < Production_Weight(ROLE_SIEGE, 8, 0, 20, fortified), "siege weapons retain a fighting escort");
    check(Production_Weight(ROLE_SUPPORT, 2, 1, 0, empty) == 0, "support vehicles cannot displace a new fighting force");
    check(Production_Weight(ROLE_SPECIAL, 4, 2, 16, empty) == 0, "special explosive vehicles stay a small part of the army");

    check(Armor_Pressure(armor), "massed armor activates a heavy-weapons response");
    check(!Armor_Pressure(infantry), "infantry-heavy opposition keeps anti-infantry production available");
    check(!Infantry_Spending_Allowed(armor, 1, 20, 15, 10000), "massed enemy tanks cap infantry escorts even with abundant cash");
    check(!Infantry_Spending_Allowed(armor, 1, 5, 10, 1000), "infantry does not spend the last factory money against armor");
    check(Infantry_Spending_Allowed(armor, 0, 20, 15, 1000), "games without usable tank factories retain infantry production");
    check(Infantry_Spending_Allowed(armor, 1, 2, 0, 1000), "a small initial infantry screen remains affordable");
    check(!Expansion_Allowed(5, armor), "inferior armor strength postpones optional MCV expansion");
    check(Expansion_Allowed(25, armor), "a strong army can afford to expand its economy");

    InfantryBudget naval = Infantry_Budget(true, false, 0, 3);
    InfantryBudget rush = Infantry_Budget(false, true, 0, 5);
    InfantryBudget mixed = Infantry_Budget(true, true, 3, 2);
    InfantryBudget ground = Infantry_Budget(false, true, 3, 1);
    check(naval.Reserve == 999000 && naval.BaseMultiplier == 1, "pure naval houses conserve infantry spending");
    check(rush.Reserve == 100 && rush.BaseMultiplier == 5000, "infantry rush keeps its own production budget");
    check(mixed.Reserve == 12000 && mixed.BaseMultiplier == 3, "mixed strategy keeps a balanced infantry budget");
    check(ground.BaseMultiplier == 4, "developed ground bases keep infantry support");
    check(naval.Reserve == 999000, "calculating another house's budget does not change a naval house");
    std::cout << checks << " AI policy scenarios passed.\n";
}
