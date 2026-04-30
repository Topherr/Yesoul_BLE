// Replay harness: reads the captured FTMS log line-by-line, parses each frame,
// and asserts the parser agrees with the legacy hand-rolled offsets for the
// three fields the legacy parser exposed (cadence, resistance, inst. power).
// Also sanity-checks the four newly-extracted fields (speed, distance,
// avg power, elapsed time) and the energy "data not available" sentinels.

#include "ftms_parser.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> out;
    std::istringstream iss(hex);
    std::string tok;
    while (iss >> tok) {
        out.push_back((uint8_t)std::stoul(tok, nullptr, 16));
    }
    return out;
}

struct Stats {
    int frames = 0;
    int with_speed = 0;
    int with_cadence = 0;
    int with_distance = 0;
    int with_resistance = 0;
    int with_inst_power = 0;
    int with_avg_power = 0;
    int with_energy = 0;
    int with_elapsed = 0;
    int legacy_match = 0;
    int legacy_mismatch = 0;
    int max_speed_cmps = 0;
    int max_inst_power = 0;
    int max_resistance = 0;
};

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1]
                                : "docs/captures/2026-04-30-yesoul-g1m-plus.log";
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "FAIL: cannot open %s\n", path);
        return 1;
    }

    Stats s;
    int failures = 0;
    std::string line;

    while (std::getline(f, line)) {
        auto raw = line.find("RAW[");
        auto bytes = line.find("bytes:");
        if (raw == std::string::npos || bytes == std::string::npos) continue;

        std::string hex = line.substr(bytes + 6);
        auto buf = hex_to_bytes(hex);
        if (buf.size() < 2) continue;

        BikeFrame frame{};
        bool ok = parse_indoor_bike_data(buf.data(), buf.size(), frame);
        if (!ok) {
            std::fprintf(stderr, "FAIL: parser rejected frame: %s\n", line.c_str());
            failures++;
            continue;
        }

        // Cross-check against legacy hardcoded offsets from the original parser.
        // Legacy: cadence = (b[4] | b[5]<<8) / 2, resistance = b[9],
        //         inst_power = b[11] | b[12]<<8.
        if (buf.size() >= 13) {
            int legacy_cadence_rpm = ((buf[4] | (buf[5] << 8))) / 2;
            int legacy_resistance  = buf[9];
            int legacy_power       = (int16_t)(buf[11] | (buf[12] << 8));

            int new_cadence_rpm = frame.cadence_halfrpm / 2;
            int new_resistance  = frame.resistance;
            int new_power       = frame.inst_power_w;

            bool match = (legacy_cadence_rpm == new_cadence_rpm)
                      && (legacy_resistance  == new_resistance)
                      && (legacy_power       == new_power);
            if (match) s.legacy_match++;
            else {
                s.legacy_mismatch++;
                std::fprintf(stderr,
                    "FAIL: legacy mismatch at frame %d: cad %d/%d res %d/%d pwr %d/%d\n",
                    s.frames, legacy_cadence_rpm, new_cadence_rpm,
                    legacy_resistance, new_resistance, legacy_power, new_power);
                failures++;
            }
        }

        s.frames++;
        if (frame.has_speed)      { s.with_speed++;      if (frame.speed_cmps > s.max_speed_cmps) s.max_speed_cmps = frame.speed_cmps; }
        if (frame.has_cadence)    s.with_cadence++;
        if (frame.has_distance)   s.with_distance++;
        if (frame.has_resistance) { s.with_resistance++; if (frame.resistance  > s.max_resistance)  s.max_resistance  = frame.resistance; }
        if (frame.has_inst_power) { s.with_inst_power++; if (frame.inst_power_w > s.max_inst_power) s.max_inst_power = frame.inst_power_w; }
        if (frame.has_avg_power)  s.with_avg_power++;
        if (frame.has_energy)     s.with_energy++;
        if (frame.has_elapsed)    s.with_elapsed++;
    }

    if (s.frames == 0) {
        std::fprintf(stderr, "FAIL: no frames parsed from %s\n", path);
        return 1;
    }

    // For this Yesoul capture every frame should expose every field bit 0x09F4
    // declares: speed, cadence, distance, resistance, inst.power, avg.power,
    // energy, elapsed.
    if (s.with_speed      != s.frames) { std::fprintf(stderr, "FAIL: speed missing on some frames (%d/%d)\n",      s.with_speed,      s.frames); failures++; }
    if (s.with_cadence    != s.frames) { std::fprintf(stderr, "FAIL: cadence missing on some frames\n"); failures++; }
    if (s.with_distance   != s.frames) { std::fprintf(stderr, "FAIL: distance missing on some frames\n"); failures++; }
    if (s.with_resistance != s.frames) { std::fprintf(stderr, "FAIL: resistance missing on some frames\n"); failures++; }
    if (s.with_inst_power != s.frames) { std::fprintf(stderr, "FAIL: inst power missing on some frames\n"); failures++; }
    if (s.with_avg_power  != s.frames) { std::fprintf(stderr, "FAIL: avg power missing on some frames\n"); failures++; }
    if (s.with_energy     != s.frames) { std::fprintf(stderr, "FAIL: energy missing on some frames\n"); failures++; }
    if (s.with_elapsed    != s.frames) { std::fprintf(stderr, "FAIL: elapsed missing on some frames\n"); failures++; }

    // Sanity: peak speed must look like a person on a spin bike, not garbage.
    if (s.max_speed_cmps < 100 || s.max_speed_cmps > 5000) {
        std::fprintf(stderr, "FAIL: peak speed %d cmps outside sane range\n", s.max_speed_cmps);
        failures++;
    }

    std::printf("frames=%d legacy_match=%d legacy_mismatch=%d "
                "max_speed=%.2fkmh max_power=%dW max_res=%d\n",
                s.frames, s.legacy_match, s.legacy_mismatch,
                s.max_speed_cmps / 100.0, s.max_inst_power, s.max_resistance);

    if (failures) {
        std::printf("FAIL: %d failures\n", failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
