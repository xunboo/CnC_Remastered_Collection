// Copyright 2026. Distributed under the project's GPLv3 license.
#ifndef HARVESTAI_H
#define HARVESTAI_H
#include <vector>

class UnitClass;
namespace HarvestAI {
void Reset(void);
void Forget(UnitClass const * unit);

inline int Clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}
inline int Nearby_Band(int distance)
{
    return Clamp(distance / 5, 2, 4);
}
inline int Ore_Score(int value, int surrounding_value, int reservations)
{
    return (Clamp(value, 0, 10000000) + Clamp(surrounding_value, 0, 10000000) / 4)
        / (1 + Clamp(reservations, 0, 64) * 2);
}
inline int Refinery_Score(int distance, int queue, int wait)
{
    return Clamp(distance, 0, 16384) + Clamp(queue, 0, 4096) * Clamp(wait, 1, 64);
}

// Unweighted shortest ground routes. Moving units do not make a whole ore
// field unreachable; destination reservations handle their congestion instead.
class TravelMap {
public:
    int Width, Height;
    std::vector<unsigned char> Clear;
    std::vector<int> Distance;
    TravelMap(int width, int height) : Width(width), Height(height),
        Clear(width * height, 0), Distance(width * height, -1) {}
    bool Valid(int cell) const { return cell >= 0 && cell < Width * Height; }
    int Separation(int a, int b) const
    {
        if (!Valid(a) || !Valid(b)) return Width + Height;
        int dx = a % Width - b % Width, dy = a / Width - b / Width;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return dx > dy ? dx : dy;
    }
    void Search(int source)
    {
        Search(std::vector<int>(1, source));
    }
    void Search(std::vector<int> const & sources)
    {
        Distance.assign(Width * Height, -1);
        std::vector<int> queue;
        queue.reserve(Width * Height);
        for (unsigned int i = 0; i < sources.size(); ++i) {
            int source = sources[i];
            if (!Valid(source) || !Clear[source] || Distance[source] >= 0) continue;
            queue.push_back(source);
            Distance[source] = 0;
        }
        static int const dx[] = {0, 1, 1, 1, 0, -1, -1, -1};
        static int const dy[] = {-1, -1, 0, 1, 1, 1, 0, -1};
        for (unsigned int i = 0; i < queue.size(); ++i) {
            int cell = queue[i], x = cell % Width, y = cell / Width;
            for (int direction = 0; direction < 8; ++direction) {
                int nx = x + dx[direction], ny = y + dy[direction];
                if (nx < 0 || nx >= Width || ny < 0 || ny >= Height) continue;
                int next = ny * Width + nx;
                if (!Clear[next] || Distance[next] >= 0) continue;
                if (dx[direction] && dy[direction]
                    && (!Clear[y * Width + nx] || !Clear[ny * Width + x])) continue;
                Distance[next] = Distance[cell] + 1;
                queue.push_back(next);
            }
        }
    }
};
}
#endif
