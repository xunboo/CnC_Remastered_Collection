// Copyright 2026. Distributed under the project's GPLv3 license.
#ifndef AIEXPANSION_H
#define AIEXPANSION_H
class UnitClass;
class HouseClass;
namespace AIExpansion {
void Reset(void);
int Base_Limit(HouseClass const * house);
void Forget(UnitClass const * unit);
void Kennel_Sold(HouseClass const * house);
bool Controls(UnitClass const * unit);
// Distances and intervals use cells and simulation ticks, not wall time.
enum { RemoteRoute = 16, LocalOreRoute = 8, ScanSeconds = 10,
    DeployCheckSeconds = 5, StalledSeconds = 20, ExpansionMinutes = 2,
    LateMinutes = 5, CoreMinimum = 2,
    IncomeBuckets = 6, IncomeBucketSeconds = 10, StorageForecastSeconds = 20 };
}
#endif
