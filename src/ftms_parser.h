#pragma once
#include <stdint.h>
#include <stddef.h>

// Decoded FTMS Indoor Bike Data frame (characteristic 0x2AD2).
// Field units match the spec: speed in 0.01 km/h, cadence in 0.5 RPM,
// distance in meters, power in watts, time in seconds.
struct BikeFrame {
    uint32_t millis_received;
    uint16_t flags;
    uint16_t speed_cmps;
    uint16_t cadence_halfrpm;
    uint32_t distance_m;
    int16_t  resistance;
    int16_t  inst_power_w;
    int16_t  avg_power_w;
    uint16_t total_energy_kcal;
    uint16_t elapsed_s;

    bool has_speed;
    bool has_cadence;
    bool has_distance;
    bool has_resistance;
    bool has_inst_power;
    bool has_avg_power;
    bool has_energy;
    bool has_elapsed;
};

// Parses a single FTMS Indoor Bike Data notification per spec.
// Walks the buffer field-by-field driven by the 16-bit flags field.
// Returns false on truncated buffers.
bool parse_indoor_bike_data(const uint8_t* buf, size_t len, BikeFrame& out);
