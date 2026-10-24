// Replays a recorded PINHOLE_INPUT_LOG through ScrollPhysics at 120 Hz and
// reports, per gesture, how far the fingers moved the content, the speed it
// was thrown at, and how far and long it coasted.
//
//   g++ -std=c++20 -O2 -Isrc tests/scroll_replay.cpp src/scrollphysics.cpp -o build/scroll_replay
//   build/scroll_replay input.log [start-position]

#include "scrollphysics.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

struct Event {
    double t;
    int phase;
    double dy;
};

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: scroll_replay input.log [start]\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    const double start = argc > 2 ? std::atof(argv[2]) : 500000;
    std::vector<Event> events;
    std::regex phaseRe(R"(phase=(\d+))"), pixelRe(R"(pixel=(-?\d+),(-?\d+))"), tsRe(R"(ts=(\d+))");
    std::string line;
    std::smatch m;
    while (std::getline(in, line)) {
        if (line.find(" wheel ") == std::string::npos)
            continue;
        Event e{};
        if (std::regex_search(line, m, tsRe))
            e.t = std::stod(m[1]);
        if (std::regex_search(line, m, phaseRe))
            e.phase = std::stoi(m[1]);
        if (std::regex_search(line, m, pixelRe))
            e.dy = -std::stod(m[2]); // as PhotoGrid: content direction
        // Begin and end arrive twice; the second is a no-op for the physics,
        // but skip it here to keep the report tidy.
        if (!events.empty() && events.back().t == e.t && events.back().phase == e.phase && e.phase != 2)
            continue;
        events.push_back(e);
    }

    ScrollPhysics physics;
    physics.setBounds(0, 1e9, 1058);
    physics.jumpTo(start);
    const double frame = 1000.0 / 120;
    double now = events.empty() ? 0 : events.front().t;
    int gesture = 0;
    double atBegin = 0, atEnd = 0, coastStart = 0, maxOver = 0;
    bool coasting = false;

    auto advanceTo = [&](double t) {
        while (now + frame <= t) {
            now += frame;
            const bool was = physics.animating();
            physics.step(frame / 1000);
            maxOver = std::max(maxOver, std::max(0.0, -physics.position()));
            if (coasting && was && !physics.animating()) {
                std::printf("      coasted %7.0f px in %4.0f ms, overscroll max %3.0f px\n",
                            physics.position() - atEnd, now - coastStart, maxOver);
                coasting = false;
            }
        }
    };

    for (const Event &e : events) {
        advanceTo(e.t);
        if (e.phase == 1) {
            if (coasting) {
                std::printf("      caught after %7.0f px, %4.0f ms\n", physics.position() - atEnd, e.t - coastStart);
                coasting = false;
            }
            physics.begin(e.t);
            atBegin = physics.position();
            maxOver = 0;
        } else if (e.phase == 2) {
            physics.move(e.t, e.dy);
        } else if (e.phase == 3) {
            physics.end(e.t);
            atEnd = physics.position();
            std::printf("gesture %2d: fingers moved it %7.0f px, thrown at %7.0f px/s\n", gesture++, atEnd - atBegin,
                        physics.velocity());
            coasting = physics.animating();
            coastStart = e.t;
        }
    }
    advanceTo(now + 5000);
    return 0;
}
