#include <string.h>
#include "fuel.h"
#include "bike/bike.h"

#define RHO_STD      1.184f     // kg/m^3 at 25 C, 101.3 kPa: the load PID's reference
#define FUEL_KG_L    0.745f     // petrol (E5/E10)
#define AFR_STOICH   14.7f
#define AFR_RICH     13.5f      // open loop: cold, or a fault
#define AFR_POWER    12.5f      // open loop under high load
#define RUNNING_RPM  300.0f
// Status 4 ("open loop: driving conditions") covers three cases in a real
// log: a closed-throttle idle (load ~27 %, fuelled normally), decel fuel cut
// (low load well above idle) and power enrichment (high load).
#define CUT_LOAD     15.0f
#define CUT_RPM      2000.0f
#define POWER_LOAD   70.0f

float fuel_rate_lph(const fuel_in_t *in)
{
    if (in->has_rpm && in->rpm < RUNNING_RPM) {
        return 0.0f;
    }
    if (!in->has_rpm || !in->has_load) {
        return -1.0f;
    }
    float afr = AFR_STOICH, trim = 1.0f;
    switch (in->status) {
    case 2:     // closed loop
    case 16:    // closed loop, with a fault in the feedback
        if (in->has_stft) trim = 1.0f + in->stft_pct / 100.0f;
        break;
    case 1:     // open loop, engine cold
    case 8:     // open loop, system fault
        afr = AFR_RICH;
        break;
    case 4:
        if (in->load_pct < CUT_LOAD && in->rpm > CUT_RPM) return 0.0f;
        if (in->load_pct >= POWER_LOAD) afr = AFR_POWER;
        break;
    default:    // unknown: assume stoichiometric
        break;
    }
    float air_kg_s = in->load_pct / 100.0f * RHO_STD * BIKE_DISPLACEMENT_L / 1000.0f * BIKE_VE_PEAK * in->rpm / 120.0f;
    return air_kg_s / afr * trim * 3600.0f / FUEL_KG_L;
}

void fill_reset(fill_t *f)
{
    memset(f, 0, sizeof(*f));
    f->version = FILL_VERSION;
    f->k = 1.0f;
}

void fill_add(fill_t *f, int dt_ms, bool running, float lph, bool has_kmh, float kmh)
{
    if (dt_ms <= 0 || dt_ms > 2000) {
        return;   // a gap: integrating across it would invent distance and fuel
    }
    if (has_kmh) f->km += kmh * dt_ms / 3600000.0;
    if (lph > 0) f->raw_ml += lph * dt_ms / 3600.0;
    if (running) {
        if (lph >= 0) {
            f->covered_ms += dt_ms;
        } else {
            f->missing_ms += dt_ms;
        }
    }
}

void fill_mark(fill_t *f)
{
    f->fills++;
    f->km = 0;
    f->raw_ml = 0;
    f->covered_ms = 0;
    f->missing_ms = 0;
}

int fuel_coverage_pct(uint32_t covered_ms, uint32_t missing_ms)
{
    uint64_t total = (uint64_t)covered_ms + missing_ms;
    return total ? (int)(covered_ms * 100ull / total) : 100;
}
