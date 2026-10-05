// Copyright 2026. Distributed under the project's GPLv3 license.
#ifndef AISTRATEGY_H
#define AISTRATEGY_H

// Deterministic production policy. Random selection remains in the game, so
// multiplayer clients use the same simulation RNG and make the same decisions.
namespace AIStrategy {

struct EnemyForces {
	int Armor;
	int Infantry;
	int Aircraft;
	int Vessels;
	int Defenses;
	int AntiAirDefenses;

	EnemyForces() : Armor(0), Infantry(0), Aircraft(0), Vessels(0), Defenses(0), AntiAirDefenses(0) {}
};

enum UnitRole {
	ROLE_ARMOR,
	ROLE_ANTI_INFANTRY,
	ROLE_SIEGE,
	ROLE_SUPPORT,
	ROLE_SPECIAL
};

inline int Combat_Unit_Count(int total, int harvesters, int mcvs)
{
	int count = total - harvesters - mcvs;
	return count > 0 ? count : 0;
}

inline bool Armor_Pressure(EnemyForces const & enemy)
{
	return enemy.Armor >= 6 && (long long)enemy.Armor * 2 >= enemy.Infantry;
}

inline bool Expansion_Allowed(int combat_units, EnemyForces const & enemy)
{
	// Recovery of the last construction yard is handled separately by the game.
	return (long long)combat_units * 4 >= (long long)enemy.Armor * 4
		+ enemy.Infantry / 2 + (long long)enemy.Aircraft * 2;
}

inline bool Infantry_Spending_Allowed(EnemyForces const & enemy, int factories,
	int infantry, int combat_units, int money)
{
	if (factories <= 0 || !Armor_Pressure(enemy)) return true;
	// Rockets and Shock Troopers remain escorts; save factory money for armor.
	return infantry < 6 + combat_units / 3 && (infantry < 3 || money >= 1800);
}

inline int Production_Weight(UnitRole role, int base, int owned, int total,
	EnemyForces const & enemy, bool anti_air = false)
{
	if (base <= 0) return 0;
	// Support and explosive units must not displace the main fighting force.
	if (role == ROLE_SUPPORT && owned >= 1 + total / 12) return 0;
	if (role == ROLE_SPECIAL && owned >= 1 + total / 16) return 0;

	int weight = base;
	if (role == ROLE_ARMOR && enemy.Armor > 0) weight += 16;
	if (Armor_Pressure(enemy)) {
		if (role == ROLE_ARMOR) weight += 64;
		if (role == ROLE_ANTI_INFANTRY || role == ROLE_SPECIAL) weight = (weight + 3) / 4;
	}
	if (role == ROLE_ANTI_INFANTRY && (long long)enemy.Infantry > (long long)enemy.Armor * 2) weight += 20;
	if (role == ROLE_SIEGE) {
		if (enemy.Defenses > 0) weight += 16;
		if (owned * 4 >= total + 4) weight = (weight + 3) / 4;
	}
	if (anti_air && enemy.Aircraft > 0) weight += 24;
	return weight;
}

inline bool Strategy_Available(int mode, bool naval, bool air, bool infantry, bool armor, bool ground)
{
	switch (mode) {
	case 1: case 7: return true;
	case 2: case 3: return naval;
	case 4: return air;
	case 5: return infantry && ground;
	case 6: return armor && ground;
	default: return false;
	}
}

inline int Naval_Production_Limit(int maximum, int enemy_average, int multiplier, int bonus, bool proactive)
{
	if (maximum <= 0) return 0;
	if (proactive) return maximum;
	if (enemy_average < 0) enemy_average = 0;
	if (multiplier < 0) multiplier = 0;
	if (bonus < 0) bonus = 0;
	// Use a wide intermediate so malformed INI values cannot overflow the cap.
	long long desired = (long long)enemy_average * multiplier + bonus;
	return desired < maximum ? (int)desired : maximum;
}

struct InfantryBudget {
	int Reserve;
	int BaseMultiplier;
};

inline InfantryBudget Infantry_Budget(bool naval, bool ground, int factories, int strategy)
{
	InfantryBudget budget;
	if (strategy == 5) {
		budget.Reserve = 100;
		budget.BaseMultiplier = 5000;
	} else if (naval) {
		budget.Reserve = ground ? 12000 : 999000;
		budget.BaseMultiplier = ground ? 3 : 1;
	} else if (factories > 1) {
		budget.Reserve = 12000;
		budget.BaseMultiplier = 4;
	} else {
		budget.Reserve = 100;
		budget.BaseMultiplier = 1000;
	}
	return budget;
}

} // namespace AIStrategy
#endif
