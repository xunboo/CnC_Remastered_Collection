// Copyright 2026. Distributed under the project's GPLv3 license.
#ifndef AIEXPANSION_H
#define AIEXPANSION_H
class UnitClass;
namespace AIExpansion {
void Reset(void);
void Forget(UnitClass const * unit);
bool Controls(UnitClass const * unit);
// Distances and intervals use cells and simulation ticks, not wall time.
enum { RemoteRoute = 16, LocalOreRoute = 8, ScanSeconds = 10,
    DeployCheckSeconds = 5, StalledSeconds = 20, ExpansionMinutes = 2 };
}
#endif
