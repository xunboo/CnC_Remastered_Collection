// Copyright 2026. Distributed under the project's GPLv3 license.
#ifndef AITACTICS_H
#define AITACTICS_H

#include <vector>
#include <queue>

// Integer-only policy and route search shared by the game and regression tests.
// Tactical plans are transient; no fields are added to saved game objects.
namespace AITactics {

inline int Clamp(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

inline int Combat_Power(int cost, int health, int maximum)
{
    if (health <= 0 || maximum <= 0) return 0;
    return (int)((long long)Clamp(cost, 50, 10000) * Clamp(health, 0, maximum) / maximum);
}

inline bool Economy_Ready(int refineries, int harvesters, bool ore_exhausted)
{
    return ore_exhausted || (refineries > 0 && harvesters > 0);
}

inline bool Can_Launch(int count, int strike, int enemy_mobile, int local_defense)
{
    return count >= 5 && strike >= 2000
        && (long long)strike * 100 >= (long long)enemy_mobile * 115
        && (long long)strike * 100 >= (long long)local_defense * 150;
}

inline bool Should_Withdraw(int strike, int enemy_mobile, int nearby_enemy)
{
    return strike <= 0 || (long long)strike * 100 < (long long)enemy_mobile * 100
        || (long long)strike * 100 < (long long)nearby_enemy * 115;
}

inline bool Cohesion_Ready(int count, int ready_count, int power, int ready_power)
{
    return count > 0 && power > 0 && (long long)ready_count * 100 >= (long long)count * 75
        && (long long)ready_power * 100 >= (long long)power * 80;
}

inline bool Defense_Sufficient(int defenders, int threat)
{
    return threat > 0 && (long long)defenders * 100 >= (long long)threat * 125;
}

inline int Weak_Point_Score(int value, int defenders, int route_length)
{
    // Towers and nearby mobile reinforcements dominate the value of a target.
    long long score = ((long long)Clamp(value, 0, 10000) + 500) * 4096
        / ((long long)Clamp(defenders, 0, 100000000) + 500);
    score -= (long long)Clamp(route_length, 0, 16384) * 16;
    return (int)score;
}

class RouteMap {
public:
    int Width;
    int Height;
    std::vector<unsigned char> Clear;
    std::vector<int> Distance;
    std::vector<int> Previous;
    std::vector<int> Danger;

    RouteMap(int width, int height) : Width(width), Height(height),
        Clear(width * height, 0), Distance(width * height, -1), Previous(width * height, -1), Danger(width * height, 0) {}

    bool Valid(int cell) const { return cell >= 0 && cell < Width * Height; }
    int Separation(int a, int b) const
    {
        if (!Valid(a) || !Valid(b)) return Width + Height;
        int dx = a % Width - b % Width;
        int dy = a / Width - b / Width;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return dx > dy ? dx : dy;
    }

    void Search(int source)
    {
        Distance.assign(Width * Height, -1);
        Previous.assign(Width * Height, -1);
        if (!Valid(source) || !Clear[source]) return;
        static int const dx[] = {0, 1, 1, 1, 0, -1, -1, -1};
        static int const dy[] = {-1, -1, 0, 1, 1, 1, 0, -1};
        struct Node {
            int Cell;
            int Cost;
            bool operator<(Node const & other) const
            {
                return Cost > other.Cost || (Cost == other.Cost && Cell > other.Cell);
            }
        };
        std::priority_queue<Node> queue;
        Node first = {source, 0};
        queue.push(first);
        Distance[source] = 0;
        while (!queue.empty()) {
            Node current = queue.top(); queue.pop();
            int cell = current.Cell;
            if (current.Cost != Distance[cell]) continue;
            int x = cell % Width, y = cell / Width;
            for (int direction = 0; direction < 8; ++direction) {
                int nx = x + dx[direction], ny = y + dy[direction];
                if (nx < 0 || nx >= Width || ny < 0 || ny >= Height) continue;
                int next = ny * Width + nx;
                if (!Clear[next]) continue;
                if (dx[direction] && dy[direction]
                    && (!Clear[y * Width + nx] || !Clear[ny * Width + x])) continue;
                int cost = current.Cost + 1 + Clamp(Danger[next], 0, 255);
                if (Distance[next] >= 0 && cost >= Distance[next]) continue;
                Distance[next] = cost;
                Previous[next] = cell;
                Node node = {next, cost}; queue.push(node);
            }
        }
    }

    int Approach(int target, int radius) const
    {
        if (!Valid(target)) return -1;
        int best = -1, best_score = 0;
        int tx = target % Width, ty = target / Width;
        for (int y = Clamp(ty - radius, 0, Height - 1); y <= Clamp(ty + radius, 0, Height - 1); ++y) {
            for (int x = Clamp(tx - radius, 0, Width - 1); x <= Clamp(tx + radius, 0, Width - 1); ++x) {
                int cell = y * Width + x;
                if (Distance[cell] < 0 || !Clear[cell]) continue;
                int score = Separation(cell, target) * 32 + Distance[cell];
                if (best < 0 || score < best_score) { best = cell; best_score = score; }
            }
        }
        return best;
    }

    std::vector<int> Path(int destination) const
    {
        std::vector<int> reverse, path;
        if (!Valid(destination) || Distance[destination] < 0) return path;
        for (int cell = destination; cell >= 0; cell = Previous[cell]) {
            reverse.push_back(cell);
            if (reverse.size() > Clear.size()) return path;
        }
        for (int i = (int)reverse.size() - 1; i >= 0; --i) path.push_back(reverse[i]);
        return path;
    }
};

} // namespace AITactics
#endif
