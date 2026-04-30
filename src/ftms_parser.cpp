#include "ftms_parser.h"
#include <string.h>

// FTMS Indoor Bike Data flag bits (Bluetooth SIG FTMS 1.0 spec).
// Note: bit 0 is "More Data" — when CLEAR, Instantaneous Speed is present.
static constexpr uint16_t F_MORE_DATA       = 1 << 0;
static constexpr uint16_t F_AVG_SPEED       = 1 << 1;
static constexpr uint16_t F_INST_CADENCE    = 1 << 2;
static constexpr uint16_t F_AVG_CADENCE     = 1 << 3;
static constexpr uint16_t F_TOTAL_DISTANCE  = 1 << 4;
static constexpr uint16_t F_RESISTANCE      = 1 << 5;
static constexpr uint16_t F_INST_POWER      = 1 << 6;
static constexpr uint16_t F_AVG_POWER       = 1 << 7;
static constexpr uint16_t F_ENERGY          = 1 << 8;
static constexpr uint16_t F_HEART_RATE      = 1 << 9;
static constexpr uint16_t F_METABOLIC_EQ    = 1 << 10;
static constexpr uint16_t F_ELAPSED_TIME    = 1 << 11;
static constexpr uint16_t F_REMAINING_TIME  = 1 << 12;

bool parse_indoor_bike_data(const uint8_t* buf, size_t len, BikeFrame& out) {
    memset(&out, 0, sizeof(out));
    if (len < 2) return false;

    out.flags = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    size_t off = 2;

    auto need = [&](size_t n) { return off + n <= len; };
    auto u16 = [&]() { uint16_t v = (uint16_t)buf[off] | ((uint16_t)buf[off+1] << 8); off += 2; return v; };
    auto i16 = [&]() { return (int16_t)u16(); };
    auto u24 = [&]() { uint32_t v = (uint32_t)buf[off] | ((uint32_t)buf[off+1] << 8) | ((uint32_t)buf[off+2] << 16); off += 3; return v; };

    if (!(out.flags & F_MORE_DATA)) {
        if (!need(2)) return false;
        out.speed_cmps = u16();
        out.has_speed = true;
    }
    if (out.flags & F_AVG_SPEED) {
        if (!need(2)) return false;
        off += 2;
    }
    if (out.flags & F_INST_CADENCE) {
        if (!need(2)) return false;
        out.cadence_halfrpm = u16();
        out.has_cadence = true;
    }
    if (out.flags & F_AVG_CADENCE) {
        if (!need(2)) return false;
        off += 2;
    }
    if (out.flags & F_TOTAL_DISTANCE) {
        if (!need(3)) return false;
        out.distance_m = u24();
        out.has_distance = true;
    }
    if (out.flags & F_RESISTANCE) {
        if (!need(2)) return false;
        out.resistance = i16();
        out.has_resistance = true;
    }
    if (out.flags & F_INST_POWER) {
        if (!need(2)) return false;
        out.inst_power_w = i16();
        out.has_inst_power = true;
    }
    if (out.flags & F_AVG_POWER) {
        if (!need(2)) return false;
        out.avg_power_w = i16();
        out.has_avg_power = true;
    }
    if (out.flags & F_ENERGY) {
        if (!need(5)) return false;
        out.total_energy_kcal = u16();
        off += 2; // energy per hour, ignored
        off += 1; // energy per minute, ignored
        out.has_energy = true;
    }
    if (out.flags & F_HEART_RATE) {
        if (!need(1)) return false;
        off += 1;
    }
    if (out.flags & F_METABOLIC_EQ) {
        if (!need(1)) return false;
        off += 1;
    }
    if (out.flags & F_ELAPSED_TIME) {
        if (!need(2)) return false;
        out.elapsed_s = u16();
        out.has_elapsed = true;
    }
    if (out.flags & F_REMAINING_TIME) {
        if (!need(2)) return false;
        off += 2;
    }
    return true;
}
