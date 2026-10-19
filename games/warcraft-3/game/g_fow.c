#include "g_local.h"
#include <stdlib.h>

#define FOW_INVALID_CELL 0xffffffffu
#define FOW_PATHING_PIXEL_SIZE 32.0f
#define FOW_TREE_DILATION_CELLS 1
#define FOW_BLOCKER_LIGHT_MARGIN_CELLS 1
#define G_FOW_CELL_INDEX(x, y) ((y) * level.fow.width + (x))
#define G_FOW_SET_VISIBLE_CELL(grid, x, y) do { \
    uint32_t fow_index_ = G_FOW_CELL_INDEX((uint32_t)(x), (uint32_t)(y)); \
    if (!(grid)->visible[fow_index_]) { \
        (grid)->visible[fow_index_] = 1; \
        (grid)->visible_rows[(uint32_t)(y)] = 1; \
        if ((grid)->dirty_visible_rows) { \
            (grid)->dirty_visible_rows[(uint32_t)(y)] = 1; \
        } \
    } \
    if (!(grid)->explored[fow_index_]) { \
        (grid)->explored[fow_index_] = 1; \
        if ((grid)->dirty_explored_rows) { \
            (grid)->dirty_explored_rows[(uint32_t)(y)] = 1; \
        } \
    } \
} while (0)

static uint32_t g_fow_blocker_hash;
static uint32_t g_fow_blocker_count;
static bool g_fow_blockers_valid;
static bool g_fow_blockers_dirty = true;
static bool g_visibility_fast;
static uint32_t *g_fow_blocker_prefix;
static size_t g_fow_prefix_capacity;
static bool g_fow_prefix_attempted, g_fow_prefix_valid;
#ifdef __ANDROID__
#define BZ_FOW_CACHE_DEFAULT "1"
#define BZ_VISIBILITY_FAST_DEFAULT "1"
#else
#define BZ_FOW_CACHE_DEFAULT "0"
#define BZ_VISIBILITY_FAST_DEFAULT "0"
#endif
#define FOW_CACHE_SLOTS 512 // entries; bounded exact footprints for dense armies without per-entity state
#define FOW_CACHE_RADIUS 31 // fog cells; a 63-cell diameter fits each cached row in one uint64_t
typedef struct {
    uint64_t rows[2 * FOW_CACHE_RADIUS + 1];
    uint32_t x, y, radius;
} fowSightCache_t;
static fowSightCache_t g_fow_sight_cache[FOW_CACHE_SLOTS], *g_fow_capture;
static uint8_t *g_fow_cached_blockers;
static bool g_fow_cache_enabled, g_fow_cache_failed;
static struct { uint32_t start, updates, hits, misses, replaced, bypasses; } g_fow_cache_stats;
#ifdef BZ_TESTS
uint32_t G_TestFowCellWrites, G_TestFowBlockerLookups, G_TestFowCastCalls, G_TestForcedVisibilityOwners;
bool G_TestFowPrefixAllocationFails, G_TestFowCacheAllocationFails;
#endif
#ifdef WC3_FOW_PACKED_MASK
static bool g_fow_fast;
#endif

static uint32_t G_FowCellCount(void) {
    return level.fow.width * level.fow.height;
}

static uint32_t G_FowCellIndex(uint32_t x, uint32_t y) {
    return G_FOW_CELL_INDEX(x, y);
}

static bool G_FowReady(void) {
    return level.fow.width > 0 && level.fow.height > 0;
}

/* Item Reveal Entire Map is a temporary, player-local sight source. Its
 * deadline lives in map state so save/load preserves the remaining duration. */
void G_FowRevealEntireMapTimed(uint32_t player, float duration) {
    uint32_t until;
    if (player >= MAX_PLAYERS || !(duration > 0.0f)) return;
    until = G_Time() + (uint32_t)(duration * 1000.0f);
    if (until > level.fow.omniscience_until[player])
        level.fow.omniscience_until[player] = until;
    if (G_FowReady())
        G_FowSetStateRect(&(fogWrite_t){ player, WC3_FOG_STATE_VISIBLE, false },
                          &level.fow.bounds);
}

bool G_FowPlayersShareVision(uint32_t viewer, uint32_t owner) {
    if (viewer >= MAX_PLAYERS || owner >= MAX_PLAYERS) {
        return false;
    }
    /* SetPlayerAlliance(source, other, shared vision) means source shares its
     * sight with other.  The viewer therefore reads the owner's outgoing
     * alliance bit. */
    return viewer == owner ||
           (level.alliances[owner][viewer] & (1 << ALLIANCE_SHARED_VISION)) ||
           (level.alliances[owner][viewer] & (1 << ALLIANCE_SHARED_VISION_FORCED));
}

bool G_UnitSharesVisionWith(edict_t const *unit, uint32_t viewer) {
    return unit && viewer < MAX_PLAYERS && (unit->shared_vision & (1u << viewer));
}

void G_SetUnitSharedVision(edict_t *unit, uint32_t viewer, bool share) {
    if (!unit || viewer >= MAX_PLAYERS) return;
    if (share) unit->shared_vision |= 1u << viewer;
    else unit->shared_vision &= ~(1u << viewer);
}

void G_AddUnitForcedVisibility(edict_t *unit, uint32_t viewer) {
    if (unit && viewer < MAX_PLAYERS && unit->forced_visibility_count[viewer] != 0xffffu)
        unit->forced_visibility_count[viewer]++;
}

void G_RemoveUnitForcedVisibility(edict_t *unit, uint32_t viewer) {
    if (unit && viewer < MAX_PLAYERS && unit->forced_visibility_count[viewer])
        unit->forced_visibility_count[viewer]--;
}

bool G_UnitIsForcedVisibleToPlayer(edict_t const *unit, uint32_t viewer) {
    if (!unit || viewer >= MAX_PLAYERS) return false;
    if (g_visibility_fast) {
        uint64_t any = 0;
        size_t bytes = sizeof(unit->forced_visibility_count);
        _Static_assert(sizeof(unit->forced_visibility_count) % sizeof(any) == 0, "Whole visibility words required");
        FOR_LOOP(i, bytes / sizeof(any)) {
            uint64_t word;
            memcpy(&word, (uint8_t const *)unit->forced_visibility_count + i * sizeof(word), sizeof(word));
            any |= word;
        }
        if (!any) return false;
    }
    FOR_LOOP(owner, MAX_PLAYERS) {
#ifdef BZ_TESTS
        G_TestForcedVisibilityOwners++;
#endif
        if (unit->forced_visibility_count[owner] && G_FowPlayersShareVision(viewer, owner)) return true;
    }
    return false;
}

uint32_t G_FowWorldToCellX(float x) {
    if (!G_FowReady()) {
        return FOW_INVALID_CELL;
    }
    int cell = (int)floorf((x - level.fow.bounds.min.x) / (float)FOW_CELL_SIZE);
    if (cell < 0) {
        return 0;
    }
    if ((uint32_t)cell >= level.fow.width) {
        return level.fow.width - 1;
    }
    return (uint32_t)cell;
}

uint32_t G_FowWorldToCellY(float y) {
    if (!G_FowReady()) {
        return FOW_INVALID_CELL;
    }
    int cell = (int)floorf((y - level.fow.bounds.min.y) / (float)FOW_CELL_SIZE);
    if (cell < 0) {
        return 0;
    }
    if ((uint32_t)cell >= level.fow.height) {
        return level.fow.height - 1;
    }
    return (uint32_t)cell;
}

static void G_FowSetVisible(fowPlayerGrid_t *grid, uint32_t x, uint32_t y) {
    if (!grid || !grid->visible || !grid->explored ||
        x >= level.fow.width || y >= level.fow.height) {
        return;
    }

    G_FOW_SET_VISIBLE_CELL(grid, x, y);
}

static bool G_FowStateValid(uint32_t state) { return state && state <= WC3_FOG_STATE_VISIBLE && !(state & (state - 1)); }

typedef struct {
    uint32_t x, y, state;
    int cells;
} fogDisk_t;



/* Scripted fog states own both planes, so the three JASS states need no parallel cinematic map. */
static void G_FowSetCellState(fowPlayerGrid_t *grid, uint32_t index, uint32_t state) {
#ifdef BZ_TESTS
    G_TestFowCellWrites++;
#endif
    uint32_t y, x;
    if (!grid || !grid->visible || !grid->explored || index >= G_FowCellCount()) return;
    y = index / level.fow.width;
    x = index - y * level.fow.width;
#ifdef WC3_FOW_PACKED_MASK
    if (g_fow_fast) {
        uint16_t *visible = grid->packed_visible + (x >> 4) + y * grid->packed_stride;
        uint16_t *explored = grid->packed_explored + (x >> 4) + y * grid->packed_stride;
        uint16_t bit = (uint16_t)(1u << (x & 15));
        /* Scripted fog writes must update the packed planes read in fast mode, not only the legacy byte planes. */
        if (state == WC3_FOG_STATE_VISIBLE) *visible |= bit, *explored |= bit;
        else if (state == WC3_FOG_STATE_FOGGED) *visible &= ~bit, *explored |= bit;
        else *visible &= ~bit, *explored &= ~bit;
    }
#endif
    if (state == WC3_FOG_STATE_VISIBLE) {
        G_FOW_SET_VISIBLE_CELL(grid, x, y);
        return;
    }
    if (grid->visible[index]) {
        grid->visible[index] = 0;
        if (grid->dirty_visible_rows) grid->dirty_visible_rows[y] = 1;
    }
    if (state == WC3_FOG_STATE_FOGGED) {
        if (!grid->explored[index]) {
            grid->explored[index] = 1;
            if (grid->dirty_explored_rows) grid->dirty_explored_rows[y] = 1;
        }
        return;
    }
    if (state == WC3_FOG_STATE_MASKED && grid->explored[index]) {
        grid->explored[index] = 0;
        if (grid->dirty_explored_rows) grid->dirty_explored_rows[y] = 1;
    }
}

static void G_FowSetBlocked(uint32_t x, uint32_t y) {
    uint32_t index;

    if (!level.fow.blocked || x >= level.fow.width || y >= level.fow.height) {
        return;
    }
    index = G_FowCellIndex(x, y);
    if (!level.fow.blocked[index]) {
        level.fow.blocked[index] = 1;
        level.fow.num_blocked++;
    }
}

/* Rebuild from the current bitmap once per update; optional scratch never owns fog state. */
static void G_FowBuildBlockerPrefix(void) {
    size_t stride = (size_t)level.fow.width + 1;
    size_t rows = (size_t)level.fow.height + 1;
    g_fow_prefix_attempted = true;
    if (stride > SIZE_MAX / rows) return;
    size_t count = stride * rows;
    if (count > SIZE_MAX / sizeof(*g_fow_blocker_prefix)) return;
    if (count > g_fow_prefix_capacity) {
#ifdef BZ_TESTS
        uint32_t *next = G_TestFowPrefixAllocationFails ? NULL : realloc(g_fow_blocker_prefix, count * sizeof(*next));
#else
        uint32_t *next = realloc(g_fow_blocker_prefix, count * sizeof(*next));
#endif
        if (!next) return; /* The exact bitmap scan remains available. */
        g_fow_blocker_prefix = next; g_fow_prefix_capacity = count;
    }
    memset(g_fow_blocker_prefix, 0, stride * sizeof(*g_fow_blocker_prefix));
    FOR_LOOP(y, level.fow.height) {
        uint32_t sum = 0;
        uint32_t *row = g_fow_blocker_prefix + (y + 1) * stride;
        uint32_t const *above = row - stride;
        uint8_t const *blocked = level.fow.blocked + y * level.fow.width;
        row[0] = 0;
        FOR_LOOP(x, level.fow.width) { sum += blocked[x] != 0; row[x + 1] = above[x + 1] + sum; }
    }
    g_fow_prefix_valid = true;
}

static void G_FowSetBlockedDilated(uint32_t x, uint32_t y, int dilation) {
    for (int dy = -dilation; dy <= dilation; dy++) {
        int by = (int)y + dy;
        if (by < 0 || by >= (int)level.fow.height) {
            continue;
        }
        for (int dx = -dilation; dx <= dilation; dx++) {
            int bx = (int)x + dx;
            if (bx < 0 || bx >= (int)level.fow.width) {
                continue;
            }
            G_FowSetBlocked((uint32_t)bx, (uint32_t)by);
        }
    }
}

static void G_FowClearVisible(fowPlayerGrid_t *grid) {
    if (!grid || !grid->visible || !grid->visible_rows || !grid->dirty_visible_rows) {
        return;
    }
    FOR_LOOP(y, level.fow.height) {
        if (!grid->visible_rows[y]) continue;
        /* Visibility writers mark occupied rows, so clearing no longer scans every cell of a mostly hidden map. */
        memset(grid->visible + y * level.fow.width, 0, level.fow.width);
#ifdef WC3_FOW_PACKED_MASK
        memset(grid->packed_visible + y * grid->packed_stride, 0, grid->packed_stride * sizeof(*grid->packed_visible));
#endif
        grid->visible_rows[y] = 0;
        grid->dirty_visible_rows[y] = 1;
    }
}

static bool G_FowAnyBlockedInBox(int minx, int miny, int maxx, int maxy) {
    if (!level.fow.blocked || !level.fow.num_blocked) {
        return false;
    }

    minx = MAX(minx, 0);
    miny = MAX(miny, 0);
    maxx = MIN(maxx, (int)level.fow.width - 1);
    maxy = MIN(maxy, (int)level.fow.height - 1);
    if (minx > maxx || miny > maxy) return false;
    if (!g_fow_prefix_attempted) G_FowBuildBlockerPrefix();
    if (g_fow_prefix_valid) {
        size_t stride = (size_t)level.fow.width + 1;
        uint32_t const *top = g_fow_blocker_prefix + (size_t)miny * stride;
        uint32_t const *bottom = g_fow_blocker_prefix + ((size_t)maxy + 1) * stride;
#ifdef BZ_TESTS
        G_TestFowBlockerLookups += 4;
#endif
        return bottom[maxx + 1] - bottom[minx] - top[maxx + 1] + top[minx] != 0;
    }
    for (int y = miny; y <= maxy; y++) {
        uint8_t const *row = level.fow.blocked + y * level.fow.width;
        for (int x = minx; x <= maxx; x++) {
#ifdef BZ_TESTS
            G_TestFowBlockerLookups++;
#endif
            if (row[x]) {
                return true;
            }
        }
    }
    return false;
}

#ifdef BZ_TESTS
bool G_TestFowBlockerBox(int minx, int miny, int maxx, int maxy) {
    return G_FowAnyBlockedInBox(minx, miny, maxx, maxy);
}
#endif

static int G_FowRadiusCells(float radius) {
    return MAX(1, (int)ceilf(radius / (float)FOW_CELL_SIZE));
}

/* Hoist row bookkeeping out of the cell loop; preserve nonzero temporary visibility markers. */
static void G_FowSetSpanState(fowPlayerGrid_t *grid, uint32_t y, uint32_t first, uint32_t last, uint32_t state) {
#ifdef WC3_FOW_PACKED_MASK
    if (g_fow_fast) {
        for (uint32_t x = first; x <= last; x++) G_FowSetCellState(grid, G_FowCellIndex(x, y), state);
        return;
    }
#endif
    uint8_t *visible = grid->visible + y * level.fow.width + first;
    uint8_t *explored = grid->explored + y * level.fow.width + first;
    uint32_t length = last - first + 1;
    bool visible_changed = false, explored_changed = false;
    if (state == WC3_FOG_STATE_VISIBLE && !memchr(visible, 0, length) && !memchr(explored, 0, length)) return;
    FOR_LOOP(i, length) {
        if (state == WC3_FOG_STATE_VISIBLE) {
            if (!visible[i]) { visible[i] = 1; visible_changed = true; }
        } else if (visible[i]) { visible[i] = 0; visible_changed = true; }
        if (state == WC3_FOG_STATE_VISIBLE || state == WC3_FOG_STATE_FOGGED) {
            if (!explored[i]) { explored[i] = 1; explored_changed = true; }
        } else if (state == WC3_FOG_STATE_MASKED && explored[i]) { explored[i] = 0; explored_changed = true; }
    }
    if (visible_changed) {
        if (state == WC3_FOG_STATE_VISIBLE) grid->visible_rows[y] = 1;
        if (grid->dirty_visible_rows) grid->dirty_visible_rows[y] = 1;
    }
    if (explored_changed && grid->dirty_explored_rows) grid->dirty_explored_rows[y] = 1;
}

/* Circular trigger/modifier state writes reuse the ordinary fog-grid rasterization. */
static void G_FowSetDiskState(fowPlayerGrid_t *grid, fogDisk_t const *disk) {
    int radius_sq = disk->cells * disk->cells;

    for (int dy = -disk->cells; dy <= disk->cells; dy++) {
        int y = (int)disk->y + dy;
        int max_dx;
        if (y < 0 || y >= (int)level.fow.height) {
            continue;
        }
        max_dx = (int)sqrtf((float)(radius_sq - dy * dy));
        int first = MAX(0, (int)disk->x - max_dx);
        int last = MIN((int)level.fow.width - 1, (int)disk->x + max_dx);
        if (first <= last) G_FowSetSpanState(grid, y, first, last, disk->state);
    }
}
static void G_FowRevealDisk(fowPlayerGrid_t *grid, uint32_t cx, uint32_t cy, int radius_cells) {
    fogDisk_t disk = { cx, cy, WC3_FOG_STATE_VISIBLE, radius_cells };
    G_FowSetDiskState(grid, &disk);
}

#ifdef WC3_FOW_PACKED_MASK
/* Apply retail's packed horizontal spans; the byte plane is materialized once after all revealers. */
static void G_FowRevealPacked(fowPlayerGrid_t *grid, uint32_t cx, uint32_t cy, int radius_cells) {
    static uint16_t const bit[16] = {
        0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
        0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000,
    };
    int radius_sq = radius_cells * radius_cells;

    for (int dy = -radius_cells; dy <= radius_cells; dy++) {
        int y = (int)cy + dy;
        int max_dx;
        int min_x;
        int max_x;

        if (y < 0 || y >= (int)level.fow.height) continue;
        max_dx = (int)sqrtf((float)(radius_sq - dy * dy));
        min_x = MAX(0, (int)cx - max_dx);
        max_x = MIN((int)level.fow.width - 1, (int)cx + max_dx);
        for (int x = min_x; x <= max_x;) {
            int word = x >> 4;
            int first = x & 15;
            int last = MIN(15, max_x - (word << 4));
            uint16_t mask = 0;

            for (int bit_index = first; bit_index <= last; bit_index++) mask |= bit[bit_index];
            grid->packed_visible[word + y * grid->packed_stride] |= mask;
            grid->packed_explored[word + y * grid->packed_stride] |= mask;
            grid->visible_rows[y] = 1;
            grid->dirty_visible_rows[y] = 1;
            grid->dirty_explored_rows[y] = 1;
            x = (word + 1) << 4;
        }
    }
}

static void G_FowRevealPackedBox(fowPlayerGrid_t *grid, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    FOR_LOOP(y, y1 - y0 + 1) {
        uint32_t const row = y + y0;
        uint32_t x = x0;
        while (x <= x1) {
            uint32_t const word = x >> 4;
            uint32_t const first = x & 15;
            uint32_t const last = MIN(15, x1 - (word << 4));
            uint16_t mask = 0;
            for (uint32_t bit_index = first; bit_index <= last; bit_index++) mask |= (uint16_t)(1u << bit_index);
            grid->packed_visible[word + row * grid->packed_stride] |= mask;
            grid->packed_explored[word + row * grid->packed_stride] |= mask;
            grid->visible_rows[row] = grid->dirty_visible_rows[row] = 1;
            grid->dirty_explored_rows[row] = 1;
            x = (word + 1) << 4;
        }
    }
}

static bool G_FowPackedAt(uint16_t const *plane, fowPlayerGrid_t const *grid, uint32_t x, uint32_t y) {
    return plane[(x >> 4) + y * grid->packed_stride] & (1u << (x & 15));
}
#endif

/* Visibility depends on the actual blocker bytes, including edits that do not rebuild entity blockers. */
static void G_FowPrepareSightCache(void) {
    g_fow_cache_enabled = atoi(gi.CvarString("wc3_fow_cache", BZ_FOW_CACHE_DEFAULT)) != 0;
    if (!g_fow_cache_enabled || g_fow_cache_failed) { g_fow_cache_enabled = false; return; }
    uint32_t cells = G_FowCellCount();
    if (!g_fow_cached_blockers) {
#ifdef BZ_TESTS
        g_fow_cached_blockers = G_TestFowCacheAllocationFails ? NULL : malloc(cells);
#else
        g_fow_cached_blockers = malloc(cells);
#endif
        if (!g_fow_cached_blockers) {
            fprintf(stderr, "G_FowUpdate: unable to allocate %u-byte sight-cache blocker snapshot; using exact casts\n", cells);
            g_fow_cache_failed = true; g_fow_cache_enabled = false; return;
        }
        memcpy(g_fow_cached_blockers, level.fow.blocked, cells);
    } else if (memcmp(g_fow_cached_blockers, level.fow.blocked, cells)) {
        memcpy(g_fow_cached_blockers, level.fow.blocked, cells);
        memset(g_fow_sight_cache, 0, sizeof(g_fow_sight_cache));
    }
}

/* Reapply only shadowcast cells; the order-dependent tree rim still runs against the live viewer grid. */
static bool G_FowReplaySight(fowPlayerGrid_t *grid, fowSightCache_t const *cached) {
    if (!cached->radius) return false;
    FOR_LOOP(row, cached->radius * 2 + 1) {
        uint64_t bits = cached->rows[row];
        while (bits) {
            uint32_t first = __builtin_ctzll(bits), count = __builtin_ctzll(~(bits >> first));
            uint32_t x = cached->x + first - cached->radius, y = cached->y + row - cached->radius;
            G_FowSetSpanState(grid, y, x, x + count - 1, WC3_FOG_STATE_VISIBLE);
            bits &= ~((UINT64_MAX >> (64 - count)) << first);
        }
    }
    return true;
}

static void G_FowCastLight(fowPlayerGrid_t *grid,
                           int cx,
                           int cy,
                           int row,
                           float start,
                           float end,
                           int radius,
                           int xx,
                           int xy,
                           int yx,
                           int yy)
{
    int radius_sq = radius * radius;
#ifdef BZ_TESTS
    G_TestFowCastCalls++;
#endif

    if (start < end) {
        return;
    }

    for (int distance = row; distance <= radius; distance++) {
        bool blocked = false;
        float next_start = start;
        int delta_y = -distance;

        for (int delta_x = -distance; delta_x <= 0; delta_x++) {
            int x = cx + delta_x * xx + delta_y * xy;
            int y = cy + delta_x * yx + delta_y * yy;
            float left_slope = ((float)delta_x - 0.5f) / ((float)delta_y + 0.5f);
            float right_slope = ((float)delta_x + 0.5f) / ((float)delta_y - 0.5f);
            bool in_bounds;
            bool cell_blocked;

            if (start < right_slope) {
                continue;
            }
            if (end > left_slope) {
                break;
            }

            in_bounds = x >= 0 && y >= 0 &&
                        x < (int)level.fow.width && y < (int)level.fow.height;
            cell_blocked = in_bounds &&
                           level.fow.blocked[G_FOW_CELL_INDEX((uint32_t)x, (uint32_t)y)] != 0;
            if (in_bounds && delta_x * delta_x + delta_y * delta_y <= radius_sq)
            {
                G_FOW_SET_VISIBLE_CELL(grid, x, y);
                if (g_fow_capture)
                    g_fow_capture->rows[y - cy + radius] |= UINT64_C(1) << (x - cx + radius);
            }

            if (blocked) {
                if (cell_blocked) {
                    next_start = right_slope;
                    continue;
                }
                blocked = false;
                start = next_start;
            } else if (cell_blocked && distance < radius) {
                blocked = true;
                G_FowCastLight(grid,
                               cx,
                               cy,
                               distance + 1,
                               start,
                               left_slope,
                               radius,
                               xx,
                               xy,
                               yx,
                               yy);
                next_start = right_slope;
            }
        }
        if (blocked) {
            break;
        }
    }
}

static void G_FowRevealShadowcast(fowPlayerGrid_t *grid, uint32_t cx, uint32_t cy, int radius_cells) {
    static int const mult[8][4] = {
        { 1,  0,  0,  1 },
        { 0,  1,  1,  0 },
        { 0, -1,  1,  0 },
        { -1, 0,  0,  1 },
        { -1, 0,  0, -1 },
        { 0, -1, -1,  0 },
        { 0,  1, -1,  0 },
        { 1,  0,  0, -1 },
    };

    fowSightCache_t *cached = NULL;
    if (g_fow_cache_enabled && radius_cells <= FOW_CACHE_RADIUS) {
        uint32_t hash = (cx * 73856093u ^ cy * 19349663u ^ (uint32_t)radius_cells * 83492791u);
        cached = &g_fow_sight_cache[hash & (FOW_CACHE_SLOTS - 1)];
        if (cached->x == cx && cached->y == cy && cached->radius == (uint32_t)radius_cells &&
            G_FowReplaySight(grid, cached)) {
            g_fow_cache_stats.hits++; return;
        }
        g_fow_cache_stats.replaced += cached->radius != 0;
        *cached = (fowSightCache_t){.x = cx, .y = cy, .radius = (uint32_t)radius_cells};
        cached->rows[radius_cells] = UINT64_C(1) << radius_cells;
        g_fow_cache_stats.misses++;
    } else g_fow_cache_stats.bypasses++;
    g_fow_capture = cached;
    G_FowSetVisible(grid, cx, cy);
    FOR_LOOP(octant, 8) {
        G_FowCastLight(grid,
                       (int)cx,
                       (int)cy,
                       1,
                       1.0f,
                       0.0f,
                       radius_cells,
                       mult[octant][0],
                       mult[octant][1],
                       mult[octant][2],
                       mult[octant][3]);
    }
    g_fow_capture = NULL;
}

static bool G_FowHasVisibleNeighbor(fowPlayerGrid_t *grid, int x, int y, int margin) {
    int margin_sq = margin * margin;

    for (int dy = -margin; dy <= margin; dy++) {
        int ny = y + dy;
        if (ny < 0 || ny >= (int)level.fow.height) {
            continue;
        }
        for (int dx = -margin; dx <= margin; dx++) {
            int nx = x + dx;
            uint32_t index;

            if (nx < 0 || nx >= (int)level.fow.width) {
                continue;
            }
            if (dx * dx + dy * dy > margin_sq) {
                continue;
            }
            index = G_FOW_CELL_INDEX((uint32_t)nx, (uint32_t)ny);
            if (grid->visible[index]) {
                return true;
            }
        }
    }
    return false;
}

/* Commit only marked blockers; the old second square walk revisited over 10K cells per Human02 update. */
static void G_FowCommitRimCells(fowPlayerGrid_t *grid, uint32_t count) {
    FOR_LOOP(i, count) {
        uint32_t const index = level.fow.rim_cells[i];
        uint32_t const y = index / level.fow.width;
        uint32_t const x = index - y * level.fow.width;

        grid->visible[index] = 0;
        G_FOW_SET_VISIBLE_CELL(grid, x, y);
    }
}

static void G_FowRevealBlockerRim(fowPlayerGrid_t *grid, uint32_t cx, uint32_t cy, int radius_cells) {
    int margin = FOW_BLOCKER_LIGHT_MARGIN_CELLS;
    int max_radius = radius_cells + margin;
    int max_radius_sq = max_radius * max_radius;
    uint32_t rim_count = 0;

    for (int dy = -max_radius; dy <= max_radius; dy++) {
        int y = (int)cy + dy;
        if (y < 0 || y >= (int)level.fow.height) {
            continue;
        }
        for (int dx = -max_radius; dx <= max_radius; dx++) {
            int x = (int)cx + dx;
            uint32_t index;

            if (x < 0 || x >= (int)level.fow.width) {
                continue;
            }
            if (dx * dx + dy * dy > max_radius_sq) {
                continue;
            }

            index = G_FOW_CELL_INDEX((uint32_t)x, (uint32_t)y);
            if (!level.fow.blocked[index]) {
                continue;
            }
            if (!grid->visible[index] &&
                G_FowHasVisibleNeighbor(grid, x, y, margin))
            {
                grid->visible[index] = 2;
                level.fow.rim_cells[rim_count++] = index;
            }
        }
    }
    G_FowCommitRimCells(grid, rim_count);
}

static void G_FowRevealCircle(uint32_t player, edict_t const *ent, float radius) {
    fowPlayerGrid_t *grid;
    uint32_t cx, cy;
    int radius_cells;

    if (player >= MAX_PLAYERS || !ent || radius <= 0.0f || !G_FowReady()) {
        return;
    }

    grid = &level.fow.players[player];
    cx = G_FowWorldToCellX(ent->s.origin.x);
    cy = G_FowWorldToCellY(ent->s.origin.y);
    if (cx == FOW_INVALID_CELL || cy == FOW_INVALID_CELL) {
        return;
    }

    radius_cells = G_FowRadiusCells(radius);
#ifdef WC3_FOW_PACKED_MASK
    /* This removable experiment mirrors retail's packed-word mask shape but
     * intentionally trades blocker precision for bounded reveal work. */
    if (g_fow_fast) {
        G_FowRevealPacked(grid, cx, cy, radius_cells);
        return;
    }
#endif
    if (G_FowAnyBlockedInBox((int)cx - radius_cells,
                             (int)cy - radius_cells,
                             (int)cx + radius_cells,
                             (int)cy + radius_cells))
    {
        G_FowRevealShadowcast(grid, cx, cy, radius_cells);
        G_FowRevealBlockerRim(grid, cx, cy, radius_cells);
    } else {
        G_FowRevealDisk(grid, cx, cy, radius_cells);
    }
}

/* MiscData owns the dawn/dusk thresholds. The same authoritative simulation
 * time drives sight, regeneration, JASS game state and future presentation. */
bool G_IsNight(void) {
    float const time = G_GetTimeOfDay();
    return !(time >= game.constants.dawnTimeGameHours &&
             time < game.constants.duskTimeGameHours);
}

static float G_FowEntitySightRadius(edict_t const *ent) {
    float day;
    float night;

    if (!ent) {
        return 0.0f;
    }
    day = ent->runtime.sight_radius.day;
    night = ent->runtime.sight_radius.night;
    /* Use the day or night sight radius based on time of day, rather than
     * always taking the larger of the two. */
    if (night <= 0.0f) night = day;
    if (day <= 0.0f) day = night;
    return G_IsNight() ? night : day;
}

static bool G_FowEntityIsRevealer(edict_t const *ent) {
    if (!ent || !ent->inuse || ent->s.player >= MAX_PLAYERS) {
        return false;
    }
    if (ent->svflags & SVF_NOCLIENT) {
        return false;
    }
    /* Gameplay-invisible units still provide sight to their owner. Only
     * non-invisibility RF_HIDDEN states suppress a unit's fog reveal. */
    if ((ent->s.renderfx & RF_HIDDEN) &&
        S_UnitIsHiddenFromPlayer(ent, ent->s.player)) {
        return false;
    }
    if (M_IsDead((edict_t *)ent)) {
        return false;
    }
    return G_FowEntitySightRadius(ent) > 0.0f;
}

static bool G_FowEntityIsBlocker(edict_t const *ent) {
    if (!ent || !ent->inuse || !(ent->s.flags & EF_FOW_BLOCKER)) {
        return false;
    }
    if (ent->s.renderfx & RF_HIDDEN) {
        return false;
    }
    if (M_IsDead((edict_t *)ent)) {
        return false;
    }
    return true;
}

static uint32_t G_FowHashMix(uint32_t hash, uint32_t value) {
    hash ^= value;
    hash *= 16777619u;
    return hash;
}

static uint32_t G_FowHashFloat(uint32_t hash, float value) {
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return G_FowHashMix(hash, bits);
}

static uint32_t G_FowHashPointer(uint32_t hash, void const *ptr) {
    uintptr_t value = (uintptr_t)ptr;

    hash = G_FowHashMix(hash, (uint32_t)value);
    return G_FowHashMix(hash, (uint32_t)(value >> 16 >> 16));
}

/* Blocker owners call this after a lifecycle change so steady updates avoid hashing every edict. */
void G_FowMarkBlockersDirty(void) { g_fow_blockers_dirty = true; }

static bool G_FowBlockersChanged(void) {
    uint32_t hash = 2166136261u;
    uint32_t count = 0;

    if (!g_fow_blockers_dirty) return false;
    g_fow_blockers_dirty = false;
    FOR_LOOP(i, globals.num_edicts) {
        edict_t const *ent = &g_edicts[i];

        if (!G_FowEntityIsBlocker(ent)) {
            continue;
        }

        count++;
        hash = G_FowHashMix(hash, i);
        hash = G_FowHashMix(hash, ent->s.flags);
        hash = G_FowHashMix(hash, ent->s.renderfx);
        hash = G_FowHashFloat(hash, ent->s.origin.x);
        hash = G_FowHashFloat(hash, ent->s.origin.y);
        hash = G_FowHashFloat(hash, ent->s.radius);
        hash = G_FowHashFloat(hash, ent->s.scale);
        hash = G_FowHashFloat(hash, ent->collision);
        hash = G_FowHashFloat(hash, ent->health.value);
        hash = G_FowHashMix(hash, ent->class_id);
        hash = G_FowHashMix(hash, ent->targtype);
        hash = G_FowHashPointer(hash, ent->pathtex);
        if (ent->pathtex) {
            hash = G_FowHashMix(hash, ent->pathtex->width);
            hash = G_FowHashMix(hash, ent->pathtex->height);
        }
    }

    if (g_fow_blockers_valid &&
        g_fow_blocker_hash == hash &&
        g_fow_blocker_count == count)
    {
        return false;
    }

    g_fow_blocker_hash = hash;
    g_fow_blocker_count = count;
    g_fow_blockers_valid = true;
    return true;
}

static int G_FowBlockerDilation(edict_t const *ent) {
    if (ent->targtype == TARG_TREE) {
        return FOW_TREE_DILATION_CELLS;
    }
    if (!(ent->svflags & SVF_MONSTER) && G_IsDestructable(ent) &&
        ent->destructable->occluder_height > 0.0f)
    {
        return FOW_TREE_DILATION_CELLS;
    }
    return 0;
}

static bool G_FowMarkBlockerPathTex(edict_t const *ent, int dilation) {
    pathTex_t const *pathtex = ent->pathtex;
    float scale;
    bool marked = false;

    if (!pathtex || !pathtex->width || !pathtex->height) {
        return false;
    }

    scale = MAX(ent->s.scale, 0.01f);
    FOR_LOOP(py, pathtex->height) {
        FOR_LOOP(px, pathtex->width) {
            color32_t const *pixel = &pathtex->map[px + py * pathtex->width];
            float x;
            float y;
            uint32_t cx;
            uint32_t cy;

            if (!pixel->b) {
                continue;
            }

            x = ent->s.origin.x +
                ((float)px + 0.5f - (float)pathtex->width * 0.5f) *
                FOW_PATHING_PIXEL_SIZE * scale;
            y = ent->s.origin.y +
                ((float)py + 0.5f - (float)pathtex->height * 0.5f) *
                FOW_PATHING_PIXEL_SIZE * scale;
            cx = G_FowWorldToCellX(x);
            cy = G_FowWorldToCellY(y);
            if (cx == FOW_INVALID_CELL || cy == FOW_INVALID_CELL) {
                continue;
            }
            G_FowSetBlockedDilated(cx, cy, dilation);
            marked = true;
        }
    }
    return marked;
}

static void G_FowMarkBlocker(edict_t const *ent) {
    uint32_t cx;
    uint32_t cy;
    float radius;
    int radius_cells;
    int dilation;

    if (!G_FowEntityIsBlocker(ent)) {
        return;
    }

    dilation = G_FowBlockerDilation(ent);
    if (G_FowMarkBlockerPathTex(ent, dilation)) {
        return;
    }

    cx = G_FowWorldToCellX(ent->s.origin.x);
    cy = G_FowWorldToCellY(ent->s.origin.y);
    if (cx == FOW_INVALID_CELL || cy == FOW_INVALID_CELL) {
        return;
    }

    G_FowSetBlockedDilated(cx, cy, dilation);
    radius = MAX(ent->s.radius, ent->collision);
    radius_cells = (int)floorf(radius / (float)FOW_CELL_SIZE);
    if (radius_cells <= 0) {
        return;
    }

    for (int dy = -radius_cells; dy <= radius_cells; dy++) {
        int y = (int)cy + dy;
        if (y < 0 || y >= (int)level.fow.height) {
            continue;
        }
        for (int dx = -radius_cells; dx <= radius_cells; dx++) {
            int x = (int)cx + dx;
            if (x < 0 || x >= (int)level.fow.width) {
                continue;
            }
            if (dx * dx + dy * dy <= radius_cells * radius_cells) {
                G_FowSetBlockedDilated((uint32_t)x, (uint32_t)y, dilation);
            }
        }
    }
}

static void G_FowRebuildBlockers(void) {
    if (!level.fow.blocked) {
        return;
    }
    memset(level.fow.blocked, 0, G_FowCellCount());
    level.fow.num_blocked = 0;
    FOR_LOOP(i, globals.num_edicts) {
        G_FowMarkBlocker(&g_edicts[i]);
    }
}

/* Reveal directly into connected viewer grids; source-owner grids are irrelevant when nobody consumes them. */
static void G_FowRevealForViewers(edict_t const *ent, float radius, uint32_t viewers) {
    FOR_LOOP(viewer, MAX_PLAYERS)
        if (viewers & (1u << viewer))
            G_FowRevealCircle(viewer, ent, radius);
}

/* --- Fog modifiers ------------------------------------------------------- *
 * Direct state writes and started modifiers use the same three-state cell
 * contract. Modifiers are applied after unit sight so their state persists. */
#define MAX_FOG_MODIFIERS 256 // handles; bounded active map-script fog modifiers

static fogModifier_t *g_fog_modifiers[MAX_FOG_MODIFIERS];
static uint32_t g_num_fog_modifiers;

static void G_FowApplyModifierForPlayer(uint32_t player, fogModifier_t const *mod);

/* Start is observable immediately in Warcraft scripts. This matters for the
 * common reveal pattern that starts and destroys/stops a VISIBLE modifier in
 * the same trigger turn: exploration must still be recorded even if the
 * modifier is gone before the next simulation fog update. */
static void G_FowApplyModifierImmediately(fogModifier_t const *mod) {
    if (!mod || !G_FowReady() || !G_FowStateValid(mod->state) ||
        mod->player >= MAX_PLAYERS) {
        return;
    }
    FOR_LOOP(viewer, MAX_PLAYERS) {
        if (viewer != mod->player &&
            (!mod->use_shared_vision || !G_FowPlayersShareVision(viewer, mod->player))) {
            continue;
        }
        G_FowApplyModifierForPlayer(viewer, mod);
    }
}

void G_FogModifierStart(fogModifier_t *mod) {
    if (!mod) {
        return;
    }
    mod->started = true;
    FOR_LOOP(i, g_num_fog_modifiers) {
        if (g_fog_modifiers[i] == mod) {
            return;
        }
    }
    if (g_num_fog_modifiers < MAX_FOG_MODIFIERS) {
        g_fog_modifiers[g_num_fog_modifiers++] = mod;
        G_FowApplyModifierImmediately(mod);
    }
}

void G_FogModifierStop(fogModifier_t *mod) {
    if (!mod) {
        return;
    }
    mod->started = false;
    FOR_LOOP(i, g_num_fog_modifiers) {
        if (g_fog_modifiers[i] == mod) {
            g_fog_modifiers[i] = g_fog_modifiers[--g_num_fog_modifiers];
            return;
        }
    }
}

/* Rectangular writes use the same cell-state contract as circular reveals. */
static void G_FowSetBoxState(fowPlayerGrid_t *grid, box2_t const *box, uint32_t state) {
    uint32_t x0 = G_FowWorldToCellX(box->min.x);
    uint32_t y0 = G_FowWorldToCellY(box->min.y);
    uint32_t x1 = G_FowWorldToCellX(box->max.x);
    uint32_t y1 = G_FowWorldToCellY(box->max.y);

    if (x0 == FOW_INVALID_CELL || y0 == FOW_INVALID_CELL ||
        x1 == FOW_INVALID_CELL || y1 == FOW_INVALID_CELL) {
        return;
    }
#ifdef WC3_FOW_PACKED_MASK
    if (g_fow_fast && state == WC3_FOG_STATE_VISIBLE) {
        G_FowRevealPackedBox(grid, x0, y0, x1, y1);
        return;
    }
#endif
    for (uint32_t y = y0; y <= y1; y++) {
        for (uint32_t x = x0; x <= x1; x++) G_FowSetCellState(grid, G_FowCellIndex(x, y), state);
    }
}

/* Immediate JASS writes persist in the target grid even when no client currently consumes it. */
void G_FowSetStateRect(fogWrite_t const *fog, box2_t const *box) {
    if (!fog || fog->player >= MAX_PLAYERS || !box ||
        !G_FowReady() || !G_FowStateValid(fog->state))
        return;
    FOR_LOOP(viewer, MAX_PLAYERS) {
        if (viewer != fog->player && (!fog->shared || !G_FowPlayersShareVision(viewer, fog->player)))
            continue;
        G_FowSetBoxState(&level.fow.players[viewer], box, fog->state);
    }
}

/* Radius and location natives share one authoritative circular state path. */
void G_FowSetStateRadius(fogWrite_t const *fog, vec2_t const *center, float radius) {
    uint32_t cx, cy;
    int cells;
    if (!fog || fog->player >= MAX_PLAYERS || !center ||
        !G_FowReady() || !G_FowStateValid(fog->state))
        return;
    cx = G_FowWorldToCellX(center->x);
    cy = G_FowWorldToCellY(center->y);
    if (cx == FOW_INVALID_CELL || cy == FOW_INVALID_CELL)
        return;
    cells = G_FowRadiusCells(radius);
    fogDisk_t disk = { cx, cy, fog->state, cells };
    FOR_LOOP(viewer, MAX_PLAYERS) {
        if (viewer != fog->player && (!fog->shared || !G_FowPlayersShareVision(viewer, fog->player)))
            continue;
        G_FowSetDiskState(&level.fow.players[viewer], &disk);
    }
}

static void G_FowApplyModifierForPlayer(uint32_t player, fogModifier_t const *mod) {
    fowPlayerGrid_t *grid = &level.fow.players[player];
    if (mod->is_rect) {
        G_FowSetBoxState(grid, &mod->rect, mod->state);
    } else {
        uint32_t cx = G_FowWorldToCellX(mod->center.x);
        uint32_t cy = G_FowWorldToCellY(mod->center.y);
        if (cx == FOW_INVALID_CELL || cy == FOW_INVALID_CELL) {
            return;
        }
#ifdef WC3_FOW_PACKED_MASK
        if (g_fow_fast && mod->state == WC3_FOG_STATE_VISIBLE)
            G_FowRevealPacked(grid, cx, cy, G_FowRadiusCells(mod->radius));
        else
#endif
            G_FowSetDiskState(grid, &(fogDisk_t){ cx, cy, mod->state, G_FowRadiusCells(mod->radius) });
    }
}

static void G_FowApplyModifiers(uint32_t viewers) {
    FOR_LOOP(i, g_num_fog_modifiers) {
        fogModifier_t const *mod = g_fog_modifiers[i];
        if (!mod || !mod->started || !G_FowStateValid(mod->state) ||
            mod->player >= MAX_PLAYERS) {
            continue;
        }
        FOR_LOOP(viewer, MAX_PLAYERS)
            if ((viewers & (1u << viewer)) &&
                (viewer == mod->player || (mod->use_shared_vision && G_FowPlayersShareVision(viewer, mod->player))))
                G_FowApplyModifierForPlayer(viewer, mod);
    }
}

void G_FowShutdown(void) {
    free(g_fow_cached_blockers); g_fow_cached_blockers = NULL;
    memset(g_fow_sight_cache, 0, sizeof(g_fow_sight_cache));
    g_fow_capture = NULL; g_fow_cache_enabled = g_fow_cache_failed = false;
    memset(&g_fow_cache_stats, 0, sizeof(g_fow_cache_stats));
    free(g_fow_blocker_prefix); g_fow_blocker_prefix = NULL; g_fow_prefix_capacity = 0;
    g_fow_prefix_attempted = g_fow_prefix_valid = false;
    FOR_LOOP(player, MAX_PLAYERS) {
        fowPlayerGrid_t *grid = &level.fow.players[player];
        SAFE_DELETE(grid->visible, gi.MemFree);
        SAFE_DELETE(grid->explored, gi.MemFree);
        SAFE_DELETE(grid->visible_rows, gi.MemFree);
        SAFE_DELETE(grid->dirty_visible_rows, gi.MemFree);
        SAFE_DELETE(grid->dirty_explored_rows, gi.MemFree);
#ifdef WC3_FOW_PACKED_MASK
        SAFE_DELETE(grid->packed_visible, gi.MemFree);
        SAFE_DELETE(grid->packed_explored, gi.MemFree);
        grid->packed_stride = 0;
#endif
    }
    SAFE_DELETE(level.fow.blocked, gi.MemFree);
    SAFE_DELETE(level.fow.rim_cells, gi.MemFree);
    memset(&level.fow, 0, sizeof(level.fow));
    memset(g_fog_modifiers, 0, sizeof(g_fog_modifiers));
    g_num_fog_modifiers = 0;
    g_fow_blocker_hash = 0;
    g_fow_blocker_count = 0;
    g_fow_blockers_valid = false;
    g_fow_blockers_dirty = true;
}

void G_FowInit(void) {
    uint32_t cells;

    G_FowShutdown();
    g_fow_blockers_valid = false;
    g_fow_blockers_dirty = true;
    level.fow.bounds = CM_GetWorldBounds();
    level.fow.width = (uint32_t)ceilf((level.fow.bounds.max.x - level.fow.bounds.min.x) / (float)FOW_CELL_SIZE);
    level.fow.height = (uint32_t)ceilf((level.fow.bounds.max.y - level.fow.bounds.min.y) / (float)FOW_CELL_SIZE);
    level.fow.width = MAX(level.fow.width, 1);
    level.fow.height = MAX(level.fow.height, 1);
    cells = G_FowCellCount();
    level.fow.blocked = gi.MemAlloc(cells);
    level.fow.rim_cells = gi.MemAlloc(cells * sizeof(*level.fow.rim_cells));
    ARRAY_COUNT(level.fow.rim_cells) = cells;
    if (!level.fow.blocked || !level.fow.rim_cells) {
        fprintf(stderr, "G_FowInit: failed to allocate %u-cell blocker grid and rim list\n", cells);
        G_FowShutdown();
        return;
    }
    memset(level.fow.blocked, 0, cells);

    FOR_LOOP(player, MAX_PLAYERS) {
        fowPlayerGrid_t *grid = &level.fow.players[player];
        grid->visible = gi.MemAlloc(cells);
        grid->explored = gi.MemAlloc(cells);
        grid->visible_rows = gi.MemAlloc(level.fow.height);
#ifdef WC3_FOW_PACKED_MASK
        grid->packed_stride = (level.fow.width + 15) >> 4;
        grid->packed_visible = gi.MemAlloc(grid->packed_stride * level.fow.height * sizeof(*grid->packed_visible));
        grid->packed_explored = gi.MemAlloc(grid->packed_stride * level.fow.height * sizeof(*grid->packed_explored));
#endif
        grid->dirty_visible_rows = gi.MemAlloc(level.fow.height);
        grid->dirty_explored_rows = gi.MemAlloc(level.fow.height);
        if (!grid->visible || !grid->explored || !grid->visible_rows ||
#ifdef WC3_FOW_PACKED_MASK
            !grid->packed_visible ||
            !grid->packed_explored ||
#endif
            !grid->dirty_visible_rows || !grid->dirty_explored_rows) {
            G_FowShutdown();
            return;
        }
        memset(grid->visible, 0, cells);
        memset(grid->explored, 0, cells);
#ifdef WC3_FOW_PACKED_MASK
        memset(grid->packed_visible, 0, grid->packed_stride * level.fow.height * sizeof(*grid->packed_visible));
        memset(grid->packed_explored, 0, grid->packed_stride * level.fow.height * sizeof(*grid->packed_explored));
#endif
        memset(grid->visible_rows, 0, level.fow.height);
        memset(grid->dirty_visible_rows, 1, level.fow.height);
        memset(grid->dirty_explored_rows, 1, level.fow.height);
    }
}

/* Mark a player grid as consumed before its first authoritative update. */
void G_FowConnectPlayer(uint32_t player) {
    if (player < MAX_PLAYERS)
        level.fow.players[player].client_connected = true;
}

void G_FowUpdate(void) {
    g_visibility_fast = gi.CvarString && atoi(gi.CvarString("wc3_visibility_fast", BZ_VISIBILITY_FAST_DEFAULT));
    g_fow_prefix_attempted = g_fow_prefix_valid = false;
    uint32_t owner_viewers[MAX_PLAYERS] = { 0 };
    uint32_t viewers = 0;

    if (!G_FowReady()) {
        return;
    }

    FOR_LOOP(player, MAX_PLAYERS)
        if (level.fow.players[player].client_connected)
            viewers |= 1u << player;
    if (!viewers)
        return;
#ifdef WC3_FOW_PACKED_MASK
    g_fow_fast = atoi(gi.CvarString("wc3_fow_fast", "0"));
#endif
    FOR_LOOP(owner, MAX_PLAYERS)
        FOR_LOOP(viewer, MAX_PLAYERS)
            if ((viewers & (1u << viewer)) && G_FowPlayersShareVision(viewer, owner))
                owner_viewers[owner] |= 1u << viewer;

    if (G_FowBlockersChanged()) {
        G_FowRebuildBlockers();
    }
    G_FowPrepareSightCache();
    FOR_LOOP(player, MAX_PLAYERS) {
        fowPlayerGrid_t *grid = &level.fow.players[player];
        if (!(viewers & (1u << player))) {
            continue;
        }
        G_FowClearVisible(grid);
    }

    FOR_LOOP(i, globals.num_edicts) {
        edict_t const *ent = &g_edicts[i];
        float radius;
        uint32_t unit_viewers;

        if (ent->s.player >= MAX_PLAYERS || !G_FowEntityIsRevealer(ent)) {
            continue;
        }
        unit_viewers = owner_viewers[ent->s.player] | (ent->shared_vision & viewers);
        if (!unit_viewers) continue;
        radius = G_FowEntitySightRadius(ent);
        G_FowRevealForViewers(ent, radius, unit_viewers);
    }

    /* Timed spell reveals are not ordinary sight sources: Far Sight ignores
     * terrain line-of-sight blockers and must survive this frame's visible-grid
     * rebuild. Its save-safe thinker owns only lifetime/state; apply the disk
     * here, after unit sight and before script fog modifiers. */
    FOR_LOOP(i, globals.num_edicts) {
        edict_t const *ent = &g_edicts[i];
        if (!ent->inuse || ent->think != far_sight_think || ent->s.player >= MAX_PLAYERS ||
            G_Time() >= ent->spawn_time || ent->collision <= 0.0f) {
            continue;
        }
        G_FowSetStateRadius(&(fogWrite_t){ ent->s.player, WC3_FOG_STATE_VISIBLE, true },
                            &ent->s.origin2, ent->collision);
    }

    /* Glyph vision is bounded in simulation time and does not leak to other
     * players. Apply after the per-frame visibility grid has been rebuilt. */
    FOR_LOOP(player, MAX_PLAYERS) {
        if (level.fow.omniscience_until[player] > G_Time() && (viewers & (1u << player)))
            G_FowSetStateRect(&(fogWrite_t){ player, WC3_FOG_STATE_VISIBLE, false },
                              &level.fow.bounds);
    }
    G_FowApplyModifiers(viewers);
    if (atoi(gi.CvarString("wc3_fow_stats", "0"))) {
        g_fow_cache_stats.updates++;
        if (level.time - g_fow_cache_stats.start >= 1000) {
            fprintf(stderr, "[FOW_CACHE] updates=%u hits=%u misses=%u replaced=%u bypasses=%u\n", g_fow_cache_stats.updates, g_fow_cache_stats.hits, g_fow_cache_stats.misses, g_fow_cache_stats.replaced, g_fow_cache_stats.bypasses);
            memset(&g_fow_cache_stats, 0, sizeof(g_fow_cache_stats)); g_fow_cache_stats.start = level.time;
        }
    }
}

/* FogEnable(false) reveals the whole map for this player, ordinary units
   included (matches WC3 cinematic behavior). Gameplay invisibility remains
   detector-gated. RDF_NOFOG is the client-visual flag set by the FogEnable
   native; honor it for server-side unit visibility too, otherwise units in the
   (still-fogged) cinematic area are never networked and the scene renders
   without its actors. */
static bool G_FowPlayerFogDisabled(uint32_t player) {
    gameClient_t *client = G_GetPlayerClientByNumber(player);
    return client && (client->ps.rdflags & RDF_NOFOG);
}

/* External observations use this same authoritative grid when retiring a
 * remembered building after its last observed location becomes visible. */
bool G_FowPointIsVisible(uint32_t player, vec2_t const *point) {
    uint32_t x, y;
    fowPlayerGrid_t const *grid;
    if (!point || player >= MAX_PLAYERS) return false;
    if (!G_FowReady() || G_FowPlayerFogDisabled(player)) return true;
    x = G_FowWorldToCellX(point->x);
    y = G_FowWorldToCellY(point->y);
    if (x == FOW_INVALID_CELL || y == FOW_INVALID_CELL) return false;
    grid = &level.fow.players[player];
#ifdef WC3_FOW_PACKED_MASK
    if (g_fow_fast) return G_FowPackedAt(grid->packed_visible, grid, x, y);
#endif
    return grid->visible && grid->visible[y * level.fow.width + x] != 0;
}

/* Hover information is interactive gameplay state, so unlike explored
 * scenery it is exposed only while the entity is actively visible. */
bool G_FowPlayerCanHoverEntity(uint32_t player, edict_t const *ent) {
    uint32_t x, y, index;
    fowPlayerGrid_t const *grid;
    bool const world_item = G_IsItem(ent);

    if (!ent || player >= MAX_PLAYERS || !G_FowReady()) {
        return true;
    }
    /* World items have no owner; map-created items often retain player zero
     * from edict initialization and must still obey their position in fog. */
    if (!world_item && ent->s.player < MAX_PLAYERS && G_FowPlayersShareVision(player, ent->s.player)) {
        return true;
    }
    if (!world_item && G_UnitIsForcedVisibleToPlayer(ent, player)) return true;
    if (S_UnitIsInvisibleToPlayer(ent, player)) {
        return false;
    }
    if (G_FowPlayerFogDisabled(player)) {
        return true;
    }
    x = G_FowWorldToCellX(ent->s.origin.x);
    y = G_FowWorldToCellY(ent->s.origin.y);
    if (x == FOW_INVALID_CELL || y == FOW_INVALID_CELL) {
        return false;
    }
    index = y * level.fow.width + x;
    grid = &level.fow.players[player];
#ifdef WC3_FOW_PACKED_MASK
    if (g_fow_fast)
        return G_FowPackedAt(grid->packed_visible, grid, x, y);
#endif
    return grid->visible && grid->visible[index] != 0;
}

bool G_FowPlayerCanSeeEntity(uint32_t player, edict_t const *ent) {
    uint32_t x, y, index;
    fowPlayerGrid_t const *grid;
    bool const world_item = G_IsItem(ent);

    if (!ent || player >= MAX_PLAYERS || !G_FowReady()) {
        return true;
    }
    /* Items are neutral world objects, not owned entities. Their edict's
     * default player number must not make pickups visible through fog. */
    if (!world_item && ent->s.player < MAX_PLAYERS && G_FowPlayersShareVision(player, ent->s.player)) {
        return true;
    }
    if (!world_item && G_UnitIsForcedVisibleToPlayer(ent, player)) return true;
    if (S_UnitIsInvisibleToPlayer(ent, player)) {
        return false;
    }
    if (G_FowPlayerFogDisabled(player)) {
        return true;
    }
    x = G_FowWorldToCellX(ent->s.origin.x);
    y = G_FowWorldToCellY(ent->s.origin.y);
    if (x == FOW_INVALID_CELL || y == FOW_INVALID_CELL) {
        return false;
    }
    index = y * level.fow.width + x;
    grid = &level.fow.players[player];
    /* Explored scenery stays shrouded after sight leaves; sending unexplored map-wide doodads saturated snapshots. */
    if ((ent->svflags & SVF_STATIC_SCENERY) || (ent->runtime.flags & UNIT_BALANCE_BUILDING)) {
#ifdef WC3_FOW_PACKED_MASK
        if (g_fow_fast)
            return G_FowPackedAt(grid->packed_explored, grid, x, y);
#endif
        return grid->explored && grid->explored[index] != 0;
    }
#ifdef WC3_FOW_PACKED_MASK
    if (g_fow_fast)
        return G_FowPackedAt(grid->packed_visible, grid, x, y);
#endif
    return grid->visible && grid->visible[index] != 0;
}

static uint8_t *G_FowPlaneForFlags(fowPlayerGrid_t *grid, uint32_t flags, uint32_t plane) {
    if (plane == FOW_MSG_VISIBLE_PLANE && (flags & FOW_MSG_VISIBLE_PLANE)) {
        return grid->visible;
    }
    if (plane == FOW_MSG_EXPLORED_PLANE && (flags & FOW_MSG_EXPLORED_PLANE)) {
        return grid->explored;
    }
    return NULL;
}

typedef struct { fowPlayerGrid_t *grid; uint8_t *planes[2]; uint32_t plane_count, width, first_row, row_count, plane_bits, x, y, plane_index; uint8_t *plane; } fowPackCtx_t;

static uint8_t G_FowPackBit(uint32_t index, void *ctx) {
    fowPackCtx_t *c = ctx;
    uint8_t v;
    (void)index; /* MSG_EncodeRLE reads sequentially, so x/y/plane track the position with no division. */
#ifdef WC3_FOW_PACKED_MASK
    if (g_fow_fast && c->plane == c->grid->visible) v = G_FowPackedAt(c->grid->packed_visible, c->grid, c->x, c->y);
    else if (g_fow_fast && c->plane == c->grid->explored) v = G_FowPackedAt(c->grid->packed_explored, c->grid, c->x, c->y);
    else
#endif
        v = c->plane[c->y * level.fow.width + c->x] ? 1 : 0;
    if (++c->x == c->width) {
        c->x = 0;
        if (++c->y == c->first_row + c->row_count) {
            c->y = c->first_row;
            if (c->plane_index + 1 < c->plane_count) c->plane = c->planes[++c->plane_index];
        }
    }
    return v;
}

static uint32_t G_FowPackRows(fowPlayerGrid_t *grid,
                           uint32_t flags,
                           uint32_t first_row,
                           uint32_t row_count,
                           uint8_t *payload,
                           uint32_t payload_size)
{
    uint32_t planes[] = { FOW_MSG_VISIBLE_PLANE, FOW_MSG_EXPLORED_PLANE };
    fowPackCtx_t c;
    if (!payload || payload_size < 2) return 0;
    c.grid = grid; c.plane_count = 0; c.width = level.fow.width; c.first_row = first_row;
    c.row_count = row_count; c.plane_bits = level.fow.width * row_count;
    c.x = 0; c.y = first_row; c.plane_index = 0; c.plane = NULL;
    FOR_LOOP(plane_index, sizeof(planes) / sizeof(planes[0])) {
        uint8_t *plane = G_FowPlaneForFlags(grid, flags, planes[plane_index]);
        if (plane) c.planes[c.plane_count++] = plane;
    }
    if (!c.plane_count || !c.plane_bits) return 0;
    c.plane = c.planes[0];
    return MSG_EncodeRLE(payload, payload_size, c.plane_bits * c.plane_count, G_FowPackBit, &c);
}

static void G_FowWriteRows(edict_t *ent, uint32_t player, uint32_t flags, uint32_t first_row, uint32_t row_count) {
    uint8_t payload[FOW_CHUNK_TARGET_BYTES];
    uint32_t plane_count = 0;
    uint32_t payload_bytes;
    pfWriteData_t data;

    if (!ent || player >= MAX_PLAYERS || !G_FowReady() || row_count == 0 ||
        first_row >= level.fow.height) {
        return;
    }
    row_count = MIN(row_count, level.fow.height - first_row);
    if (flags & FOW_MSG_VISIBLE_PLANE) {
        plane_count++;
    }
    if (flags & FOW_MSG_EXPLORED_PLANE) {
        plane_count++;
    }
    if (plane_count == 0 || 1 + level.fow.width * row_count * plane_count > sizeof(payload)) {
        return;
    }

    payload_bytes = G_FowPackRows(&level.fow.players[player],
                                  flags,
                                  first_row,
                                  row_count,
                                  payload,
                                  sizeof(payload));
    if (!payload_bytes) {
        return;
    }

    gi.Write(PF_BYTE, &(int32_t){ svc_fogofwar });
    gi.Write(PF_BYTE, &(int32_t){ flags | FOW_MSG_RLE });
    gi.Write(PF_SHORT, &(int32_t){ level.fow.width });
    gi.Write(PF_SHORT, &(int32_t){ level.fow.height });
    gi.Write(PF_SHORT, &(int32_t){ first_row });
    gi.Write(PF_SHORT, &(int32_t){ row_count });
    gi.Write(PF_SHORT, &(int32_t){ payload_bytes });
    data = (pfWriteData_t){ payload, payload_bytes };
    gi.Write(PF_DATA, &data);
    gi.unicast(ent);
}

static uint32_t G_FowRowsPerChunk(uint32_t flags) {
    uint32_t plane_count = 0;

    if (flags & FOW_MSG_VISIBLE_PLANE) {
        plane_count++;
    }
    if (flags & FOW_MSG_EXPLORED_PLANE) {
        plane_count++;
    }
    if (!level.fow.width || plane_count == 0) {
        return 1;
    }
    return MAX(1, (FOW_CHUNK_TARGET_BYTES - 1) / (level.fow.width * plane_count));
}

void G_FowSendFull(edict_t *ent) {
    uint32_t player;
    uint32_t rows_per_chunk;

    if (!ent || !ent->client || !G_FowReady()) {
        return;
    }
    player = ent->client->ps.number;
    if (player >= MAX_PLAYERS) {
        return;
    }
    G_FowConnectPlayer(player);
    rows_per_chunk = G_FowRowsPerChunk(FOW_MSG_VISIBLE_PLANE | FOW_MSG_EXPLORED_PLANE);
    for (uint32_t row = 0; row < level.fow.height; row += rows_per_chunk) {
        G_FowWriteRows(ent,
                       player,
                       FOW_MSG_FULL | FOW_MSG_VISIBLE_PLANE | FOW_MSG_EXPLORED_PLANE,
                       row,
                       MIN(rows_per_chunk, level.fow.height - row));
    }
}

static void G_FowSendDirtyPlane(edict_t *ent,
                                uint32_t player,
                                uint8_t *dirty_rows,
                                uint32_t plane_flag)
{
    uint32_t rows_per_chunk;
    uint32_t row = 0;

    if (!dirty_rows) {
        return;
    }
    rows_per_chunk = G_FowRowsPerChunk(plane_flag);
    while (row < level.fow.height) {
        while (row < level.fow.height && !dirty_rows[row]) {
            row++;
        }
        if (row >= level.fow.height) {
            break;
        }
        uint32_t first = row;
        uint32_t count = 0;
        while (row < level.fow.height && dirty_rows[row] && count < rows_per_chunk) {
            dirty_rows[row] = 0;
            row++;
            count++;
        }
        G_FowWriteRows(ent, player, plane_flag, first, count);
    }
}

void G_FowSendDeltas(void) {
    if (!G_FowReady()) {
        return;
    }

    FOR_LOOP(player, MIN((uint32_t)game.max_clients, (uint32_t)MAX_PLAYERS)) {
        edict_t *ent = G_GetPlayerEntityByNumber(player);
        if (!level.fow.players[player].client_connected || !ent || !ent->client) {
            continue;
        }
        G_FowSendDirtyPlane(ent,
                            player,
                            level.fow.players[player].dirty_visible_rows,
                            FOW_MSG_VISIBLE_PLANE);
        G_FowSendDirtyPlane(ent,
                            player,
                            level.fow.players[player].dirty_explored_rows,
                            FOW_MSG_EXPLORED_PLANE);
    }
}
