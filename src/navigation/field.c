/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include "field.h"
#include "nav_private.h"
#include "../entity.h"
#include "../sched.h"
#include "../perf.h"
#include "../map/public/tile.h"
#include "../game/public/game.h"
#include "../lib/public/pqueue.h"
#include "../lib/public/mem.h"

#include <string.h>
#include <assert.h>
#include <math.h>


#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))
#define MAX_ENTS_PER_CHUNK  (4096)
#define SEARCH_BUFFER       (16.0f)
#define IDX(r, width, c)    ((r) * (width) + (c))

PQUEUE_TYPE(coord, struct coord)
PQUEUE_IMPL(static, coord, struct coord)

PQUEUE_TYPE(td, struct tile_desc)
PQUEUE_IMPL(static, td, struct tile_desc)

KHASH_MAP_INIT_INT(aabb, struct aabb)

KHASH_DECLARE(id, khint32_t, int)

struct box_xz{
    float x_min, x_max;
    float z_min, z_max;
};

struct region{
    /* Top-left coordinate */
    struct tile_desc base;
    size_t r, c;
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int manhattan_dist(struct coord a, struct coord b)
{
    int dr = abs(a.r - b.r);
    int dc = abs(a.c - b.c);
    return dr + dc;
}

static int field_compare_tiles(void *a, void *b)
{
    struct coord *ac = a;
    struct coord *bc = b;
    return !((bc->r == ac->r) && (bc->c == ac->c));
}

static int field_compare_tds(void *a, void *b)
{
    struct tile_desc *ac = a;
    struct tile_desc *bc = b;
    return !((bc->chunk_r == ac->chunk_r) 
          && (bc->chunk_c == ac->chunk_c)
          && (bc->tile_r  == ac->tile_r)
          && (bc->tile_c  == ac->tile_c));
}

static bool field_tile_passable(const struct nav_chunk *chunk, struct coord tile)
{
    if(chunk->cost_base[tile.r][tile.c] == COST_IMPASSABLE)
        return false;
    if(chunk->blockers[tile.r][tile.c] > 0)
        return false;
    return true;
}

static bool tile_outside_region(struct map_resolution res, struct region region, 
                                struct tile_desc td)
{
    int dr, dc;
    M_Tile_Distance(res, &region.base, &td, &dr, &dc);
    if(dr < 0 || dr >= region.r)
        return true;
    if(dc < 0 || dc >= region.c)
        return true;
    return false;
}

static bool ents_diplomacy_state(int fac_a, int fac_b,
                                 enum diplomacy_state *out,
                                 struct nav_unit_query_ctx *ctx)
{
    if(ctx) {
        assert(ARR_SIZE(*ctx->diptable) == MAX_FACTIONS);
        return G_GetDiplomacyStateFrom(
            (enum diplomacy_state (*)[MAX_FACTIONS])ctx->diptable, fac_a, fac_b, out);
    }else{
        return G_GetDiplomacyState(fac_a, fac_b, out);
    }
}

static uint16_t enemy_faction_from(int faction_id, struct nav_unit_query_ctx *ctx)
{
    uint16_t ret = 0;
    for(int i = 0; i < MAX_FACTIONS; i++) {

        enum diplomacy_state ds;
        if(!ents_diplomacy_state(i, faction_id, &ds, ctx))
            continue;
        if(ds != DIPLOMACY_STATE_WAR)
            continue;
        ret |= (0x1 << i);
    }
    return ret;
}

static uint16_t enemies_for_faction(int faction_id, struct nav_unit_query_ctx *ctx)
{
    uint16_t ret = 0;
    if(faction_id != FACTION_ID_NONE) {
        if(ctx) {
            ret = enemy_faction_from(faction_id, ctx);
        }else{
            ret = G_GetEnemyFactions(faction_id);
        }
    }
    return ret;
}

static bool field_tile_passable_no_enemies(
    const struct nav_chunk *chunk, 
    struct coord            tile, 
    uint16_t                enemies)
{
    if(chunk->cost_base[tile.r][tile.c] == COST_IMPASSABLE)
        return false;

    bool enemies_only = true;
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(chunk->factions[i][tile.r][tile.c] && !(enemies & (0x1 << i))) {
            enemies_only = false;
            break;
        }
    }

    if(enemies_only)
        return true;

    if(chunk->blockers[tile.r][tile.c] > 0)
        return false;
    return true;
}

static int field_neighbours_grid(
    const struct nav_chunk    *chunk, 
    struct coord               coord, 
    bool                       only_passable, 
    int                        faction_id,
    struct nav_unit_query_ctx *ctx,
    struct coord              *out_neighbours, 
    uint8_t                   *out_costs)
{
    int ret = 0;
    uint16_t enemies = 0;
    if(faction_id != FACTION_ID_NONE) {
        enemies = enemies_for_faction(faction_id, ctx);
    }

    for(int r = -1; r <= 1; r++) {
    for(int c = -1; c <= 1; c++) {

        int abs_r = coord.r + r;
        int abs_c = coord.c + c;

        if(abs_r < 0 || abs_r >= FIELD_RES_R)
            continue;
        if(abs_c < 0 || abs_c >= FIELD_RES_C)
            continue;
        if(r == 0 && c == 0)
            continue;
        if((r == c) || (r == -c)) /* diag */
            continue;

        if(only_passable) {
            bool passable;
            if(faction_id == FACTION_ID_NONE) {
                passable = field_tile_passable(chunk, (struct coord){abs_r, abs_c});
            }else{
                passable = field_tile_passable_no_enemies(chunk, (struct coord){abs_r, abs_c}, enemies);
            }
            if(!passable)
                continue;
        }

        out_neighbours[ret] = (struct coord){abs_r, abs_c};
        out_costs[ret] = chunk->cost_base[abs_r][abs_c];

        ret++;
    }}
    assert(ret < 9);
    return ret;
}

static int field_neighbours_grid_global(
    const struct nav_private *priv, 
    enum nav_layer            layer,
    struct tile_desc          coord, 
    bool                      only_passable, 
    uint16_t                  enemies,
    struct tile_desc         *out_neighbours, 
    uint8_t                  *out_costs)
{
    int ret = 0;

    struct map_resolution res;
    N_GetResolution(priv, &res);

    for(int r = -1; r <= 1; r++) {
    for(int c = -1; c <= 1; c++) {

        if(r == 0 && c == 0)
            continue;
        if((r == c) || (r == -c)) /* diag */
            continue;

        struct tile_desc curr = coord;
        if(!M_Tile_RelativeDesc(res, &curr, c, r))
            continue;

        struct nav_chunk *chunk 
            = &priv->chunks[layer][IDX(curr.chunk_r, priv->width, curr.chunk_c)];
        if(only_passable) {

            bool passable;
            struct coord tile = (struct coord){curr.tile_r, curr.tile_c};

            if(enemies == 0) {
                passable = field_tile_passable(chunk, tile);
            }else{
                passable = field_tile_passable_no_enemies(chunk, tile, enemies);
            }
            if(!passable)
                continue;
        }

        out_neighbours[ret] = curr;
        out_costs[ret] = chunk->cost_base[curr.tile_r][curr.tile_c];

        ret++;
    }}
    assert(ret < 9);
    return ret;
}

static int field_neighbours_grid_los(
    const struct nav_chunk    *chunk,
    const struct LOS_field    *los, 
    int                        faction_id,
    struct coord            coord, 
    struct nav_unit_query_ctx *ctx,
    struct coord              *out_neighbours, 
    uint8_t                   *out_costs)
{
    int ret = 0;
    uint16_t enemies = 0;
    if(faction_id != FACTION_ID_NONE) {
        enemies = enemy_faction_from(faction_id, ctx);
    }

    for(int r = -1; r <= 1; r++) {
    for(int c = -1; c <= 1; c++) {

        int abs_r = coord.r + r;
        int abs_c = coord.c + c;

        if(abs_r < 0 || abs_r >= FIELD_RES_R)
            continue;
        if(abs_c < 0 || abs_c >= FIELD_RES_C)
            continue;
        if(r == 0 && c == 0)
            continue;
        if((r == c) || (r == -c)) /* diag */
            continue;
        if(los->field[abs_r][abs_c].wavefront_blocked)
            continue;

        out_neighbours[ret] = (struct coord){abs_r, abs_c};
        out_costs[ret] = chunk->cost_base[abs_r][abs_c];

        bool passable;
        if(faction_id == FACTION_ID_NONE) {
            passable = field_tile_passable(chunk, (struct coord){abs_r, abs_c});
        }else{
            passable = field_tile_passable_no_enemies(chunk, (struct coord){abs_r, abs_c}, enemies);
        }

        if(!passable) {
            out_costs[ret] = COST_IMPASSABLE;
        }
        ret++;
    }}
    assert(ret < 9);
    return ret;
}

static enum flow_dir field_flow_dir(
    const int    rdim,
    const int    cdim,
    const float  integration_field[], 
    struct coord coord)
{
    float min_cost = INFINITY;
    const int r = coord.r;
    const int c = coord.c;

    if(r > 0)
        min_cost = MIN(min_cost, integration_field[(r-1) * rdim + c]);

    if(r < (rdim-1))
        min_cost = MIN(min_cost, integration_field[(r+1) * rdim + c]);

    if(c > 0)
        min_cost = MIN(min_cost, integration_field[r * rdim + (c-1)]);

    if(c < (cdim-1))
        min_cost = MIN(min_cost, integration_field[r * rdim + (c + 1)]);

    /* Diagonal directions are allowed only when _both_ the side 
     * tiles sharing an edge with the corner tile are passable. 
     * This is so that the flow vector never causes an entity 
     * to move from a passable region to an impassable one. */

    if(r > 0 && c > 0
    && integration_field[(r-1) * rdim + c] < INFINITY
    && integration_field[r * rdim + (c-1)] < INFINITY)
        min_cost = MIN(min_cost, integration_field[(r-1) * rdim + (c-1)]);

    if(r > 0 && c < (cdim-1)
    && integration_field[(r-1) * rdim + c] < INFINITY
    && integration_field[r * rdim + (c + 1)] < INFINITY)
        min_cost = MIN(min_cost, integration_field[(r-1) * rdim + (c + 1)]);

    if(r < (rdim-1) && c > 0
    && integration_field[(r+1) * rdim + c] < INFINITY
    && integration_field[r * rdim + (c-1)] < INFINITY)
        min_cost = MIN(min_cost, integration_field[(r+1) * rdim + (c-1)]);

    if(r < (rdim-1) && c < (cdim-1)
    && integration_field[(r+1) * rdim + c] < INFINITY
    && integration_field[r * rdim + (c + 1)] < INFINITY)
        min_cost = MIN(min_cost, integration_field[(r+1) * rdim + (c+1)]);

    assert(min_cost < INFINITY);

    /* Prioritize the cardinal directions over the diagonal ones */
    if(r > 0 
    && integration_field[(r-1) * rdim + c] == min_cost)
        return FD_N; 
    else if(r < (rdim-1) 
    && integration_field[(r+1) * rdim + c] == min_cost)
        return FD_S;
    else if(c < (cdim-1) 
    && integration_field[r * rdim + (c+1)] == min_cost)
        return FD_E;
    else if(c > 0 
    && integration_field[r * rdim + (c-1)] == min_cost)
        return FD_W;
    else if(r > 0 && c > 0 
    && integration_field[(r-1) * rdim + (c-1)] == min_cost)
        return FD_NW; 
    else if(r > 0 && c < (cdim-1) 
    && integration_field[(r-1) * rdim + (c+1)] == min_cost)
        return FD_NE;
    else if(r < (rdim-1) && c > 0 
    && integration_field[(r+1) * rdim + (c-1)] == min_cost)
        return FD_SW;
    else if(r < (rdim-1) && c < (rdim-1) 
    && integration_field[(r+1) * rdim + (c+1)] == min_cost)
        return FD_SE;
    else {
        assert(0);
        return 0;
    }
}

static bool field_is_los_corner(
    struct coord  cell, 
    const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C],
    const uint16_t blockers_field[FIELD_RES_R][FIELD_RES_C])
{
    if(cell.r > 0 && cell.r < FIELD_RES_R-1) {

        bool left_blocked  = cost_field    [cell.r - 1][cell.c] == COST_IMPASSABLE
                          || blockers_field[cell.r - 1][cell.c] > 0;
        bool right_blocked = cost_field    [cell.r + 1][cell.c] == COST_IMPASSABLE
                          || blockers_field[cell.r + 1][cell.c] > 0;
        if(left_blocked ^ right_blocked)
            return true;
    }

    if(cell.c > 0 && cell.c < FIELD_RES_C-1) {

        bool top_blocked = cost_field    [cell.r][cell.c - 1] == COST_IMPASSABLE
                        || blockers_field[cell.r][cell.c - 1] > 0;
        bool bot_blocked = cost_field    [cell.r][cell.c + 1] == COST_IMPASSABLE
                        || blockers_field[cell.r][cell.c + 1] > 0;
        if(top_blocked ^ bot_blocked)
            return true;
    }
    
    return false;
}

static void field_create_wavefront_blocked_line(
    struct tile_desc          target, 
    struct tile_desc          corner, 
    const struct nav_private *priv, 
    vec3_t                    map_pos, 
    struct LOS_field         *out_los)
{
    struct map_resolution res;
    N_GetResolution(priv, &res);

    /* First determine the slope of the LOS blocker line in the XZ plane */
    struct box target_bounds = M_Tile_Bounds(res, map_pos, target);
    struct box corner_bounds = M_Tile_Bounds(res, map_pos, corner);

    vec2_t target_center = (vec2_t){
        target_bounds.x - target_bounds.width / 2.0f,
        target_bounds.z + target_bounds.height / 2.0f
    };
    vec2_t corner_center = (vec2_t){
        corner_bounds.x - corner_bounds.width / 2.0f,
        corner_bounds.z + corner_bounds.height / 2.0f
    };

    vec2_t slope;
    PFM_Vec2_Sub(&target_center, &corner_center, &slope);
    PFM_Vec2_Normal(&slope, &slope);

    /* Now use Bresenham's line drawing algorithm to follow a line 
     * of the computed slope starting at the 'corner' until we hit the 
     * edge of the field. 
     * Multiply by 1_000 to convert slope to integer deltas, but keep 
     * 3 digits of precision after the decimal.*/
    int dx =  abs(slope.raw[0] * 1000);
    int dy = -abs(slope.raw[1] * 1000);
    int sx = slope.raw[0] > 0.0f ? 1 : -1;
    int sy = slope.raw[1] < 0.0f ? 1 : -1;
    int err = dx + dy, e2;

    struct coord curr = (struct coord){corner.tile_r, corner.tile_c};
    do {

        out_los->field[curr.r][curr.c].wavefront_blocked = 1;

        e2 = 2 * err;
        if(e2 >= dy) {
            err += dy;
            curr.c += sx;
        }
        if(e2 <= dx) {
            err += dx;
            curr.r += sy;
        }

    }while(curr.r >= 0 && curr.r < FIELD_RES_R && curr.c >= 0 && curr.c < FIELD_RES_C);
}

static void field_pad_wavefront(struct LOS_field *out_los)
{
    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        if(out_los->field[r][c].wavefront_blocked) {
        
            for(int rr = r-1; rr <= r+1; rr++) {
            for(int cc = c-1; cc <= c+1; cc++) {
            
                if(rr < 0 || rr > FIELD_RES_R-1)
                    continue;
                if(cc < 0 || cc > FIELD_RES_C-1)
                    continue;
                out_los->field[rr][cc].visible = 0;
            }}
        }
    }}
}

static void field_build_integration(
    pq_coord_t                *frontier, 
    const struct nav_chunk    *chunk, 
    int                        faction_id, 
    struct nav_unit_query_ctx *ctx,
    float                      inout[FIELD_RES_R][FIELD_RES_C])
{
    while(pq_size(frontier) > 0) {

        struct coord curr;
        pq_coord_pop(frontier, &curr);

        struct coord neighbours[8];
        uint8_t neighbour_costs[8];
        int num_neighbours = field_neighbours_grid(chunk, curr, true, faction_id, 
            ctx, neighbours, neighbour_costs);

        for(int i = 0; i < num_neighbours; i++) {

            float total_cost = inout[curr.r][curr.c] + neighbour_costs[i];
            if(total_cost < inout[neighbours[i].r][neighbours[i].c]) {

                inout[neighbours[i].r][neighbours[i].c] = total_cost;
                if(!pq_coord_contains(frontier, field_compare_tiles, neighbours[i]))
                    pq_coord_push(frontier, total_cost, neighbours[i]);
            }
        }
    }
}

/* Like 'field_build_integration', but supporting any sized square region 
 * which may straddle chunk boundaries.
 */
static void field_build_integration_region(
    pq_td_t                  *frontier,
    const struct nav_private *priv,
    enum nav_layer            layer,
    uint16_t                  enemies,
    struct region             region,
    float                     inout[])
{
    struct map_resolution res;
    N_GetResolution(priv, &res);

    while(pq_size(frontier) > 0) {

        struct tile_desc curr;
        pq_td_pop(frontier, &curr);

        struct tile_desc neighbours[8];
        uint8_t neighbour_costs[8];
        int num_neighbours = field_neighbours_grid_global(priv, layer, curr, true, enemies, 
            neighbours, neighbour_costs);

        int dr, dc;
        M_Tile_Distance(res, &region.base, &curr, &dr, &dc);
        assert(dr >= 0 && dr < region.r);
        assert(dc >= 0 && dc < region.c);

        for(int i = 0; i < num_neighbours; i++) {

            if(tile_outside_region(res, region, neighbours[i]))
                continue;

            int neighb_dr, neighb_dc;
            M_Tile_Distance(res, &region.base, &neighbours[i], &neighb_dr, &neighb_dc);
            assert(neighb_dr >= 0 && neighb_dr < region.r);
            assert(neighb_dc >= 0 && neighb_dc < region.c);

            assert(manhattan_dist((struct coord){dr, dc}, 
                (struct coord){neighb_dr, neighb_dc}) == 1);

            float total_cost = inout[dr * region.r+ dc] + neighbour_costs[i];
            if(total_cost < inout[neighb_dr * region.r + neighb_dc]) {

                inout[neighb_dr * region.r + neighb_dc] = total_cost;
                if(!pq_td_contains(frontier, field_compare_tds, neighbours[i]))
                    pq_td_push(frontier, total_cost, neighbours[i]);
            }
        }
    }
}

/* same as 'field_build_integration' but only impassable tiles 
 * will be added to the frontier 
 */
static void field_build_integration_nonpass(
    pq_coord_t                *frontier, 
    const struct nav_chunk    *chunk, 
    int                        faction_id, 
    struct nav_unit_query_ctx *ctx,
    float                      inout[FIELD_RES_R][FIELD_RES_C])
{
    while(pq_size(frontier) > 0) {

        struct coord curr;
        pq_coord_pop(frontier, &curr);

        struct coord neighbours[8];
        uint8_t neighbour_costs[8];
        int num_neighbours = field_neighbours_grid(chunk, curr, false, faction_id, 
            ctx, neighbours, neighbour_costs);

        for(int i = 0; i < num_neighbours; i++) {

            if(field_tile_passable(chunk, neighbours[i]))
                continue;

            float total_cost = inout[curr.r][curr.c] + neighbour_costs[i];
            if(total_cost < inout[neighbours[i].r][neighbours[i].c]) {

                inout[neighbours[i].r][neighbours[i].c] = total_cost;
                if(!pq_coord_contains(frontier, field_compare_tiles, neighbours[i]))
                    pq_coord_push(frontier, total_cost, neighbours[i]);
            }
        }
    }
}

/* Like 'field_build_integration_nonpass', but supporting any sized square region 
 * which may straddle chunk boundaries.
 */
static void field_build_integration_nonpass_region(
    pq_td_t                  *frontier,
    const struct nav_private *priv,
    enum nav_layer            layer,
    uint16_t                  enemies,
    struct region             region,
    float                     inout[])
{
    struct map_resolution res;
    N_GetResolution(priv, &res);

    while(pq_size(frontier) > 0) {

        struct tile_desc curr;
        pq_td_pop(frontier, &curr);

        struct tile_desc neighbours[8];
        uint8_t neighbour_costs[8];
        int num_neighbours = field_neighbours_grid_global(priv, layer, curr, false, enemies, 
            neighbours, neighbour_costs);

        int dr, dc;
        M_Tile_Distance(res, &region.base, &curr, &dr, &dc);
        assert(dr >= 0 && dr < region.r);
        assert(dc >= 0 && dc < region.c);

        for(int i = 0; i < num_neighbours; i++) {

            struct tile_desc neighb = neighbours[i];
            if(tile_outside_region(res, region, neighb))
                continue;

            struct nav_chunk *curr_chunk = 
                &priv->chunks[layer][neighb.chunk_r * priv->width + neighb.chunk_c];
            struct coord curr_coord = (struct coord){neighb.tile_r, neighb.tile_c};
            if(field_tile_passable(curr_chunk, curr_coord))
                continue;

            int neighb_dr, neighb_dc;
            M_Tile_Distance(res, &region.base, &neighb, &neighb_dr, &neighb_dc);
            assert(neighb_dr >= 0 && neighb_dr < region.r);
            assert(neighb_dc >= 0 && neighb_dc < region.c);

            float total_cost = inout[dr * region.r + dc] + neighbour_costs[i];
            if(total_cost < inout[neighb_dr * region.r + neighb_dc]) {

                inout[neighb_dr * region.r + neighb_dc] = total_cost;
                if(!pq_td_contains(frontier, field_compare_tds, neighb))
                    pq_td_push(frontier, total_cost, neighb);
            }
        }
    }
}

static void field_build_flow(float intf[FIELD_RES_R][FIELD_RES_C], struct flow_field *inout_flow)
{

    /* Build the flow field from the integration field. Don't touch any impassable tiles
     * as they may have already been set in the case that a single chunk is divided into
     * multiple passable 'islands', but a computed path takes us through more than one of
     * these 'islands'. */
    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        if(intf[r][c] == INFINITY)
            continue;

        if(intf[r][c] == 0.0f) {

            inout_flow->field[r][c].dir_idx = FD_NONE;
            continue;
        }

        inout_flow->field[r][c].dir_idx = field_flow_dir(FIELD_RES_R, FIELD_RES_C, 
            (const float*)intf, (struct coord){r, c});
    }}
}

/* Like 'field_build_flow', but potentially having an integration field that is a different
 * size than the flow field. In case the integration field is bigger than the field size,
 * a subset starting at (roff, coff) is used for building the flow field.
 */
static void field_build_flow_region(
    int                rdim, 
    int                cdim, 
    int                roff,
    int                coff,
    float              intf[], 
    struct flow_field *inout_flow)
{
    for(int r = 0; r < MIN(FIELD_RES_R, rdim); r++) {
    for(int c = 0; c < MIN(FIELD_RES_C, cdim); c++) {

        int infr = r + roff;
        int infc = c + coff;

        if(intf[infr * rdim + infc] == INFINITY)
            continue;

        if(intf[infr * rdim + infc] == 0.0f) {

            inout_flow->field[r][c].dir_idx = FD_NONE;
            continue;
        }

        inout_flow->field[r][c].dir_idx = field_flow_dir(rdim ,cdim, 
            intf, (struct coord){infr, infc});
    }}
}

static void set_flow_cell(enum flow_dir value, int r, int c, int rdim, int cdim, uint8_t *buff)
{
    size_t row_size = rdim / 2;
    size_t aligned_c = (c - (c % 2)) / 2;
    size_t byte_index = r * row_size + aligned_c;
    if(c % 2 == 1) {
        buff[byte_index] = (buff[byte_index] & 0xf0) | (value << 0);
    }else{
        buff[byte_index] = (buff[byte_index] & 0x0f) | (value << 4);
    }
}

/* Build an arbitrary-sized flow field not aligned to chunk boundaries.
 */
static void field_build_flow_unaligned(
    int                rdim, 
    int                cdim, 
    float              intf[], 
    uint8_t           *inout_flow)
{
    assert(rdim % 2 == 0);
    assert(cdim % 2 == 0);

    for(int r = 0; r < rdim; r++) {
    for(int c = 0; c < cdim; c++) {

        if(intf[r * rdim + c] == INFINITY)
            continue;

        if(intf[r * rdim + c] == 0.0f) {

            set_flow_cell(FD_NONE, r, c, rdim, cdim, inout_flow);
            continue;
        }

        enum flow_dir dir = field_flow_dir(rdim, cdim, intf, (struct coord){r, c});
        set_flow_cell(dir, r, c, rdim, cdim, inout_flow);
    }}
}

static void field_fixup_portal_edges(
    float                intf[FIELD_RES_R][FIELD_RES_C], 
    struct flow_field   *inout_flow,
    const struct portal *port)
{
    bool up    = port->connected->chunk.r < port->chunk.r;
    bool down  = port->connected->chunk.r > port->chunk.r;
    bool left  = port->connected->chunk.c < port->chunk.c;
    bool right = port->connected->chunk.c > port->chunk.c;
    assert(up ^ down ^ left ^ right);

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        if(intf[r][c] == 0.0f) {

            if(up)
                inout_flow->field[r][c].dir_idx = FD_N;
            else if(down)
                inout_flow->field[r][c].dir_idx = FD_S;
            else if(left)
                inout_flow->field[r][c].dir_idx = FD_W;
            else if(right)
                inout_flow->field[r][c].dir_idx = FD_E;
            else
                assert(0);
        }
    }}
}

static void field_chunk_bounds(vec3_t map_pos, struct coord chunk_coord, struct box_xz *out)
{
    size_t chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    int x_offset = -(chunk_coord.c * chunk_x_dim);
    int z_offset =  (chunk_coord.r * chunk_z_dim);

    out->x_max = map_pos.x + x_offset;
    out->x_min = out->x_max - chunk_x_dim;

    out->z_min = map_pos.z + z_offset;
    out->z_max = out->z_min + chunk_z_dim;
}

static int ent_faction_id(uint32_t uid, struct nav_unit_query_ctx *ctx)
{
    if(ctx) {
        return G_GetFactionIDFrom(ctx->faction_ids, uid);
    }else{
        return G_GetFactionID(uid);
    }
}

static uint32_t ent_flags(uint32_t uid, struct nav_unit_query_ctx *ctx)
{
    if(ctx) {
        return G_FlagsGetFrom(ctx->flags, uid);
    }else{
        return G_FlagsGet(uid);
    }
}

static bool ent_dying(uint32_t uid, struct nav_unit_query_ctx *ctx)
{
    if(ctx) {
        khiter_t k = kh_get(id, ctx->dying_set, uid);
        return (k != kh_end(ctx->dying_set));
    }else{
        return G_Combat_IsDying(uid);
    }
}

static void ent_curr_obb(uint32_t uid, struct obb *out, bool identity, 
                         struct nav_unit_query_ctx *ctx)
{
    if(ctx) {
        khiter_t k = kh_get(aabb, ctx->aabbs, uid);
        assert(k != kh_end(ctx->aabbs));
        struct aabb *aabb = &kh_value(ctx->aabbs, k);

        vec3_t pos = G_Pos_GetFrom(ctx->positions, uid);
        vec3_t scale = Entity_GetScaleFrom(ctx->transforms, uid);
        quat_t rot = Entity_GetRotFrom(ctx->transforms, uid);

        mat4x4_t model;
        Entity_ModelMatrixFrom(pos, rot, scale, &model);

        struct obb obb;
        Entity_CurrentOBBFrom(aabb, model, scale, out);
    }else{
        Entity_CurrentOBB(uid, out, identity);
    }
}

static vec2_t ent_pos_xz(uint32_t uid, struct nav_unit_query_ctx *ctx)
{
    if(ctx) {
        return G_Pos_GetXZFrom(ctx->positions, uid);
    }else{
        return G_Pos_GetXZ(uid);
    }
}

static float ent_sel_radius(uint32_t uid, struct nav_unit_query_ctx *ctx)
{
    if(ctx) {
        return G_GetSelectionRadiusFrom(ctx->sel_radiuses, uid);
    }else{
        return G_GetSelectionRadius(uid);
    }
}

static bool ent_player_visible(uint32_t uid, struct nav_unit_query_ctx *ctx)
{
    if(ctx) {
        struct obb obb;
        ent_curr_obb(uid, &obb, false, ctx);

        return G_Fog_ObjVisibleFrom(ctx->fog_state, ctx->fog_enabled, 
            ctx->player_controllable, &obb);
    }else{
        struct obb obb;
        Entity_CurrentOBB(uid, &obb, false);
        uint16_t pmask = G_GetPlayerControlledFactions();

        return G_Fog_ObjVisible(pmask, &obb);
    }
}

static bool field_enemy_ent(int faction_id, uint32_t ent, struct nav_unit_query_ctx *ctx)
{
    if(ent_faction_id(ent, ctx) == faction_id)
        return false;

    uint32_t flags = ent_flags(ent, ctx);
    if(!(flags & ENTITY_FLAG_COMBATABLE))
        return false;

    enum diplomacy_state ds;
    if(!ents_diplomacy_state(faction_id, ent_faction_id(ent, ctx), &ds, ctx))
        return false;

    if(ds != DIPLOMACY_STATE_WAR)
        return false;

    if(!ent_player_visible(ent, ctx))
        return false;

    return true;
}

static void qpush(struct tile_desc *q, size_t *qsize, int *head, int *tail, 
                   size_t nelems, struct tile_desc entry)
{
    assert(*qsize < nelems);

    ++(*tail);
    *tail = *tail % nelems;

    assert(*tail >= 0 && *tail < nelems);
    q[*tail] = entry;
    ++(*qsize);
}

static struct tile_desc qpop(struct tile_desc *q, size_t *qsize, int *head, int *tail, 
                              size_t nelems)
{
    assert(*qsize > 0);
    assert(*head >= 0 && *head < nelems);
    struct tile_desc ret = q[*head];
    ++(*head);
    *head = *head % nelems;
    --(*qsize);
    return ret;
}

static size_t field_closest_tiles_local(
    const struct nav_chunk *chunk, 
    struct coord            chunk_coord,
    struct coord            target, 
    uint16_t                local_iid, 
    uint16_t                global_iid,
    struct coord           *out, 
    size_t                  maxout)
{
    const size_t nelems = FIELD_RES_R * FIELD_RES_C;
    bool visited[FIELD_RES_R][FIELD_RES_C] = {0};
    struct tile_desc frontier[FIELD_RES_R * FIELD_RES_C];
    int fhead = 0, ftail = -1;
    size_t qsize = 0;

    size_t ret = 0;
    int first_mh_dist = -1;

    struct tile_desc target_tile = (struct tile_desc){
        chunk_coord.r, chunk_coord.c,
        target.r, target.c
    };
    qpush(frontier, &qsize, &fhead, &ftail, nelems, target_tile);
    visited[target.r][target.c] = true;

    while(qsize > 0) {

        struct tile_desc td = qpop(frontier, &qsize, &fhead, &ftail, nelems);
        struct coord curr = (struct coord){td.tile_r, td.tile_c};
        struct coord deltas[] = {
            { 0, -1},
            { 0, +1},
            {-1,  0},
            {+1,  0},
        };

        for(int i = 0; i < ARR_SIZE(deltas); i++) {

            struct coord neighb = (struct coord){
                curr.r + deltas[i].r,
                curr.c + deltas[i].c,
            };
            if(neighb.r < 0 || neighb.r >= FIELD_RES_R)
                continue;
            if(neighb.c < 0 || neighb.c >= FIELD_RES_C)
                continue;

            if(visited[neighb.r][neighb.c])
                continue;
            visited[neighb.r][neighb.c] = true;
            struct tile_desc ntd = (struct tile_desc){
                chunk_coord.r, chunk_coord.c,
                neighb.r, neighb.c
            };
            qpush(frontier, &qsize, &fhead, &ftail, nelems, ntd);
        }

        int mh_dist = manhattan_dist(target, curr);
        assert(mh_dist >= first_mh_dist);
        if(first_mh_dist > -1 && mh_dist > first_mh_dist) {
            assert(ret > 0);
            goto done; /* The mh distance is strictly increasing as we go outwards */
        }
        if(chunk->cost_base[curr.r][curr.c] == COST_IMPASSABLE)
            continue;
        if(chunk->blockers[curr.r][curr.c] > 0)
            continue;
        if(global_iid != ISLAND_NONE 
        && chunk->islands[curr.r][curr.c] != global_iid)
            continue;
        if(local_iid != ISLAND_NONE 
        && chunk->local_islands[curr.r][curr.c] != local_iid)
            continue;

        if(first_mh_dist == -1)
            first_mh_dist = mh_dist;

        out[ret++] = curr;
        if(ret == maxout)
            goto done;
    }

done:
    return ret;
}

static size_t field_tile_initial_frontier(
    struct coord               tile, 
    const struct nav_chunk    *chunk,
    bool                       ignoreblock, 
    int                        faction_id, 
    struct nav_unit_query_ctx *ctx,
    struct coord              *out, 
    size_t                     maxout)
{
    if(maxout == 0)
        return 0;

    uint16_t enemies = 0;
    if(faction_id != FACTION_ID_NONE) {
        enemies = enemies_for_faction(faction_id, ctx);
    }

    if(ignoreblock) {
        *out = tile;
        return 1;
    }

    if(faction_id == FACTION_ID_NONE) {
        if(!field_tile_passable(chunk, tile))
            return 0;
    }else{
        if(!field_tile_passable_no_enemies(chunk, tile, enemies))
            return 0;
    }

    /* The target tile is not blocked. Make it the frontier. */
    *out = tile;
    return 1;
}

static bool field_tile_adjacent_to_next_iid(
    const struct nav_private *priv, 
    enum nav_layer            layer, 
    struct portal_desc        pd, 
    int                       r, 
    int                       c)
{
    struct map_resolution res;
    N_GetResolution(priv, &res);

    const struct nav_chunk *next_chunk = &priv->chunks[layer][pd.next->chunk.r * priv->width + pd.next->chunk.c];
    for(int r2 = pd.next->endpoints[0].r; r2 <= pd.next->endpoints[1].r; r2++) {
    for(int c2 = pd.next->endpoints[0].c; c2 <= pd.next->endpoints[1].c; c2++) {

        struct tile_desc curr_td = (struct tile_desc){pd.port->chunk.r, pd.port->chunk.c, r,  c };
        struct tile_desc next_td = (struct tile_desc){pd.next->chunk.r, pd.next->chunk.c, r2, c2};
        int dr, dc;
        M_Tile_Distance(res, &curr_td, &next_td, &dr, &dc);

        if(abs(dr) + abs(dc) == 1) {
            uint16_t neighb_liid = next_chunk->local_islands[r2][c2];
            if(neighb_liid == pd.next_iid) {
                return true;
            }
        }
    }}
    return false;
}

static size_t field_portal_initial_frontier(
    const struct nav_private  *priv,
    enum nav_layer             layer,
    struct portal_desc         pd,
    const struct nav_chunk    *chunk,
    bool                       ignoreblock, 
    int                        faction_id, 
    struct nav_unit_query_ctx *ctx,
    struct coord              *out, 
    size_t                     maxout)
{
    if(maxout == 0)
        return 0;

    uint16_t enemies = 0;
    if(faction_id != FACTION_ID_NONE) {
        enemies = enemies_for_faction(faction_id, ctx);
    }

    /* Set all non-blocked tiles of the portal as the frontier */
    int ret = 0;
    for(int r = pd.port->endpoints[0].r; r <= pd.port->endpoints[1].r; r++) {
    for(int c = pd.port->endpoints[0].c; c <= pd.port->endpoints[1].c; c++) {

        assert(chunk->cost_base[r][c] != COST_IMPASSABLE);

        bool passable;
        if(faction_id == FACTION_ID_NONE) {
            passable = field_tile_passable(chunk, (struct coord){r, c});
        }else{
            passable = field_tile_passable_no_enemies(chunk, (struct coord){r, c}, enemies);
        }
        if(!passable)
            continue;

        if(pd.port_iid != ISLAND_NONE && chunk->local_islands[r][c] != pd.port_iid)
            continue;

        if(!field_tile_adjacent_to_next_iid(priv, layer, pd, r, c))
            continue;

        out[ret++] = (struct coord){r, c};
        if(ret == maxout)
            return ret;
    }}

    return ret;
}

static size_t field_enemies_initial_frontier(
    struct enemies_desc       *enemies, 
    const struct nav_private  *priv, 
    struct tile_desc           base,
    int                        rdim,
    int                        cdim,
    enum nav_layer             layer,
    struct nav_unit_query_ctx *ctx,
    struct tile_desc          *out, 
    size_t                     maxout)
{
    assert(Sched_UsingBigStack());

    struct box_xz bounds;
    field_chunk_bounds(enemies->map_pos, enemies->chunk, &bounds);
    float xlen = bounds.x_max - bounds.x_min;
    float zlen = bounds.z_max - bounds.z_min;

    STALLOC(uint32_t, ents, MAX_ENTS_PER_CHUNK);
    size_t num_ents;
    if(ctx) {
        num_ents = G_Pos_EntsInRectFrom(
            ctx->postree, ctx->flags,
            (vec2_t){bounds.x_min - xlen/2.0f - SEARCH_BUFFER, 
                     bounds.z_min - zlen/2.0f - SEARCH_BUFFER},
            (vec2_t){bounds.x_max + xlen/2.0f + SEARCH_BUFFER, 
                     bounds.z_max + zlen/2.0f + SEARCH_BUFFER},
            ents, MAX_ENTS_PER_CHUNK);
    }else{
        num_ents = G_Pos_EntsInRect(
            (vec2_t){bounds.x_min - xlen/2.0f - SEARCH_BUFFER, 
                     bounds.z_min - zlen/2.0f - SEARCH_BUFFER},
            (vec2_t){bounds.x_max + xlen/2.0f + SEARCH_BUFFER, 
                     bounds.z_max + zlen/2.0f + SEARCH_BUFFER},
            ents, MAX_ENTS_PER_CHUNK);
    }

    struct map_resolution res;
    N_GetResolution(priv, &res);

    STALLOC(bool, has_enemy, rdim * cdim);
    memset(has_enemy, 0, sizeof(bool) * rdim * cdim);

    for(int i = 0; i < num_ents; i++) {
    
        uint32_t curr_enemy = ents[i];
        if(!field_enemy_ent(enemies->faction_id, curr_enemy, ctx))
            continue;
        if(ent_dying(curr_enemy, ctx))
            continue;

        struct tile_desc tds[512];
        int ntds;
        if(ent_flags(curr_enemy, ctx) & ENTITY_FLAG_BUILDING) {

            struct obb obb;
            ent_curr_obb(curr_enemy, &obb, true, ctx);
            ntds = M_Tile_AllUnderObj(enemies->map_pos, res, &obb, tds, ARR_SIZE(tds));
        }else{
            ntds = M_Tile_AllUnderCircle(res, ent_pos_xz(curr_enemy, ctx),
                ent_sel_radius(curr_enemy, ctx), enemies->map_pos, tds, ARR_SIZE(tds));
        }

        if(layer >= NAV_LAYER_GROUND_3X3) {
            ntds += M_Tile_Contour(ntds, tds, res, tds + ntds, ARR_SIZE(tds) - ntds);
        }
        if(layer >= NAV_LAYER_GROUND_5X5) {
            ntds += M_Tile_Contour(ntds, tds, res, tds + ntds, ARR_SIZE(tds) - ntds);
        }
        if(layer >= NAV_LAYER_GROUND_7X7) {
            ntds += M_Tile_Contour(ntds, tds, res, tds + ntds, ARR_SIZE(tds) - ntds);
        }

        for(int j = 0; j < ntds; j++) {

            int dr, dc;
            struct tile_desc curr_td = tds[j];

            M_Tile_Distance(res, &base, &curr_td, &dr, &dc);
            if(dr < 0 || dr >= rdim)
                continue;
            if(dc < 0 || dc >= cdim)
                continue;
            has_enemy[dr * rdim + dc] = true;
        }
    }

    int ret = 0;
    for(int r = 0; r < rdim; r++) {
    for(int c = 0; c < cdim; c++) {
        
        if(ret == maxout)
            goto out;
        if(!has_enemy[r * rdim + c])
            continue;

        struct tile_desc td = base;
        bool status = M_Tile_RelativeDesc(res, &td, c, r);
        assert(status);
        out[ret++] = td;
    }}

out:
    STFREE(ents);
    STFREE(has_enemy);
    return ret;
}

static size_t field_entity_initial_frontier(
    struct entity_desc        *target, 
    const struct nav_private  *priv, 
    struct tile_desc           base,
    int                        rdim,
    int                        cdim,
    enum nav_layer             layer,
    struct nav_unit_query_ctx *ctx,
    struct tile_desc          *out, 
    size_t                     maxout)
{
    struct map_resolution res;
    N_GetResolution(priv, &res);

    int ntds;
    struct tile_desc tds[512];
    uint32_t ent = target->target;

    if(ent_flags(ent, ctx) & ENTITY_FLAG_BUILDING) {

        struct obb obb;
        ent_curr_obb(ent, &obb, true, ctx);
        ntds = M_Tile_AllUnderObj(target->map_pos, res, &obb, tds, ARR_SIZE(tds));
    }else{
        ntds = M_Tile_AllUnderCircle(res, ent_pos_xz(ent, ctx),
            ent_sel_radius(ent, ctx), target->map_pos, tds, ARR_SIZE(tds));
    }

    if(layer == NAV_LAYER_GROUND_3X3) {
        ntds += M_Tile_Contour(ntds, tds, res, tds + ntds, ARR_SIZE(tds) - ntds);
    }
    if(layer >= NAV_LAYER_GROUND_5X5) {
        ntds += M_Tile_Contour(ntds, tds, res, tds + ntds, ARR_SIZE(tds) - ntds);
    }
    if(layer >= NAV_LAYER_GROUND_7X7) {
        ntds += M_Tile_Contour(ntds, tds, res, tds + ntds, ARR_SIZE(tds) - ntds);
    }

    int ret = 0;
    for(int i = 0; i < ntds; i++) {

        if(ret == maxout)
            break;

        int dr, dc;
        M_Tile_Distance(res, &base, &tds[i], &dr, &dc);
        if(dr < 0 || dr >= rdim)
            continue;
        if(dc < 0 || dc >= cdim)
            continue;
        out[ret++] = tds[i];
    }
    return ret;
}

static size_t field_initial_frontier(
    enum nav_layer             layer,
    struct field_target        target, 
    const struct nav_chunk    *chunk, 
    const struct nav_private  *priv,
    bool                       ignoreblock, 
    int                        faction_id,
    struct nav_unit_query_ctx *ctx,
    struct coord              *init_frontier, 
    size_t                     maxout)
{
    size_t ninit = 0;
    switch(target.type) {
    case TARGET_PORTAL:
        
        ninit = field_portal_initial_frontier(priv, layer, target.pd, chunk, ignoreblock, 
            faction_id, ctx, init_frontier, maxout);
        break;

    case TARGET_TILE:

        ninit = field_tile_initial_frontier(target.tile, chunk, ignoreblock, 
            faction_id, ctx, init_frontier, maxout);
        break;

    case TARGET_ENEMIES:
    case TARGET_ENTITY:
        /* Requires special handling */
        assert(0);
        break;

    default: assert(0);
    }
    return ninit;
}

static void field_fixup(
    struct field_target     target, 
    float                   integration_field[FIELD_RES_R][FIELD_RES_C],
    struct flow_field      *inout_flow, 
    const struct nav_chunk *chunk)
{
    if(target.type == TARGET_PORTAL) {
        field_fixup_portal_edges(integration_field, inout_flow, target.pd.port); 
    }

    if(target.type == TARGET_PORTALMASK) {

        for(int i = 0; i < chunk->num_portals; i++) {

            if(!(target.portalmask & (((uint64_t)1) << i)))
                continue;
            field_fixup_portal_edges(integration_field, inout_flow, &chunk->portals[i]);
        }
    }
}

static size_t visited_idx(struct map_resolution res, struct region region, struct tile_desc curr)
{
    int dr, dc;
    M_Tile_Distance(res, &region.base, &curr, &dr, &dc);
    return (dr * region.r + dc);
}

/* Returns all pathable tiles surrounding an impassable island 
 * that 'start' is a part of. 
 */
static size_t field_passable_frontier(
    const struct nav_private *priv, 
    enum nav_layer            layer,
    struct tile_desc          start, 
    struct region             region,
    struct tile_desc         *out, 
    size_t                    maxout,
    void                     *workspace,
    size_t                    ws_size)
{
    struct nav_chunk *start_chunk = 
        &priv->chunks[layer][start.chunk_r * priv->width + start.chunk_c];
    struct coord start_coord = (struct coord){start.tile_r, start.tile_c};
    assert(!field_tile_passable(start_chunk, start_coord));
    size_t ret = 0;

    struct map_resolution res;
    N_GetResolution(priv, &res);
    const size_t nelems = pow(MAX(region.r, region.c), 2.0f);

    /* Allow worker threads to use thread-local workspace memory
     * for stack allocations when this routine is running in task
     * context.
     */
    bool *visited = NULL;
    struct tile_desc *frontier = NULL;
    if(!workspace) {
        visited = malloc(sizeof(bool) * nelems);
        frontier = malloc(sizeof(struct tile_desc) * nelems);
    }else{
        assert(ws_size >= (sizeof(bool) + sizeof(struct tile_desc)) * nelems + 16);
        visited = workspace;
        assert(((uintptr_t)visited) % sizeof(bool) == 0);
        /* Align the 'frontier' pointer */
        char *tmp = (void*)(((char*)workspace) + (sizeof(bool) * nelems));
        tmp += 16;
        tmp = (char*)(((uintptr_t)tmp) & ~0xf);
        frontier = (void*)tmp;
        assert(((uintptr_t)frontier) % 16 == 0);
    }
    memset(visited, 0, nelems * sizeof(bool));

    int fhead = 0, ftail = -1;
    size_t qsize = 0;

    qpush(frontier, &qsize, &fhead, &ftail, nelems, start);
    visited[visited_idx(res, region, start)] = true;

    while(qsize > 0) {
    
        struct tile_desc curr = qpop(frontier, &qsize, &fhead, &ftail, nelems);
        struct coord deltas[] = {
            { 0, -1},
            { 0, +1},
            {-1,  0},
            {+1,  0},
        };

        struct nav_chunk *curr_chunk = 
            &priv->chunks[layer][curr.chunk_r * priv->width + curr.chunk_c];
        struct coord curr_coord = (struct coord){curr.tile_r, curr.tile_c};
        if(field_tile_passable(curr_chunk, curr_coord)) {

            out[ret++] = curr;
            if(ret == maxout)
                return ret;
            continue;
        }

        for(int i = 0; i < ARR_SIZE(deltas); i++) {

            struct tile_desc neighb = curr;
            if(!M_Tile_RelativeDesc(res, &neighb, deltas[i].c, deltas[i].r))
                continue;
            if(tile_outside_region(res, region, neighb))
                continue;

            if(visited[visited_idx(res, region, neighb)])
                continue;
            visited[visited_idx(res, region, neighb)] = true;

            assert(neighb.chunk_r < res.chunk_w);
            assert(neighb.chunk_c < res.chunk_h);
            qpush(frontier, &qsize, &fhead, &ftail, nelems, neighb);
        }
    }
    if(!workspace) {
        PF_FREE(visited);
        PF_FREE(frontier);
    }
    return ret;
}

/* Update the field to guide towards the nearest possible enemy of the 
 * specified faction.
 */
static void field_update_enemies(
    struct coord               chunk_coord, 
    const struct nav_private  *priv, 
    enum nav_layer             layer, 
    struct enemies_desc        target, 
    struct nav_unit_query_ctx *ctx,
    struct flow_field         *inout_flow)
{
    struct map_resolution res;
    N_GetResolution(priv, &res);

    pq_td_t frontier;
    pq_td_init(&frontier);

    /* Make the integration field have a padding of of half a chunk width/length 
     * on every side of it. Initially, we will build a flow field with this 'padding'
     * around it, but then we will cut out the center FIELD_RES_R * FIELD_RES_C 
     * region and use that as the final field. The purpose of this is to consider 
     * enemies which are immediately outside the chunk bounds and also guide towards
     * them if they are optimal targets. 
     */
    const int rdim = (priv->height > 1) ? FIELD_RES_R * 2 + (FIELD_RES_R % 2) : FIELD_RES_R;
    const int cdim = (priv->width  > 1) ? FIELD_RES_C * 2 + (FIELD_RES_C % 2) : FIELD_RES_C;

    STALLOC(float, integration_field, rdim * cdim);
    for(int r = 0; r < rdim; r++) {
    for(int c = 0; c < cdim; c++) {
        integration_field[r * rdim + c] = INFINITY;
    }}

    struct tile_desc base = (struct tile_desc){
        .chunk_r = (chunk_coord.r > 0) ? chunk_coord.r - 1 : chunk_coord.r,
        .chunk_c = (chunk_coord.c > 0) ? chunk_coord.c - 1 : chunk_coord.c,
        .tile_r  = (chunk_coord.r > 0) ? FIELD_RES_R / 2 + (FIELD_RES_R % 2) : 0,
        .tile_c  = (chunk_coord.c > 0) ? FIELD_RES_C / 2 + (FIELD_RES_C % 2) : 0,
    };

    STALLOC(struct tile_desc, init_frontier, rdim * cdim);
    size_t ninit = field_enemies_initial_frontier(&target, priv, base, rdim, cdim,
        layer, ctx, init_frontier, rdim * cdim);

    for(int i = 0; i < ninit; i++) {

        struct tile_desc curr = init_frontier[i];

        int dr, dc;
        M_Tile_Distance(res, &base, &curr, &dr, &dc);
        assert(dr >= 0 && dr < rdim);
        assert(dc >= 0 && dc < cdim);

        pq_td_push(&frontier, 0.0f, curr); 
        integration_field[dr * rdim + dc] = 0.0f;
    }

    inout_flow->target = (struct field_target){
        .type = TARGET_ENEMIES,
        .enemies = target
    };

    const int roff = (chunk_coord.r > 0) ? FIELD_RES_R / 2 + (FIELD_RES_R % 2) : 0;
    const int coff = (chunk_coord.c > 0) ? FIELD_RES_C / 2 + (FIELD_RES_C % 2) : 0;

    struct region region = (struct region){base, rdim, cdim};
    field_build_integration_region(&frontier, priv, layer, 0, 
        region, integration_field);
    field_build_flow_region(rdim, cdim, roff, coff, integration_field, inout_flow);

    STFREE(integration_field);
    STFREE(init_frontier);
    pq_td_destroy(&frontier);
}

/* Update the field to guide towards the nearest possible tile which is 
 * adjacent to one of the tiles occupied by the specified entity.
 */
static void field_update_entity(
    struct coord              chunk_coord, 
    const struct nav_private *priv, 
    enum nav_layer            layer, 
    struct entity_desc        target, 
    struct nav_unit_query_ctx *ctx,
    struct flow_field         *inout_flow)
{
    struct map_resolution res;
    N_GetResolution(priv, &res);

    pq_td_t frontier;
    pq_td_init(&frontier);

    const int rdim = (priv->height > 1) ? FIELD_RES_R * 2 + (FIELD_RES_R % 2) : FIELD_RES_R;
    const int cdim = (priv->width  > 1) ? FIELD_RES_C * 2 + (FIELD_RES_C % 2) : FIELD_RES_C;

    STALLOC(float, integration_field, rdim * cdim);
    for(int r = 0; r < rdim; r++) {
    for(int c = 0; c < cdim; c++) {
        integration_field[r * rdim + c] = INFINITY;
    }}

    struct tile_desc base = (struct tile_desc){
        .chunk_r = (chunk_coord.r > 0) ? chunk_coord.r - 1 : chunk_coord.r,
        .chunk_c = (chunk_coord.c > 0) ? chunk_coord.c - 1 : chunk_coord.c,
        .tile_r  = (chunk_coord.r > 0) ? FIELD_RES_R / 2 + (FIELD_RES_R % 2) : 0,
        .tile_c  = (chunk_coord.c > 0) ? FIELD_RES_C / 2 + (FIELD_RES_C % 2) : 0,
    };

    STALLOC(struct tile_desc, init_frontier, rdim * cdim);
    size_t ninit = field_entity_initial_frontier(&target, priv, base, rdim, cdim,
        layer, ctx, init_frontier, rdim * cdim);

    for(int i = 0; i < ninit; i++) {

        struct tile_desc curr = init_frontier[i];

        int dr, dc;
        M_Tile_Distance(res, &base, &curr, &dr, &dc);
        assert(dr >= 0 && dr < rdim);
        assert(dc >= 0 && dc < cdim);

        pq_td_push(&frontier, 0.0f, curr); 
        integration_field[dr * rdim + dc] = 0.0f;
    }

    inout_flow->target = (struct field_target){
        .type = TARGET_ENTITY,
        .ent = target
    };

    const int roff = (chunk_coord.r > 0) ? FIELD_RES_R / 2 + (FIELD_RES_R % 2) : 0;
    const int coff = (chunk_coord.c > 0) ? FIELD_RES_C / 2 + (FIELD_RES_C % 2) : 0;

    struct region region = (struct region){base, rdim, cdim};
    field_build_integration_region(&frontier, priv, layer, 0, region, integration_field);
    field_build_flow_region(rdim, cdim, roff, coff, integration_field, inout_flow);

    STFREE(integration_field);
    STFREE(init_frontier);
    pq_td_destroy(&frontier);
}

static struct region clamped_region(struct nav_private *priv, size_t rdim, size_t cdim,
                                    struct tile_desc center)
{
    /* Find the clamped base of the field */
    struct map_resolution res;
    N_GetResolution(priv, &res);

    bool exists;
    struct tile_desc base, baser = center, basec = center;
    exists = M_Tile_RelativeDesc(res, &baser, 0, -(rdim / 2));
    if(!exists) {
        base.chunk_r = 0;
        base.tile_r = 0;
    }else{
        base.chunk_r = baser.chunk_r;
        base.tile_r = baser.tile_r;
    }

    exists = M_Tile_RelativeDesc(res, &basec, -(cdim / 2), 0);
    if(!exists) {
        base.chunk_c = 0;
        base.tile_c = 0;
    }else{
        base.chunk_c = basec.chunk_c;
        base.tile_c = basec.tile_c;
    }

    struct tile_desc end, endr = center, endc = center;
    exists = M_Tile_RelativeDesc(res, &endr, 0, (rdim / 2));
    if(!exists) {
        end.chunk_r = res.chunk_h - 1;
        end.tile_r = res.tile_h - 1;
    }else{
        end.chunk_r = endr.chunk_r;
        end.tile_r  = endr.tile_r;
    }

    exists = M_Tile_RelativeDesc(res, &endc, (cdim / 2), 0);
    if(!exists) {
        end.chunk_c = res.chunk_w - 1;
        end.tile_c = res.tile_h - 1;
    }else{
        end.chunk_c = endc.chunk_c;
        end.tile_c = endc.tile_c;
    }

    int dr, dc;
    M_Tile_Distance(res, &base, &end, &dr, &dc);

    return (struct region){
        .base = base, 
        .r = dr, 
        .c = dc
    };
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

ff_id_t N_FlowFieldID(struct coord chunk, struct field_target target, enum nav_layer layer)
{
    if(target.type == TARGET_PORTAL) {

        return (((uint64_t)layer)                          << 60)
             | (((uint64_t)target.type)                    << 56)
             | (((uint64_t)target.pd.next_iid & 0xf)       << 48)
             | (((uint64_t)target.pd.port_iid & 0xf)       << 40)
             | (((uint64_t)target.pd.port->endpoints[0].r) << 34)
             | (((uint64_t)target.pd.port->endpoints[0].c) << 28)
             | (((uint64_t)target.pd.port->endpoints[1].r) << 22)
             | (((uint64_t)target.pd.port->endpoints[1].c) << 16)
             | (((uint64_t)chunk.r)                        <<  8)
             | (((uint64_t)chunk.c)                        <<  0);

    }else if(target.type == TARGET_TILE) {

        return (((uint64_t)layer)                          << 60)
             | (((uint64_t)target.type)                    << 56)
             | (((uint64_t)target.tile.r)                  << 24)
             | (((uint64_t)target.tile.c)                  << 16)
             | (((uint64_t)chunk.r)                        <<  8)
             | (((uint64_t)chunk.c)                        <<  0);

    }else if(target.type == TARGET_ENEMIES) {

        return (((uint64_t)layer)                          << 60)
             | (((uint64_t)target.type)                    << 56)
             | (((uint64_t)target.enemies.faction_id)      << 24)
             | (((uint64_t)chunk.r)                        <<  8)
             | (((uint64_t)chunk.c)                        <<  0);

    }else if(target.type == TARGET_ENTITY) {
    
        return (((uint64_t)layer)                          << 60)
             | (((uint64_t)target.type)                    << 56)
             | (((uint64_t)target.ent.target)              << 24)
             | (((uint64_t)chunk.r)                        <<  8)
             | (((uint64_t)chunk.c)                        <<  0);

    }else {
        assert(0);
        return 0;
    }
}

enum nav_layer N_FlowFieldLayer(ff_id_t id)
{
    return (id >> 60);
}

int N_FlowFieldTargetType(ff_id_t id)
{
    return (id >> 56) & 0xf;
}

void N_FlowFieldInit(struct coord chunk_coord, struct flow_field *out)
{
    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        out->field[r][c].dir_idx = FD_NONE;
    }}
    out->chunk = chunk_coord;
}

void N_FlowFieldUpdate(
    struct coord               chunk_coord, 
    const struct nav_private  *priv, 
    int                        faction_id,
    enum nav_layer             layer, 
    struct field_target        target, 
    struct nav_unit_query_ctx *ctx,
    struct flow_field         *inout_flow)
{
    PERF_ENTER();
    if(target.type == TARGET_ENEMIES) {
        field_update_enemies(chunk_coord, priv, layer, target.enemies, ctx, inout_flow);
        PERF_RETURN_VOID();
    }

    if(target.type == TARGET_ENTITY) {
        field_update_entity(chunk_coord, priv, layer, target.ent, ctx, inout_flow);
        PERF_RETURN_VOID();
    }

    const struct nav_chunk *chunk = &priv->chunks[layer][IDX(chunk_coord.r, priv->width, chunk_coord.c)];
    pq_coord_t frontier;
    pq_coord_init(&frontier);

    float integration_field[FIELD_RES_R][FIELD_RES_C];
    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {
        integration_field[r][c] = INFINITY;
    }}

    struct coord init_frontier[FIELD_RES_R * FIELD_RES_C];
    size_t ninit = field_initial_frontier(layer, target, chunk, priv, false, faction_id, 
        ctx, init_frontier, ARR_SIZE(init_frontier));

    for(int i = 0; i < ninit; i++) {

        struct coord curr = init_frontier[i];
        pq_coord_push(&frontier, 0.0f, curr); 
        integration_field[curr.r][curr.c] = 0.0f;
    }

    inout_flow->target = target;
    field_build_integration(&frontier, chunk, faction_id, ctx, integration_field);
    field_build_flow(integration_field, inout_flow);
    field_fixup(target, integration_field, inout_flow, chunk);

    pq_coord_destroy(&frontier);
    PERF_RETURN_VOID();
}

void N_LOSFieldCreate(
    dest_id_t                  id, 
    struct coord               chunk_coord, 
    struct tile_desc           target,
    const struct nav_private  *priv, 
    vec3_t                     map_pos, 
    struct nav_unit_query_ctx *ctx,
    struct LOS_field          *out_los, 
    const struct LOS_field    *prev_los)
{
    int faction_id = N_DestFactionID(id);
    out_los->chunk = chunk_coord;
    memset(out_los->field, 0x00, sizeof(out_los->field));

    pq_coord_t frontier;
    pq_coord_init(&frontier);
    const struct nav_chunk *chunk = &priv->chunks[N_DestLayer(id)]
                                                 [chunk_coord.r * priv->width + chunk_coord.c];

    float integration_field[FIELD_RES_R][FIELD_RES_C];
    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {
        integration_field[r][c] = INFINITY; 
    }}

    /* Case 1: LOS for the destination chunk */
    if(chunk_coord.r == target.chunk_r && chunk_coord.c == target.chunk_c) {

        pq_coord_push(&frontier, 0.0f, (struct coord){target.tile_r, target.tile_c});
        integration_field[target.tile_r][target.tile_c] = 0.0f;
        assert(NULL == prev_los);

    /* Case 2: LOS for a chunk other than the destination chunk 
     * In this case, carry over the 'visible' and 'wavefront blocked' flags from 
     * the shared edge with the previous chunk. Then treat each tile with the 
     * 'wavefront blocked' flag as a LOS corner. This will make the LOS seamless
     * accross chunk borders. */
    }else{

        bool horizontal = {0};
        int curr_edge_idx = {0};
        int prev_edge_idx = {0};
        
        assert(prev_los);
        if(prev_los->chunk.r < chunk_coord.r) {

            horizontal = false;
            curr_edge_idx = 0;
            prev_edge_idx = FIELD_RES_R-1;

        }else if(prev_los->chunk.r > chunk_coord.r) {

            horizontal = false;
            curr_edge_idx = FIELD_RES_R-1;
            prev_edge_idx = 0;

        }else if(prev_los->chunk.c < chunk_coord.c) {

            horizontal = true;
            curr_edge_idx = 0;
            prev_edge_idx = FIELD_RES_C-1;

        }else if(prev_los->chunk.c > chunk_coord.c) {

            horizontal = true;
            curr_edge_idx = FIELD_RES_C-1;
            prev_edge_idx = 0;

        }else{
            assert(0);
        }

        if(horizontal) {

            for(int r = 0; r < FIELD_RES_R; r++) {

                out_los->field[r][curr_edge_idx] = prev_los->field[r][prev_edge_idx];
                if(out_los->field[r][curr_edge_idx].wavefront_blocked) {

                    struct tile_desc src_desc = (struct tile_desc) {
                        chunk_coord.r, chunk_coord.c, 
                        r, curr_edge_idx
                    };
                    field_create_wavefront_blocked_line(target, src_desc, priv, map_pos, out_los);
                }
                if(out_los->field[r][curr_edge_idx].visible) {

                    pq_coord_push(&frontier, 0.0f, (struct coord){r, curr_edge_idx});
                    integration_field[r][curr_edge_idx] = 0.0f;
                }
            }
        }else{
        
            for(int c = 0; c < FIELD_RES_C; c++) {

                out_los->field[curr_edge_idx][c] = prev_los->field[prev_edge_idx][c];
                if(out_los->field[curr_edge_idx][c].wavefront_blocked) {

                    struct tile_desc src_desc = (struct tile_desc) {
                        chunk_coord.r, chunk_coord.c, 
                        curr_edge_idx, c
                    };
                    field_create_wavefront_blocked_line(target, src_desc, priv, map_pos, out_los);
                }
                if(out_los->field[curr_edge_idx][c].visible) {

                    pq_coord_push(&frontier, 0.0f, (struct coord){curr_edge_idx, c});
                    integration_field[curr_edge_idx][c] = 0.0f; 
                }
            }
        }
    }

    while(pq_size(&frontier) > 0) {

        struct coord curr;
        pq_coord_pop(&frontier, &curr);

        struct coord neighbours[8];
        uint8_t neighbour_costs[8];
        int num_neighbours = field_neighbours_grid_los(chunk, out_los, faction_id, 
            curr, ctx, neighbours, neighbour_costs);

        for(int i = 0; i < num_neighbours; i++) {

            int nr = neighbours[i].r, nc = neighbours[i].c;
            if(neighbour_costs[i] > 1) {
                
                if(!field_is_los_corner(neighbours[i], chunk->cost_base, chunk->blockers))
                    continue;

                struct tile_desc src_desc = (struct tile_desc) {
                    .chunk_r = chunk_coord.r,
                    .chunk_c = chunk_coord.c,
                    .tile_r = neighbours[i].r,
                    .tile_c = neighbours[i].c
                };
                field_create_wavefront_blocked_line(target, src_desc, priv, map_pos, out_los);
            }else{

                float new_cost = integration_field[curr.r][curr.c] + 1;
                out_los->field[nr][nc].visible = 1;

                if(new_cost < integration_field[neighbours[i].r][neighbours[i].c]) {

                    integration_field[nr][nc] = new_cost;
                    if(!pq_coord_contains(&frontier, field_compare_tiles, neighbours[i]))
                        pq_coord_push(&frontier, new_cost, neighbours[i]);
                }
            }
        }
    }
    pq_coord_destroy(&frontier);

    /* Add a single tile-wide padding of invisible tiles around the wavefront. This is 
     * because we want to be conservative and not mark any tiles visible from which we
     * can't raycast to the destination point from any point within the tile without 
     * the ray going over impassable terrain. This is a nice property for the movement
     * code. */
    field_pad_wavefront(out_los);
}

void N_FlowFieldUpdateToNearestPathable(
    const struct nav_private  *priv, 
    enum nav_layer             layer,
    struct coord               chunk,
    struct coord               start, 
    int                        faction_id, 
    struct nav_unit_query_ctx *ctx,
    struct flow_field         *inout_flow)
{
    struct tile_desc init_frontier[FIELD_RES_R * FIELD_RES_C];
    struct region chunk_region = (struct region){
        .base = {chunk.r, chunk.c, 0, 0},
        .r = FIELD_RES_R, 
        .c = FIELD_RES_C
    };
    struct tile_desc start_coord = (struct tile_desc){
        chunk.r, chunk.c,
        start.r, start.c
    };
    assert(chunk_region.base.chunk_r == start_coord.chunk_r);
    assert(chunk_region.base.chunk_c == start_coord.chunk_c);
    size_t ninit = field_passable_frontier(priv, layer, start_coord, 
        chunk_region, init_frontier, ARR_SIZE(init_frontier), NULL, 0);

    pq_coord_t frontier;
    pq_coord_init(&frontier);

    float integration_field[FIELD_RES_R][FIELD_RES_C];
    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {
        integration_field[r][c] = INFINITY;
    }}

    for(int i = 0; i < ninit; i++) {

        struct coord curr = (struct coord){
            init_frontier[i].tile_r,
            init_frontier[i].tile_c
        };
        pq_coord_push(&frontier, 0.0f, curr); 
        integration_field[curr.r][curr.c] = 0.0f;
    }

    struct nav_chunk *navchunk = &priv->chunks[layer][chunk.r * priv->width + chunk.c];
    field_build_integration_nonpass(&frontier, navchunk, faction_id, ctx, integration_field);

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        if(integration_field[r][c] == INFINITY)
            continue;
        if(integration_field[r][c] == 0.0f)
            continue;
        inout_flow->field[r][c].dir_idx = field_flow_dir(FIELD_RES_R, FIELD_RES_C, 
            (const float*)integration_field, (struct coord){r, c});
    }}

    pq_coord_destroy(&frontier);
}

void N_FlowFieldUpdateIslandToNearest(
    uint16_t                   local_iid, 
    const struct nav_private  *priv,
    enum nav_layer             layer, 
    int                        faction_id, 
    struct nav_unit_query_ctx *ctx,
    struct flow_field         *inout_flow)
{
    struct coord chunk_coord = inout_flow->chunk;
    const struct nav_chunk *chunk = &priv->chunks[layer][IDX(chunk_coord.r, priv->width, chunk_coord.c)];

    struct tile_desc base = (struct tile_desc){
        .chunk_r = chunk_coord.r,
        .chunk_c = chunk_coord.c,
        .tile_r  = 0,
        .tile_c  = 0,
    };

    pq_coord_t frontier;
    pq_coord_init(&frontier);

    struct coord init_frontier[FIELD_RES_R * FIELD_RES_C];
    size_t ninit = 0;

    if(inout_flow->target.type == TARGET_ENEMIES) {

        struct tile_desc enemies_init_frontier[FIELD_RES_R * FIELD_RES_C];
        size_t ntds = field_enemies_initial_frontier(&inout_flow->target.enemies, priv, base, 
            FIELD_RES_R, FIELD_RES_C, layer, ctx, enemies_init_frontier, ARR_SIZE(enemies_init_frontier));
        for(int i = 0; i < ntds; i++) {
            init_frontier[ninit++] = (struct coord){
                enemies_init_frontier[i].tile_r,
                enemies_init_frontier[i].tile_c,
            };
        }
        assert(ninit == ntds);

    }else if(inout_flow->target.type == TARGET_ENTITY) {

        struct tile_desc entity_init_frontier[FIELD_RES_R * FIELD_RES_C];
        size_t ntds = field_entity_initial_frontier(&inout_flow->target.ent, priv, base, 
            FIELD_RES_R, FIELD_RES_C, layer, ctx, entity_init_frontier, ARR_SIZE(entity_init_frontier));
        for(int i = 0; i < ntds; i++) {
            init_frontier[ninit++] = (struct coord){
                entity_init_frontier[i].tile_r,
                entity_init_frontier[i].tile_c,
            };
        }
        assert(ninit == ntds);
    
    }else{
        ninit = field_initial_frontier(layer, inout_flow->target, chunk, priv, false, faction_id, 
            ctx, init_frontier, ARR_SIZE(init_frontier));
        /* If there were no tiles in the initial frontier, that means the target
         * was completely blocked off. */
        if(!ninit) {
            ninit = field_initial_frontier(layer, inout_flow->target, chunk, priv, true, faction_id, 
                ctx, init_frontier, ARR_SIZE(init_frontier));
        }
    }

    /* the new frontier can have some duplicate coordiantes */
    int min_mh_dist = INT_MAX;
    struct coord new_init_frontier[FIELD_RES_R * FIELD_RES_C];
    size_t new_ninit = 0;

    for(int i = 0; i < ninit; i++) {

        struct coord curr = init_frontier[i];
        uint16_t curr_giid = chunk->islands[curr.r][curr.c];
        uint16_t curr_liid = chunk->local_islands[curr.r][curr.c];

        /* In case any part of the frontier has tiles matching the desired local ID, 
         * then only include those tiles. This means at least some part of the frontier
         * is reachable from the specified island.
         */
        if(curr_liid == local_iid) {
            if(min_mh_dist > 0)
                new_ninit = 0;
            min_mh_dist = 0;
            new_init_frontier[new_ninit++] = curr;
            continue;
        }

        struct coord tmp[FIELD_RES_R * FIELD_RES_C];
        int nextra = field_closest_tiles_local(chunk, chunk_coord, curr, local_iid, curr_giid, 
            tmp, ARR_SIZE(tmp) - new_ninit);
        if(!nextra)
            continue;

        int mh_dist = manhattan_dist(tmp[0], curr);
        if(mh_dist < min_mh_dist) {
            min_mh_dist = mh_dist;
            new_ninit = 0;
        }

        if(mh_dist > min_mh_dist)
            continue;

        memcpy(new_init_frontier + new_ninit, tmp, nextra * sizeof(struct coord));
        new_ninit += nextra;
    }

    float integration_field[FIELD_RES_R][FIELD_RES_C];
    for(int r = 0; r < FIELD_RES_R; r++)
        for(int c = 0; c < FIELD_RES_C; c++)
            integration_field[r][c] = INFINITY;

    for(int i = 0; i < new_ninit; i++) {

        struct coord curr = new_init_frontier[i];
        pq_coord_push(&frontier, 0.0f, curr); 
        integration_field[curr.r][curr.c] = 0.0f;
    }

    field_build_integration(&frontier, chunk, faction_id, ctx, integration_field);
    field_build_flow(integration_field, inout_flow);
    field_fixup(inout_flow->target, integration_field, inout_flow, chunk);

    pq_coord_destroy(&frontier);
}

vec2_t N_FlowDir(enum flow_dir dir)
{
    static vec2_t s_flow_dir_lookup[9] = {0};
    s_flow_dir_lookup[FD_NONE]  = (vec2_t){  0.0f,               0.0f              };
    s_flow_dir_lookup[FD_NW]    = (vec2_t){  1.0f / sqrt(2.0f), -1.0f / sqrt(2.0f) };
    s_flow_dir_lookup[FD_N]     = (vec2_t){  0.0f,              -1.0f              };
    s_flow_dir_lookup[FD_NE]    = (vec2_t){ -1.0f / sqrt(2.0f), -1.0f / sqrt(2.0f) };
    s_flow_dir_lookup[FD_W]     = (vec2_t){  1.0f,               0.0f              };
    s_flow_dir_lookup[FD_E]     = (vec2_t){ -1.0f,               0.0f              };
    s_flow_dir_lookup[FD_SW]    = (vec2_t){  1.0f / sqrt(2.0f),  1.0f / sqrt(2.0f) };
    s_flow_dir_lookup[FD_S]     = (vec2_t){  0.0f,               1.0f              };
    s_flow_dir_lookup[FD_SE]    = (vec2_t){ -1.0f / sqrt(2.0f),  1.0f / sqrt(2.0f) };

    return s_flow_dir_lookup[dir];
}

void N_CellArrivalFieldCreate(void *nav_private, size_t rdim, size_t cdim, 
                              enum nav_layer layer, uint16_t enemies,
                              struct tile_desc target, struct tile_desc center, 
                              uint8_t *out, void *workspace, size_t workspace_size)
{
    PERF_ENTER();
    assert(rdim % 2 == 0);
    assert(cdim % 2 == 0);

    size_t out_size = (rdim * cdim) / 2;
    memset(out, 0, out_size);

    struct nav_private *priv = nav_private;
    struct map_resolution res;
    N_GetResolution(priv, &res);

    pq_td_t frontier;
    pq_td_init(&frontier);

    size_t integration_field_size = sizeof(float) * rdim * cdim;
    assert(workspace_size >= integration_field_size);
    float *integration_field = workspace;
    assert(((uintptr_t)integration_field) % sizeof(float) == 0);

    for(int r = 0; r < rdim; r++) {
    for(int c = 0; c < cdim; c++) {
        integration_field[r * rdim + c] = INFINITY;
    }}

    int base_abs_r = center.chunk_r * res.tile_h + center.tile_r - (rdim / 2);
    int base_abs_c = center.chunk_c * res.tile_w + center.tile_c - (cdim / 2);

    /* Clamp the base coordinate to account for any off-by-one 
     * errors due to rounding and binning.
     */
    int target_abs_r = target.chunk_r * res.tile_h + target.tile_r;
    int target_abs_c = target.chunk_c * res.tile_w + target.tile_c;
    if((target_abs_r - base_abs_r) >= rdim) {
        base_abs_r = target_abs_r - (rdim - 1);
    }
    if((target_abs_c - base_abs_c) >= cdim) {
        base_abs_c = target_abs_c - (cdim - 1);
    }

    /* Find the clamped minimum coordinate of the field. Note that the 'base'
     * coordinate may fall outside the map bounds.
     */
    struct tile_desc base = (struct tile_desc){
        base_abs_r / res.tile_h,
        base_abs_c / res.tile_w,
        base_abs_r % res.tile_h,
        base_abs_c % res.tile_w,
    };

    int dr, dc;
    M_Tile_Distance(res, &base, &target, &dr, &dc);
    assert(dr >= 0 && dr < rdim);
    assert(dc >= 0 && dc < cdim);

    pq_td_push(&frontier, 0.0f, target); 
    integration_field[dr * rdim + dc] = 0.0f;

    struct region region = (struct region){base, rdim, cdim};
    field_build_integration_region(&frontier, priv, layer, enemies, region, integration_field);
    field_build_flow_unaligned(rdim, cdim, integration_field, out);

    pq_td_destroy(&frontier);
    PERF_RETURN_VOID();
}

void N_CellArrivalFieldUpdateToNearestPathable(void *nav_private, size_t rdim, size_t cdim, 
                              enum nav_layer layer, uint16_t enemies,
                              struct tile_desc start, struct tile_desc center, 
                              uint8_t *inout, void *workspace, size_t workspace_size)
{
    struct nav_private *priv = nav_private;
    size_t integration_field_size = sizeof(float) * rdim * cdim;

    float *integration_field = workspace;
    assert(((uintptr_t)integration_field) % sizeof(float) == 0);
    assert(workspace_size >= integration_field_size);

    workspace_size -= integration_field_size;
    workspace = ((char*)workspace) + integration_field_size;
    assert(((uintptr_t)workspace) % sizeof(float) == 0);

    size_t frontier_size = sizeof(struct tile_desc) * rdim * cdim;
    assert(workspace_size >= frontier_size * 2);

    /* Align the 'frontier' pointer */
    char *tmp = workspace;
    tmp += 16;
    tmp = (char*)(((uintptr_t)tmp) & ~0xf);

    struct tile_desc *init_frontier = (void*)tmp;
    assert(((uintptr_t)init_frontier) % 16 == 0);

    size_t delta = (char*)init_frontier - (char*)workspace;
    workspace_size -= delta;
    workspace_size -= frontier_size;
    workspace = ((char*)init_frontier) + frontier_size;

    struct region clamped = clamped_region(priv, rdim, cdim, center);
    size_t ninit = field_passable_frontier(priv, layer, start, 
        clamped, init_frontier, rdim * cdim, workspace, workspace_size);

    pq_td_t frontier;
    pq_td_init(&frontier);

    for(int r = 0; r < rdim; r++) {
    for(int c = 0; c < cdim; c++) {
        integration_field[r * rdim + c] = INFINITY;
    }}

    /* Find the clamped minimum coordinate of the field. Note that the 'base'
     * coordinate may fall outside the map bounds.
     */
    struct map_resolution res;
    N_GetResolution(priv, &res);

    int abs_r = center.chunk_r * res.tile_h + center.tile_r - (rdim / 2);
    int abs_c = center.chunk_c * res.tile_w + center.tile_c - (cdim / 2);
    struct tile_desc base = (struct tile_desc){
        abs_r / res.tile_h,
        abs_c / res.tile_w,
        abs_r % res.tile_h,
        abs_c % res.tile_w,
    };

    for(int i = 0; i < ninit; i++) {

        int dr, dc;
        M_Tile_Distance(res, &base, &init_frontier[i], &dr, &dc);
        /* Skip any tiles that don't fall within the bounds */
        if(dr < 0 || dr >= rdim)
            continue;
        if(dc < 0 || dc >= cdim)
            continue;
        assert(dr >= 0 && dr < rdim);
        assert(dc >= 0 && dc < cdim);

        pq_td_push(&frontier, 0.0f, init_frontier[i]);
        integration_field[dr * rdim + dc] = 0.0f;
    }

    struct region region = (struct region){base, rdim, cdim};
    field_build_integration_nonpass_region(&frontier, priv, layer, enemies, 
        region, integration_field);

    for(int r = 0; r < rdim; r++) {
    for(int c = 0; c < cdim; c++) {

        struct tile_desc curr = base;
        bool exists = M_Tile_RelativeDesc(res, &curr, c, r);
        if(!exists)
            continue;

        if(integration_field[r * rdim + c] == INFINITY)
            continue;
        if(integration_field[r * rdim + c] == 0.0f)
            continue;

        enum flow_dir dir = field_flow_dir(rdim, cdim, (const float *)integration_field,
            (struct coord){r, c});
        set_flow_cell(dir, r, c, rdim, cdim, inout);
    }}

    pq_td_destroy(&frontier);
}

