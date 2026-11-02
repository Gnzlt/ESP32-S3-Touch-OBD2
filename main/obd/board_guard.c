#include "board_guard.h"

#include "alerts.h"

void guard_init(board_guard_t *g)
{
    g->low_since = g->crit_since = -1;
    g->heat_saver = false;
    g->heat_armed = true;
}

void guard_saver_changed(board_guard_t *g) { g->heat_saver = false; }

static bool warm(const guard_in_t *in)
{
    return (in->has_chip && in->chip_c >= CHIP_WARN_C) || (in->has_cell && in->cell_c >= CELL_WARN_C);
}

// Both parts back under their warning levels by TEMP_REARM_C (an unknown one counts as cool).
static bool cooled(const guard_in_t *in)
{
    return (!in->has_chip || in->chip_c <= CHIP_WARN_C - TEMP_REARM_C)
           && (!in->has_cell || in->cell_c <= CELL_WARN_C - TEMP_REARM_C);
}

static bool critical(const guard_in_t *in)
{
    return (in->has_chip && in->chip_c >= CHIP_CRIT_C) || (in->has_cell && in->cell_c >= CELL_CRIT_C);
}

static bool held(int64_t *since, bool now_true, int64_t now, int64_t hold_ms)
{
    if (!now_true) {
        *since = -1;
        return false;
    }
    if (*since < 0) *since = now;
    return now - *since >= hold_ms;
}

guard_action_t guard_update(board_guard_t *g, const guard_in_t *in, int64_t now)
{
    bool empty = in->battery && !in->charging && in->mv > 0 && in->mv < GUARD_EMPTY_MV;
    if (held(&g->low_since, empty, now, GUARD_EMPTY_HOLD_MS)) return GUARD_OFF_EMPTY;
    if (held(&g->crit_since, critical(in), now, GUARD_HOT_HOLD_MS)) return GUARD_OFF_HOT;
    if (cooled(in)) {
        g->heat_armed = true;
        if (g->heat_saver) {
            g->heat_saver = false;
            if (in->saver && !in->low) return GUARD_SAVER_OFF;
        }
    }
    // Once per warm spell: switched off by hand while still warm, it stays off.
    if (warm(in) && g->heat_armed) {
        g->heat_armed = false;
        if (!in->saver) {
            g->heat_saver = true;
            return GUARD_SAVER_ON;
        }
    }
    return GUARD_NONE;
}

bool guard_may_wake(guard_action_t why, const guard_in_t *in)
{
    if (why == GUARD_OFF_EMPTY) {
        // USB is back, or the cell is no longer empty (charged elsewhere,
        // or swapped). A cell that wasn't read can't be judged: start.
        return !in->battery || in->charging || in->mv >= GUARD_WAKE_MV;
    }
    if (why == GUARD_OFF_HOT) return cooled(in);
    return true;
}
