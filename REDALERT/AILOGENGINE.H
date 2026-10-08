// Copyright 2026. Distributed under the project's GPLv3 license.
#ifndef AILOGENGINE_H
#define AILOGENGINE_H
#include "AILOG.H"
class HouseClass;
class TechnoClass;
class MissionClass;
namespace AILog {
void Simulation_Tick();
void Object_Order(MissionClass const * object, char const * event, unsigned int requested);
void House_Event(HouseClass const * house, char const * event, Fields const & details);
class HouseScope {
    HouseClass const * House;
    char const * Policy;
    std::string Before;
public:
    HouseScope(HouseClass const * house, char const * policy);
    ~HouseScope();
};
class MissionScope {
    unsigned int Target, Generation;
    int House;
    std::string Before;
public:
    explicit MissionScope(MissionClass const * object);
    ~MissionScope();
};
}
#endif
