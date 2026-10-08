// Copyright 2026. Distributed under the project's GPLv3 license.
#ifndef AILOG_H
#define AILOG_H
#include <string>

namespace AILog {
std::string Quote(char const * text);
class Fields {
    std::string Body;
    void Key(char const * name);
public:
    Fields & Number(char const * name, long long value);
    Fields & Text(char const * name, char const * value);
    Fields & Boolean(char const * name, bool value);
    Fields & Raw(char const * name, std::string const & json);
    std::string Json() const { return "{" + Body + "}"; }
};
// Prepare closes the previous session. Begin opens only when simulation starts.
void Prepare(unsigned int low, unsigned int high, int frame, char const * reason);
void Start_Reason(char const * reason);
void Begin(int frame, Fields const & context);
void End(int frame, char const * reason);
bool Enabled();
int Current_Frame();
std::string Path();
void Record(int frame, int house, char const * event, std::string const & data);
inline void Record(int frame, int house, char const * event, Fields const & data) {
    Record(frame, house, event, data.Json());
}
// Report evaluations including early returns and their final selected value.
template<class T> class Choice {
    int Frame, House;
    char const * Event;
    T const * Value;
    long long Before;
    Fields Data;
public:
    Choice(int frame, int house, char const * event, T const * value, Fields const & data)
        : Frame(frame), House(house), Event(event), Value(value), Before(static_cast<long long>(*value)), Data(data) {}
    ~Choice() {
        if (Enabled() && House >= 0) Record(Frame, House, Event, Data.Number("before", Before).Number("selected", static_cast<long long>(*Value)));
    }
};
}
#endif
