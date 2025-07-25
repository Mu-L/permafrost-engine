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

#include "movement.h"
#include "game_private.h"
#include "formation.h"
#include "combat.h"
#include "clearpath.h"
#include "position.h"
#include "fog_of_war.h"
#include "public/game.h"
#include "../config.h"
#include "../camera.h"
#include "../asset_load.h"
#include "../event.h"
#include "../entity.h"
#include "../cursor.h"
#include "../settings.h"
#include "../ui.h"
#include "../perf.h"
#include "../sched.h"
#include "../task.h"
#include "../main.h"
#include "../navigation/public/nav.h"
#include "../lib/public/queue.h"
#include "../phys/public/collision.h"
#include "../script/public/script.h"
#include "../render/public/render.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/vec.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stalloc.h"
#include "../anim/public/anim.h"
#include "../lib/public/stalloc.h"

#include <assert.h>
#include <SDL.h>

static int hz_count(enum movement_hz hz);

/* For the purpose of movement simulation, all entities have the same mass,
 * meaning they are accelerate the same amount when applied equal forces. */
#define ENTITY_MASS           (1.0f)
#define EPSILON               (1.0f/1024)
#define MAX_FORCE             (0.75f)
#define SCALED_MAX_FORCE      (MAX_FORCE / hz_count(s_move_work.hz) * 20.0)
#define VEL_HIST_LEN          (14)
#define MAX_MOVE_TASKS        (64)
#define MAX_GPU_FLOCK_MEMBERS (1024)  /* Must match movement.glsl */

#define SIGNUM(x)    (((x) > 0) - ((x) < 0))
#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)  (sizeof(a)/sizeof(a[0]))
#define CHUNK_WIDTH  (X_COORDS_PER_TILE * TILES_PER_CHUNK_WIDTH)
#define CHUNK_HEIGHT (Z_COORDS_PER_TILE * TILES_PER_CHUNK_HEIGHT)
#define STR(a)       #a

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

#define CHK_TRUE_JMP(_pred, _label)     \
    do{                                 \
        if(!(_pred))                    \
            goto _label;                \
    }while(0)

enum arrival_state{
    /* Entity is moving towards the flock's destination point */
    STATE_MOVING,
    /* Like STATE_MOVING, but the entity is also constrained by
     * a number of forces that give it a tendency to occupy its' 
     * relative position in a formation. */
    STATE_MOVING_IN_FORMATION,
    /* Entity is considered to have arrived and no longer moving. */
    STATE_ARRIVED,
    /* Entity is approaching the nearest enemy entity */
    STATE_SEEK_ENEMIES,
    /* The navigation system was unable to guide the entity closer
     * to the goal. It stops and waits. */
    STATE_WAITING,
    /* Move towards the closest point touching the target entity, but 
     * stop before actually stepping on it's tiles. */
    STATE_SURROUND_ENTITY,
    /* Move towards the closest position that will take us within the
     * specified range of the target entity */
    STATE_ENTER_ENTITY_RANGE,
    /* Entity is turning until it faces a particular direction */
    STATE_TURNING,
    /* For entities that are a part of a formation, the final stage
     * of the path will have the entity move to its' dedicated cell
     * in the formation. */
    STATE_ARRIVING_TO_CELL
};

struct movestate{
    enum arrival_state state;
    /* The base movement speed in units of OpenGL coords / second 
     */
    float              max_speed;
    /* The current velocity 
     */
    vec2_t             velocity;
    /* State tracking variables for interpolating between movement ticks.
     * During a single movement tick, the entity's position is moved to
     * intermediate points between 'prev_pos' and 'next_pos', in increments
     * expressed by 'step'.
     */
    vec3_t             next_pos;    /* The computed next movement tick position */
    vec3_t             prev_pos;    /* The position at the start of the previous tick */
    quat_t             next_rot;    /* The computed next movement tick rotation */
    quat_t             prev_rot;    /* The rotation at the start of the previous tick */
    float              step;        /* The fraction of the distance covered in a single step 
                                     * (nsteps = 1.0/step) */
    int                left;        /* The number of interpolation steps left (0 means the entity is at next_pos) */
    /* Flag to track whether the entiy is currently acting as a 
     * navigation blocker, and the last position where it became a blocker. 
     */
    bool               blocking;
    vec2_t             last_stop_pos;
    float              last_stop_radius;
    /* Information for waking up from the 'WAITING' state 
     */
    enum arrival_state wait_prev;
    int                wait_ticks_left;
    /* History of the previous ticks' velocities. Used for velocity smoothing. 
     */
    vec2_t             vel_hist[VEL_HIST_LEN];
    int                vel_hist_idx;
    /* Entity that we're moving towards when in the 'SURROUND_STATIC_ENTITY' state 
     */
    uint32_t           surround_target_uid;
    vec2_t             surround_target_prev;
    vec2_t             surround_nearest_prev;
    /* Flag indicating that we are now using the 'surround' field rather than the 
     * 'target seek' field to get to path to our target. This kicks in once we pass
     * the distance 'low water' threshold and is turned off if we pass the 'high water' 
     * threshold again - this is to prevent 'toggling' at a boundary where we switch 
     * from one field to another. 
     */
    bool               using_surround_field;
    /* Additional state for entities in 'ENTER_ENTITY_RANGE' state 
     */
    vec2_t             target_prev_pos;
    float              target_range;
    /* The target direction for 'turning' entities 
     */
    quat_t             target_dir;
};

struct flock{
    khash_t(entity) *ents;
    vec2_t           target_xz; 
    dest_id_t        dest_id;
};

struct formation_state{
    formation_id_t fid;
    bool           assignment_ready;
    bool           assigned_to_cell;
    bool           in_range_of_cell;
    bool           arrived_at_cell;
    vec2_t         normal_cohesion_force;
    vec2_t         normal_align_force;
    vec2_t         normal_drag_force;
    quat_t         target_orientation;
};

enum movestate_flags{
    UPDATE_SET_STATE       = (1 << 0),
    UPDATE_SET_VELOCITY    = (1 << 1),
    UPDATE_SET_POSITION    = (1 << 2),
    UPDATE_SET_ROTATION    = (1 << 3),
    UPDATE_SET_NEXT_POS    = (1 << 4),
    UPDATE_SET_PREV_POS    = (1 << 5),
    UPDATE_SET_STEP        = (1 << 6),
    UPDATE_SET_LEFT        = (1 << 7),
    UPDATE_SET_NEXT_ROT    = (1 << 8),
    UPDATE_SET_PREV_ROT    = (1 << 9),
    UPDATE_SET_DEST        = (1 << 10),
    UPDATE_SET_TARGET_PREV = (1 << 11),
    UPDATE_SET_MOVING      = (1 << 12),
    UPDATE_SET_TARGET_DIR  = (1 << 13)
};

struct movestate_patch{
    enum movestate_flags flags;
    enum arrival_state   next_state;
    vec2_t               next_velocity;
    vec3_t               next_pos;
    quat_t               next_rot;
    bool                 next_block;
    vec3_t               next_ppos;
    vec3_t               next_npos;
    float                next_step;
    float                next_left;
    quat_t               next_nrot;
    quat_t               next_prot;
    vec2_t               next_dest;
    bool                 next_attack;
    vec2_t               next_target_prev;
    quat_t               next_target_dir;
};

struct move_work_in{
    uint32_t       ent_uid;
    vec2_t         ent_des_v;
    float          speed;
    vec2_t         cell_pos;
    struct cp_ent  cp_ent;
    bool           save_debug;
    vec_cp_ent_t  *stat_neighbs;
    vec_cp_ent_t  *dyn_neighbs;
    bool           has_dest_los;
    struct formation_state fstate;
    vec2_t         cell_arrival_vdes;
};

struct move_work_out{
    uint32_t ent_uid;
    vec2_t   ent_des_v;
    vec2_t   ent_vel;
    struct movestate_patch patch;
};

struct move_task_arg{
    size_t begin_idx;
    size_t end_idx;
};

/* The subset of the gamestate that is necessary 
 * to derive the new entity velocities and positions. 
 * We make a copy of this state so that movement 
 * computations can safely be done asynchronously,
 * or even be spread over multiple frames. 
 */
struct move_gamestate{
    khash_t(id)           *flags;
    khash_t(pos)          *positions;
    qt_ent_t              *postree;
    khash_t(range)        *sel_radiuses;
    khash_t(id)           *faction_ids;
    khash_t(id)           *ent_gpu_id_map;
    khash_t(id)           *gpu_id_ent_map;
    struct map            *map;
    /* Additional state needed for nav_unit_query_ctx */
    struct kh_aabb_s      *aabbs;
    void                  *transforms;
    bool                  fog_enabled;
    uint32_t              *fog_state;
    struct kh_id_s        *dying_set;
    enum diplomacy_state (*diptable)[MAX_FACTIONS];
    uint16_t               player_controllable;
};

enum move_work_type{
    WORK_TYPE_CPU,
    WORK_TYPE_GPU
};

enum move_work_status{
    WORK_COMPLETE,
    WORK_INCOMPLETE
};

struct move_work{
    struct memstack           mem;
    struct move_gamestate     gamestate;
    enum move_work_type       type;
    struct nav_unit_query_ctx unit_query_ctx;
    enum movement_hz          hz;
    struct move_work_in      *in;
    struct move_work_out     *out;
    size_t                    nwork;
    size_t                    ntasks;
    uint32_t                  tids[MAX_MOVE_TASKS];
    SDL_atomic_t              gpu_velocities_ready;
    vec2_t                   *gpu_velocities;
    struct future             futures[MAX_MOVE_TASKS];
};

/* Must match movement.glsl */
struct gpu_flock_desc{
    GLuint  ents[MAX_GPU_FLOCK_MEMBERS];
    GLuint  nmembers;
    GLfloat target_x;
    GLfloat target_z;
};

/* Must match movement.glsl */
struct gpu_ent_desc{
    vec2_t   dest;
    vec2_t   vdes;
    vec2_t   cell_pos;
    vec2_t   formation_cohesion_force;
    vec2_t   formation_align_force;
    vec2_t   formation_drag_force;
    vec2_t   pos;
    vec2_t   velocity;
    uint32_t movestate;
    uint32_t flock_id;
    uint32_t flags;
    float    speed;
    float    max_speed;
    float    radius;
    uint32_t layer;
    uint32_t has_dest_los;
    uint32_t formation_assignment_ready;
    uint32_t __pad0; /* Keep aligned to vec2 size */
};

enum move_cmd_type{
    MOVE_CMD_ADD,
    MOVE_CMD_REMOVE,
    MOVE_CMD_STOP,
    MOVE_CMD_SET_DEST,
    MOVE_CMD_CHANGE_DIRECTION,
    MOVE_CMD_SET_ENTER_RANGE,
    MOVE_CMD_SET_SEEK_ENEMIES,
    MOVE_CMD_SET_SURROUND_ENTITY,
    MOVE_CMD_UPDATE_POS,
    MOVE_CMD_UPDATE_FACTION_ID,
    MOVE_CMD_UPDATE_SELECTION_RADIUS,
    MOVE_CMD_SET_MAX_SPEED,
    MOVE_CMD_MAKE_FLOCKS,
    MOVE_CMD_UNBLOCK,
    MOVE_CMD_BLOCK
};

struct move_cmd{
    bool               deleted;
    enum move_cmd_type type;
    struct attr        args[6];
};

KHASH_MAP_INIT_INT(state, struct movestate)
KHASH_MAP_INIT_INT(aabb, struct aabb)

QUEUE_TYPE(cmd, struct move_cmd)
QUEUE_IMPL(static, cmd, struct move_cmd)

VEC_TYPE(flock, struct flock)
VEC_IMPL(static inline, flock, struct flock)

static void move_push_cmd(struct move_cmd cmd);
static void do_set_dest(uint32_t uid, vec2_t dest_xz, bool attack);
static void do_stop(uint32_t uid);
static void do_update_pos(uint32_t uid, vec2_t pos);
static void move_tick(void *user, void *event);
static struct result navigation_tick_task(void *arg);

/* Parameters controlling steering/flocking behaviours */
#define SEPARATION_FORCE_SCALE          (0.6f)
#define MOVE_ARRIVE_FORCE_SCALE         (0.5f)
#define MOVE_COHESION_FORCE_SCALE       (0.15f)
#define ALIGNMENT_FORCE_SCALE           (0.15f)

#define SEPARATION_BUFFER_DIST          (0.0f)
#define COHESION_NEIGHBOUR_RADIUS       (50.0f)
#define ARRIVE_SLOWING_RADIUS           (10.0f)
#define ADJACENCY_SEP_DIST              (5.0f)
#define ALIGN_NEIGHBOUR_RADIUS          (10.0f)
#define SEPARATION_NEIGHB_RADIUS        (30.0f)
#define CELL_ARRIVAL_RADIUS             (30.0f)

#define COLLISION_MAX_SEE_AHEAD         (10.0f)
#define WAIT_TICKS                      (60)
#define MAX_TURN_RATE                   (15.0f) /* degree/tick */
#define SCALED_MAX_TURN_RATE            (MAX_TURN_RATE / hz_count(s_move_work.hz) * 20.0)
#define MAX_NEIGHBOURS                  (32)

#define SURROUND_LOW_WATER_X            (CHUNK_WIDTH/3.0f)
#define SURROUND_HIGH_WATER_X           (CHUNK_WIDTH/2.0f)
#define SURROUND_LOW_WATER_Z            (CHUNK_HEIGHT/3.0f)
#define SURROUND_HIGH_WATER_Z           (CHUNK_HEIGHT/2.0f)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map       *s_map;
static bool                    s_attack_on_lclick = false;
static bool                    s_move_on_lclick = false;
static bool                    s_click_move_enabled = true;

static bool                    s_mouse_dragged = false;
static vec3_t                  s_drag_begin_pos;
static vec3_t                  s_drag_end_pos;
static bool                    s_drag_attacking;

static vec_entity_t            s_move_markers;
static vec_flock_t             s_flocks;
static khash_t(state)         *s_entity_state_table;

/* Store the most recently issued move command location for debug rendering */
static bool                    s_last_cmd_dest_valid = false;
static dest_id_t               s_last_cmd_dest;

static struct move_work        s_move_work;
static queue_cmd_t             s_move_commands;
static struct memstack         s_eventargs;

static unsigned long           s_last_tick = 0;
static unsigned long           s_last_interpolate_tick = 0;

static enum movement_hz        s_move_hz = MOVE_HZ_20;
static bool                    s_move_hz_dirty = false;
static bool                    s_use_gpu = true;
static bool                    s_move_tick_queued = false;

static uint32_t                s_tick_task_tid = NULL_TID;
static struct future           s_tick_task_future;

static const char *s_state_str[] = {
    [STATE_MOVING]              = STR(STATE_MOVING),
    [STATE_MOVING_IN_FORMATION] = STR(STATE_MOVING_IN_FORMATION),
    [STATE_ARRIVED]             = STR(STATE_ARRIVED),
    [STATE_SEEK_ENEMIES]        = STR(STATE_SEEK_ENEMIES),
    [STATE_WAITING]             = STR(STATE_WAITING),
    [STATE_SURROUND_ENTITY]     = STR(STATE_SURROUND_ENTITY),
    [STATE_ENTER_ENTITY_RANGE]  = STR(STATE_ENTER_ENTITY_RANGE),
    [STATE_TURNING]             = STR(STATE_TURNING),
    [STATE_ARRIVING_TO_CELL]    = STR(STATE_ARRIVING_TO_CELL)
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* The returned pointer is guaranteed to be valid to write to for
 * so long as we don't add anything to the table. At that point, there
 * is a case that a 'realloc' might take place. */
static struct movestate *movestate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;
    return &kh_value(s_entity_state_table, k);
}

static void flock_try_remove(struct flock *flock, uint32_t uid)
{
    khiter_t k;
    if((k = kh_get(entity, flock->ents, uid)) != kh_end(flock->ents)) {
        kh_del(entity, flock->ents, k);
        G_Formation_RemoveUnit(uid);
    }
}

static void flock_add(struct flock *flock, uint32_t uid)
{
    int ret;
    khiter_t k = kh_put(entity, flock->ents, uid, &ret);
    assert(ret != -1 && ret != 0);
}

static bool flock_contains(const struct flock *flock, uint32_t uid)
{
    khiter_t k = kh_get(entity, flock->ents, uid);
    if(k != kh_end(flock->ents))
        return true;
    return false;
}

static struct flock *flock_for_ent(uint32_t uid)
{
    for(int i = 0; i < vec_size(&s_flocks); i++) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);            
        if(flock_contains(curr_flock, uid))
            return curr_flock;
    }
    return NULL;
}

uint32_t flock_id_for_ent(uint32_t uid, const struct flock **out)
{
    for(int i = 0; i < vec_size(&s_flocks); i++) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);            
        khiter_t k = kh_get(entity, curr_flock->ents, uid);
        if(k != kh_end(curr_flock->ents)) {
            *out = curr_flock;
            return (i + 1);
        }
    }
    *out = NULL;
    return 0;
}

static struct flock *flock_for_dest(dest_id_t id)
{
    for(int i = 0; i < vec_size(&s_flocks); i++) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);            
        if(curr_flock->dest_id == id)
            return curr_flock;
    }
    return NULL;
}

static void entity_block(uint32_t uid)
{
    float sel_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersIncref(pos, sel_radius, 
        G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid), flags, s_map);

    struct movestate *ms = movestate_get(uid);
    assert(!ms->blocking);

    ms->blocking = true;
    ms->last_stop_pos = pos;
    ms->last_stop_radius = sel_radius;
    struct entity_block_desc *desc = stalloc(&s_eventargs, sizeof(struct entity_block_desc));
    *desc = (struct entity_block_desc){
        .uid = uid,
        .radius = sel_radius,
        .pos = pos
    };
    E_Global_Notify(EVENT_MOVABLE_ENTITY_BLOCK, desc, ES_ENGINE);
}

static void entity_unblock(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms->blocking);

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, faction_id, flags, s_map);
    ms->blocking = false;

    struct entity_block_desc *desc = stalloc(&s_eventargs, sizeof(struct entity_block_desc));
    *desc = (struct entity_block_desc){
        .uid = uid,
        .radius = ms->last_stop_radius,
        .pos = ms->last_stop_pos
    };
    E_Global_Notify(EVENT_MOVABLE_ENTITY_UNBLOCK, desc, ES_ENGINE);
}

static bool stationary(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return true;

    if(ms->max_speed == 0.0f)
        return true;

    return false;
}

static bool entities_equal(uint32_t *a, uint32_t *b)
{
    return (*a == *b);
}

static void vec2_truncate(vec2_t *inout, float max_len)
{
    if(PFM_Vec2_Len(inout) > max_len) {

        PFM_Vec2_Normal(inout, inout);
        PFM_Vec2_Scale(inout, max_len, inout);
    }
}

static bool ent_still(const struct movestate *ms)
{
    return (ms->state == STATE_ARRIVED || ms->state == STATE_WAITING);
}

static float entity_speed(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    if(G_Formation_GetForEnt(uid) != NULL_FID) {
        return G_Formation_Speed(uid);
    }

    struct movestate *ms = movestate_get(uid);
    return ms->max_speed;
}

static void entity_finish_moving(uint32_t uid, enum arrival_state newstate, bool block)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    assert(!ent_still(ms));
    uint32_t flags = G_FlagsGet(uid);

    E_Entity_Notify(EVENT_MOTION_END, uid, NULL, ES_ENGINE);
    if(flags & ENTITY_FLAG_COMBATABLE
    && (newstate != STATE_TURNING)) {
        G_Combat_SetStance(uid, COMBAT_STANCE_AGGRESSIVE);
    }

    if(newstate == STATE_WAITING) {
        ms->wait_prev = ms->state;
        ms->wait_ticks_left = WAIT_TICKS;
    }

    ms->state = newstate;
    ms->velocity = (vec2_t){0.0f, 0.0f};

    if(block) {
        entity_block(uid);
    }
    assert(ent_still(ms));
}

static void on_marker_anim_finish(void *user, void *event)
{
    ASSERT_IN_MAIN_THREAD();
    uint32_t ent = (uintptr_t)user;

    int idx = vec_entity_indexof(&s_move_markers, ent, entities_equal);
    assert(idx != -1);
    vec_entity_del(&s_move_markers, idx);

    E_Entity_Unregister(EVENT_ANIM_FINISHED, ent, on_marker_anim_finish);
    G_RemoveEntity(ent);
    G_FreeEntity(ent);
}

static void remove_from_flocks(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    /* Remove any flocks which may have become empty. Iterate vector in backwards order 
     * so that we can delete while iterating, since the last element in the vector takes
     * the place of the deleted one. 
     */
    for(int i = vec_size(&s_flocks)-1; i >= 0; i--) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);
        flock_try_remove(curr_flock, uid);

        if(kh_size(curr_flock->ents) == 0) {
            kh_destroy(entity, curr_flock->ents);
            vec_flock_del(&s_flocks, i);
        }
    }
    assert(NULL == flock_for_ent(uid));
}

static void filter_selection_pathable(const vec_entity_t *in_sel, vec_entity_t *out_sel)
{
    ASSERT_IN_MAIN_THREAD();

    vec_entity_init(out_sel);
    for(int i = 0; i < vec_size(in_sel); i++) {

        uint32_t curr = vec_AT(in_sel, i);
        struct movestate *ms = movestate_get(curr);
        if(!ms)
            continue;

        vec2_t xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        if(!M_NavPositionPathable(s_map, Entity_NavLayerWithRadius(flags, radius), xz_pos))
            continue;
        vec_entity_push(out_sel, curr);
    }
}

static void split_into_layers(const vec_entity_t *sel, vec_entity_t layer_flocks[])
{
    ASSERT_IN_MAIN_THREAD();

    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        vec_entity_init(layer_flocks + i);
    }

    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);
        vec_entity_push(&layer_flocks[layer], curr);
    }
}

static bool make_flock(const vec_entity_t *units, vec2_t target_xz, 
                       enum nav_layer layer, bool attack, enum formation_type type)
{
    ASSERT_IN_MAIN_THREAD();

    if(vec_size(units) == 0)
        return true;

    bool ret;
    uint32_t first = vec_AT(units, 0);

    /* The following won't be optimal when the entities in the unitsection are on different 
     * 'islands'. Handling that case is not a top priority. 
     */
    vec2_t first_ent_pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, first);
    target_xz = M_NavClosestReachableDest(s_map, layer, first_ent_pos_xz, target_xz);

    /* First remove the entities in the unitsection from any active flocks */
    for(int i = 0; i < vec_size(units); i++) {

        uint32_t curr_ent = vec_AT(units, i);
        remove_from_flocks(curr_ent);
    }

    struct flock new_flock = (struct flock) {
        .ents = kh_init(entity),
        .target_xz = target_xz,
    };

    if(!new_flock.ents)
        return false;

    for(int i = 0; i < vec_size(units); i++) {

        uint32_t curr_ent = vec_AT(units, i);
        if(stationary(curr_ent))
            continue;

        struct movestate *ms = movestate_get(curr_ent);
        assert(ms);

        if(ent_still(ms)) {
            entity_unblock(curr_ent); 
            E_Entity_Notify(EVENT_MOTION_START, curr_ent, NULL, ES_ENGINE);
        }

        flock_add(&new_flock, curr_ent);
        ms->state = (type == FORMATION_NONE) ? STATE_MOVING : STATE_MOVING_IN_FORMATION;
    }

    /* The flow fields will be computed on-demand during the next movement update tick */
    new_flock.target_xz = target_xz;
    if(attack) {
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, first);
        new_flock.dest_id = M_NavDestIDForPosAttacking(s_map, target_xz, layer, faction_id);
    }else{
        new_flock.dest_id = M_NavDestIDForPos(s_map, target_xz, layer);
    }

    if(kh_size(new_flock.ents) == 0) {
        kh_destroy(entity, new_flock.ents);
        return false;
    }

    /* If there is another flock with the same dest_id, then we merge the two flocks. */
    struct flock *merge_flock = flock_for_dest(new_flock.dest_id);
    if(merge_flock) {

        uint32_t curr;
        kh_foreach_key(new_flock.ents, curr, { flock_add(merge_flock, curr); });
        kh_destroy(entity, new_flock.ents);
    
    }else{
        formation_id_t fid;
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, first);
        vec_flock_push(&s_flocks, new_flock);
    }

    s_last_cmd_dest_valid = true;
    s_last_cmd_dest = new_flock.dest_id;
    return true;
}

static void make_flocks(const vec_entity_t *sel, vec2_t target_xz, vec2_t target_orientation, 
                        enum formation_type type, bool attack)
{
    ASSERT_IN_MAIN_THREAD();

    vec_entity_t fsel;
    filter_selection_pathable(sel, &fsel);

    if(vec_size(&fsel) == 0)
        return;

    vec_entity_t layer_flocks[NAV_LAYER_MAX];
    split_into_layers(&fsel, layer_flocks);

    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        make_flock(layer_flocks + i, target_xz, i, attack, type);
        vec_entity_destroy(layer_flocks + i);
    }

    G_Formation_Create(target_xz, target_orientation, &fsel, type);
    vec_entity_destroy(&fsel);
}

static size_t adjacent_flock_members(uint32_t uid, const struct flock *flock, 
                                     uint32_t out[])
{
    vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    size_t ret = 0;
    uint32_t curr;

    kh_foreach_key(flock->ents, curr, {

        if(curr == uid)
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        PFM_Vec2_Sub(&ent_xz_pos, &curr_xz_pos, &diff);

        float radius_uid = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        float radius_curr = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);

        if(PFM_Vec2_Len(&diff) <= radius_uid + radius_curr + ADJACENCY_SEP_DIST) {
            out[ret++] = curr;  
        }
    });
    return ret;
}

static void move_marker_add(vec3_t pos, bool attack)
{
    uint32_t flags;
    const uint32_t uid = Entity_NewUID();
    bool loaded = attack 
                ? AL_EntityFromPFObj("assets/models/arrow", "arrow-red.pfobj", 
                                     "__move_marker__", uid, &flags) 
                : AL_EntityFromPFObj("assets/models/arrow", "arrow-green.pfobj", 
                                     "__move_marker__", uid, &flags);
    if(!loaded)
        return;

    flags |= ENTITY_FLAG_MARKER;
    G_AddEntity(uid, flags, pos);

    Entity_SetScale(uid, (vec3_t){2.0f, 2.0f, 2.0f});
    E_Entity_Register(EVENT_ANIM_FINISHED, uid, on_marker_anim_finish, 
        (void*)(uintptr_t)uid, G_RUNNING);
    A_SetActiveClip(uid, "Converge", ANIM_MODE_ONCE, 48);

    vec_entity_push(&s_move_markers, uid);
}

static void move_order(const vec_entity_t *sel, bool attack, vec3_t mouse_coord, 
                       vec2_t orientation)
{
    size_t nmoved = 0;
    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);
        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;

        G_StopEntity(curr, false, true);
        E_Entity_Notify(EVENT_MOVE_ISSUED, curr, NULL, ES_ENGINE);
        G_NotifyOrderIssued(curr, true);
        nmoved++;

        if(flags & ENTITY_FLAG_COMBATABLE) {
            G_Combat_SetStance(curr, 
                attack ? COMBAT_STANCE_AGGRESSIVE : COMBAT_STANCE_NO_ENGAGEMENT);
        }
    }

    if(nmoved) {
        move_marker_add(mouse_coord, attack);
        vec_entity_t *copy = malloc(sizeof(vec_entity_t));
        vec_entity_init(copy);
        vec_entity_copy(copy, (vec_entity_t*)sel);
        move_push_cmd((struct move_cmd){
            .type = MOVE_CMD_MAKE_FLOCKS,
            .args[0] = (struct attr){
                .type = TYPE_POINTER,
                .val.as_pointer = copy
            },
            .args[1] = (struct attr){
                .type = TYPE_VEC2,
                .val.as_vec2 = (vec2_t){mouse_coord.x, mouse_coord.z}
            },
            .args[2] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = G_Formation_PreferredForSet(copy)
            },
            .args[3] = (struct attr){
                .type = TYPE_BOOL,
                .val.as_bool = attack
            },
            .args[4] = (struct attr){
                .type = TYPE_VEC2,
                .val.as_vec2 = orientation
            }
        });
    }
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    bool targeting = G_Move_InTargetMode();
    bool attack = s_attack_on_lclick && (mouse_event->button == SDL_BUTTON_LEFT);
    bool move = s_move_on_lclick ? mouse_event->button == SDL_BUTTON_LEFT
                                 : mouse_event->button == SDL_BUTTON_RIGHT;

    assert(!s_move_on_lclick || !s_attack_on_lclick);
    assert(!attack || !move);

    s_attack_on_lclick = false;
    s_move_on_lclick = false;

    if(!s_click_move_enabled)
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    if((mouse_event->button == SDL_BUTTON_RIGHT) && targeting)
        return;

    if(!attack && !move)
        return;

    if(G_CurrContextualAction() != CTX_ACTION_NONE)
        return;

    if(G_MouseInTargetMode() && !targeting)
        return;

    vec3_t mouse_coord;
    if(!M_MinimapMouseMapCoords(s_map, &mouse_coord)
    && !M_Raycast_MouseIntersecCoord(&mouse_coord))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    vec_entity_t fsel;
    filter_selection_pathable(sel, &fsel);

    if(vec_size(&fsel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return;

    /* Allow dragging the mouse to orient the formation around 
     * the clicked location. The move orders will be issued when
     * the mouse is released. 
     */
    if(G_Formation_PreferredForSet(&fsel) != FORMATION_NONE) {
        s_mouse_dragged = true;
        s_drag_begin_pos = mouse_coord;
        s_drag_end_pos = mouse_coord;
        s_drag_attacking = attack;
        return;
    }

    move_order(&fsel, attack, mouse_coord, (vec2_t){0.0f, 0.0f});
    vec_entity_destroy(&fsel);
}

static void on_mouseup(void *user, void *event)
{
    if(!s_mouse_dragged)
        return;
    s_mouse_dragged = false;

    enum selection_type seltype;
    const vec_entity_t *sel = G_Sel_Get(&seltype);

    vec2_t endpoints[] = {
        (vec2_t){s_drag_begin_pos.x, s_drag_begin_pos.z},
        (vec2_t){s_drag_end_pos.x, s_drag_end_pos.z}
    };

    vec2_t orientation;
    PFM_Vec2_Sub(&endpoints[1], &endpoints[0], &orientation);
    if(PFM_Vec2_Len(&orientation) < 0.1f) {
        orientation = G_Formation_AutoOrientation(endpoints[0], sel);
    }else{
        PFM_Vec2_Normal(&orientation, &orientation);
    }
    move_order(sel, s_drag_attacking, s_drag_begin_pos, orientation);
}

static void on_mousemotion(void *user, void *event)
{
    if(!s_mouse_dragged)
        return;

    vec3_t mouse_coord = (vec3_t){0.0f, 0.0f, 0.0f};
    if(!M_Raycast_MouseIntersecCoord(&mouse_coord))
        return;

    s_drag_end_pos = mouse_coord;
}

static void render_formation_orientation(void)
{
    vec2_t endpoints[] = {
        (vec2_t){s_drag_begin_pos.x, s_drag_begin_pos.z},
        (vec2_t){s_drag_end_pos.x, s_drag_end_pos.z}
    };

    vec2_t delta;
    PFM_Vec2_Sub(&endpoints[1], &endpoints[0], &delta);
    if(PFM_Vec2_Len(&delta) > EPSILON) {
        PFM_Vec2_Normal(&delta, &delta);
    }

    float width = 1.0f;
    vec3_t green = (vec3_t){140.0f / 255.0f, 240.0f / 255.0f, 140.0f / 255.0f};
    vec3_t red = (vec3_t){230.0f / 255.0f, 64.0f / 255.0f, 85.0f / 255.0f};

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawLine,
        .nargs = 4,
        .args = {
            R_PushArg(endpoints, sizeof(endpoints)),
            R_PushArg(&width, sizeof(width)),
            R_PushArg(s_drag_attacking ? &red : &green, sizeof(vec3_t)),
            (void*)G_GetPrevTickMap()
        }
    });
    enum selection_type seltype;
    const vec_entity_t *sel = G_Sel_Get(&seltype);
    
    G_Formation_RenderPlacement(sel, endpoints[0], delta);
}

static void on_render_3d(void *user, void *event)
{
    if(s_mouse_dragged) {
        render_formation_orientation();
    }

    const struct camera *cam = G_GetActiveCamera();
    enum nav_layer layer;

    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.navigation_layer", &setting);
    assert(status == SS_OKAY);
    layer = setting.as_int;

    status = Settings_Get("pf.debug.show_last_cmd_flow_field", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool && s_last_cmd_dest_valid) {
        M_NavRenderVisiblePathFlowField(s_map, cam, s_last_cmd_dest);
    }

    status = Settings_Get("pf.debug.show_first_sel_movestate", &setting);
    assert(status == SS_OKAY);

    enum selection_type seltype;
    const vec_entity_t *sel = G_Sel_Get(&seltype);

    if(setting.as_bool && vec_size(sel) > 0) {
    
        uint32_t ent = vec_AT(sel, 0);
        struct movestate *ms = movestate_get(ent);
        if(ms) {

            char strbuff[256];
            pf_snprintf(strbuff, ARR_SIZE(strbuff), "Arrival State: %s Velocity: (%f, %f)", 
                s_state_str[ms->state], ms->velocity.x, ms->velocity.z);
            struct rgba text_color = (struct rgba){255, 0, 0, 255};
            UI_DrawText(strbuff, (struct rect){5,50,600,50}, text_color);

            const struct camera *cam = G_GetActiveCamera();
            struct flock *flock = flock_for_ent(ent);

            switch(ms->state) {
            case STATE_MOVING:
            case STATE_MOVING_IN_FORMATION:
            case STATE_ENTER_ENTITY_RANGE:
                assert(flock);
                M_NavRenderVisiblePathFlowField(s_map, cam, flock->dest_id);
                break;
            case STATE_SURROUND_ENTITY: {

                if(!G_EntityExists(ms->surround_target_uid))
                    break;

                if(ms->using_surround_field) {
                    float radius = G_GetSelectionRadiusFrom(
                        s_move_work.gamestate.sel_radiuses, ent);
                    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, ent);
                    int layer = Entity_NavLayerWithRadius(flags, radius); 
                    M_NavRenderVisibleSurroundField(s_map, cam, layer, ms->surround_target_uid);
                    UI_DrawText("(Surround Field)", (struct rect){5,75,600,50}, text_color);
                }else{
                    M_NavRenderVisiblePathFlowField(s_map, cam, flock->dest_id);
                    UI_DrawText("(Path Field)", (struct rect){5,75,600,50}, text_color);
                }
                break;
            }
            case STATE_ARRIVED:
            case STATE_WAITING:
            case STATE_TURNING:
                break;
            case STATE_SEEK_ENEMIES: {
                float radius = G_GetSelectionRadiusFrom(
                    s_move_work.gamestate.sel_radiuses, ent);
                uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, ent);
                int layer = Entity_NavLayerWithRadius(flags, radius); 
                int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, ent);
                M_NavRenderVisibleEnemySeekField(s_map, cam, layer, faction_id);
                break;
            }
            case STATE_ARRIVING_TO_CELL:
                /* Following the cell arrival field */
                break;
            default: 
                assert(0);
            }
        }
    }

    status = Settings_Get("pf.debug.show_enemy_seek_fields", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {

        status = Settings_Get("pf.debug.enemy_seek_fields_faction_id", &setting);
        assert(status == SS_OKAY);
    
        M_NavRenderVisibleEnemySeekField(s_map, cam, layer, setting.as_int);
    }

    status = Settings_Get("pf.debug.show_navigation_blockers", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_NavRenderNavigationBlockers(s_map, cam, layer);
    }

    status = Settings_Get("pf.debug.show_navigation_portals", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_NavRenderNavigationPortals(s_map, cam, layer);
    }

    status = Settings_Get("pf.debug.show_navigation_cost_base", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_RenderVisiblePathableLayer(s_map, cam, layer);
    }

    status = Settings_Get("pf.debug.show_chunk_boundaries", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_RenderChunkBoundaries(s_map, cam);
    }

    status = Settings_Get("pf.debug.show_navigation_island_ids", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_NavRenderNavigationIslandIDs(s_map, cam, layer);
    }

    status = Settings_Get("pf.debug.show_navigation_local_island_ids", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_NavRenderNavigationLocalIslandIDs(s_map, cam, layer);
    }
}

static quat_t dir_quat_from_velocity(vec2_t velocity)
{
    assert(PFM_Vec2_Len(&velocity) > EPSILON);

    float angle_rad = atan2(velocity.raw[1], velocity.raw[0]) - M_PI/2.0f;
    return (quat_t) {
        0.0f, 
        1.0f * sin(angle_rad / 2.0f),
        0.0f,
        cos(angle_rad / 2.0f)
    };
}

static bool entity_exists(uint32_t uid)
{
    khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
    return (k != kh_end(s_move_work.gamestate.positions));
}

static void request_async_field(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms || ent_still(ms))
        return;

    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    struct flock *fl = flock_for_ent(uid);

    switch(ms->state) {
    case STATE_SEEK_ENEMIES:  {
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
        int layer = Entity_NavLayerWithRadius(flags, radius);
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
        return M_NavRequestAsyncEnemySeekField(s_move_work.gamestate.map, 
            layer, pos_xz, faction_id);
    }
    case STATE_SURROUND_ENTITY: {

        if(!entity_exists(ms->surround_target_uid))
            return;

        if(ms->using_surround_field) {
            float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
            uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
            int layer = Entity_NavLayerWithRadius(flags, radius);
            int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
            return M_NavRequestAsyncSurroundField(s_move_work.gamestate.map, layer, pos_xz, 
                ms->surround_target_uid, faction_id);
        }
        break;
    }
    default:;
        /* No-op */
    }
}

static vec2_t ent_desired_velocity(uint32_t uid, vec2_t cell_arrival_vdes)
{
    const struct movestate *ms = movestate_get(uid);
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    struct flock *fl = flock_for_ent(uid);

    switch(ms->state) {
    case STATE_TURNING:
        return (vec2_t){0.0f, 0.0f};

    case STATE_SEEK_ENEMIES:  {
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
        int layer = Entity_NavLayerWithRadius(flags, radius);
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
        return M_NavDesiredEnemySeekVelocity(s_move_work.gamestate.map, layer, pos_xz, faction_id);
    }
    case STATE_SURROUND_ENTITY: {

        if(!entity_exists(ms->surround_target_uid)) {
            return M_NavDesiredPointSeekVelocity(s_move_work.gamestate.map, fl->dest_id, 
                pos_xz, fl->target_xz);
        }

        if(ms->using_surround_field) {
            float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
            uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
            int layer = Entity_NavLayerWithRadius(flags, radius);
            int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
            return M_NavDesiredSurroundVelocity(s_move_work.gamestate.map, layer, pos_xz, 
                ms->surround_target_uid, faction_id);
        }else{
            return M_NavDesiredPointSeekVelocity(s_move_work.gamestate.map, fl->dest_id, 
                pos_xz, fl->target_xz);
        }
        break;
    }
    case STATE_ARRIVING_TO_CELL: {
        return cell_arrival_vdes;
    }
    default:
        assert(fl);
        return M_NavDesiredPointSeekVelocity(s_move_work.gamestate.map, fl->dest_id, 
            pos_xz, fl->target_xz);
    }
}

/* Seek behaviour makes the entity target and approach a particular destination point.
 */
static vec2_t seek_force(uint32_t uid, vec2_t target_xz)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);

    PFM_Vec2_Sub(&target_xz, &pos_xz, &desired_velocity);
    PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
    PFM_Vec2_Scale(&desired_velocity, ms->max_speed / hz_count(s_move_work.hz), &desired_velocity);

    PFM_Vec2_Sub(&desired_velocity, &ms->velocity, &ret);
    return ret;
}

/* Arrival behaviour is like 'seek' but the entity decelerates and comes to a halt when it is 
 * within a threshold radius of the destination point.
 * 
 * When not within line of sight of the destination, this will steer the entity along the 
 * flow field.
 */
static vec2_t arrive_force_point(uint32_t uid, vec2_t target_xz, vec2_t vdes, bool has_dest_los)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float distance;

    struct movestate *ms = movestate_get(uid);
    assert(ms);

    if(has_dest_los) {

        PFM_Vec2_Sub(&target_xz, &pos_xz, &desired_velocity);
        distance = PFM_Vec2_Len(&desired_velocity);
        PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
        PFM_Vec2_Scale(&desired_velocity, ms->max_speed / hz_count(s_move_work.hz), 
            &desired_velocity);

        if(distance < ARRIVE_SLOWING_RADIUS) {
            PFM_Vec2_Scale(&desired_velocity, distance / ARRIVE_SLOWING_RADIUS, &desired_velocity);
        }
    }else{
        PFM_Vec2_Scale(&vdes, ms->max_speed / hz_count(s_move_work.hz), &desired_velocity);
    }

    PFM_Vec2_Sub(&desired_velocity, &ms->velocity, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t arrive_force_cell(uint32_t uid, vec2_t cell_xz, vec2_t vdes)
{
    struct movestate *ms = movestate_get(uid);
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float distance;

    vec2_t desired_velocity;
    PFM_Vec2_Sub(&cell_xz, &pos_xz, &desired_velocity);
    distance = PFM_Vec2_Len(&desired_velocity);

    if(distance < ARRIVE_SLOWING_RADIUS) {
        PFM_Vec2_Scale(&desired_velocity, distance / ARRIVE_SLOWING_RADIUS, &desired_velocity);
    }else{
        PFM_Vec2_Scale(&vdes, ms->max_speed / hz_count(s_move_work.hz), &desired_velocity);
    }
    return desired_velocity;
}

static vec2_t arrive_force_enemies(uint32_t uid, vec2_t vdes)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float distance;

    const struct movestate *ms = movestate_get(uid);
    assert(ms);

    PFM_Vec2_Scale(&vdes, ms->max_speed / hz_count(s_move_work.hz), &desired_velocity);
    PFM_Vec2_Sub(&desired_velocity, (vec2_t*)&ms->velocity, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

/* Alignment is a behaviour that causes a particular agent to line up with agents close by.
 */
static vec2_t alignment_force(uint32_t uid, const struct flock *flock)
{
    vec2_t ret = (vec2_t){0.0f};
    size_t neighbour_count = 0;

    uint32_t curr;
    kh_foreach_key(flock->ents, curr, {

        if(curr == uid)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);

        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);
        if(PFM_Vec2_Len(&diff) < ALIGN_NEIGHBOUR_RADIUS) {

            struct movestate *ms = movestate_get(uid);
            assert(ms);

            if(PFM_Vec2_Len(&ms->velocity) < EPSILON)
                continue; 

            PFM_Vec2_Add(&ret, &ms->velocity, &ret);
            neighbour_count++;
        }
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    struct movestate *ms = movestate_get(uid);
    assert(ms);

    PFM_Vec2_Scale(&ret, 1.0f / neighbour_count, &ret);
    PFM_Vec2_Sub(&ret, &ms->velocity, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

/* Cohesion is a behaviour that causes agents to steer towards the center of mass of nearby agents.
 */
static vec2_t cohesion_force(uint32_t uid, const struct flock *flock)
{
    vec2_t COM = (vec2_t){0.0f};
    size_t neighbour_count = 0;
    vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);

    uint32_t curr;
    kh_foreach_key(flock->ents, curr, {

        if(curr == uid)
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        float t = (PFM_Vec2_Len(&diff) - COHESION_NEIGHBOUR_RADIUS*0.75) 
                / COHESION_NEIGHBOUR_RADIUS;
        float scale = exp(-6.0f * t);

        PFM_Vec2_Scale(&curr_xz_pos, scale, &curr_xz_pos);
        PFM_Vec2_Add(&COM, &curr_xz_pos, &COM);
        neighbour_count++;
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    vec2_t ret;
    PFM_Vec2_Scale(&COM, 1.0f / neighbour_count, &COM);
    PFM_Vec2_Sub(&COM, &ent_xz_pos, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

/* Separation is a behaviour that causes agents to steer away from nearby agents.
 */
static vec2_t separation_force(uint32_t uid, float buffer_dist)
{
    vec2_t ret = (vec2_t){0.0f};
    uint32_t ent_flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    uint32_t near_ents[128];
    int num_near = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree,
        s_move_work.gamestate.flags,
        G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid), 
        SEPARATION_NEIGHB_RADIUS, near_ents, ARR_SIZE(near_ents));

    for(int i = 0; i < num_near; i++) {

        uint32_t curr = near_ents[i];
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        if(curr == uid)
            continue;
        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;
        if((ent_flags & ENTITY_FLAG_AIR) != (flags & ENTITY_FLAG_AIR))
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);

        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid) 
                     + G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr) 
                     + buffer_dist;
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        if(PFM_Vec2_Len(&diff) < EPSILON)
            continue;

        /* Exponential decay with y=1 when diff = radius*0.85 
         * Use smooth decay curves in order to curb the 'toggling' or oscillating 
         * behaviour that may arise when there are discontinuities in the forces. 
         */
        float t = (PFM_Vec2_Len(&diff) - radius*0.85) / PFM_Vec2_Len(&diff);
        float scale = exp(-20.0f * t);
        PFM_Vec2_Scale(&diff, scale, &diff);

        PFM_Vec2_Add(&ret, &diff, &ret);
    }

    if(0 == num_near)
        return (vec2_t){0.0f};

    PFM_Vec2_Scale(&ret, -1.0f, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t point_seek_total_force(uint32_t uid, const struct flock *flock, 
                                     vec2_t vdes, bool has_dest_los)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t arrive = arrive_force_point(uid, flock->target_xz, vdes, has_dest_los);
    vec2_t cohesion = cohesion_force(uid, flock);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);
    PFM_Vec2_Add(&ret, &cohesion, &ret);

    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t cell_seek_total_force(uint32_t uid, vec2_t cell_pos, vec2_t vdes,
                                    vec2_t cohesion, vec2_t alignment)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t delta;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    PFM_Vec2_Sub(&cell_pos, &pos_xz, &delta);

    vec2_t arrive = arrive_force_cell(uid, cell_pos, vdes);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&alignment,  ALIGNMENT_FORCE_SCALE,     &alignment);

    vec2_t ret = (vec2_t){0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);

    if(PFM_Vec2_Len(&delta) > CELL_ARRIVAL_RADIUS) {
        PFM_Vec2_Add(&ret, &cohesion, &ret);
        PFM_Vec2_Add(&ret, &alignment, &ret);
    }

    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t enemy_seek_total_force(uint32_t uid, vec2_t vdes)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t arrive = arrive_force_enemies(uid, vdes);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f, 0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);

    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t new_pos_for_vel(uint32_t uid, vec2_t velocity)
{
    vec2_t xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t new_pos;

    PFM_Vec2_Add(&xz_pos, &velocity, &new_pos);
    return new_pos;
}

/* Nullify the components of the force which would guide
 * the entity towards an impassable tile. */
static void nullify_impass_components(uint32_t uid, vec2_t *inout_force)
{
    vec2_t nt_dims = N_TileDims();
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t left =  (vec2_t){pos.x + nt_dims.x, pos.z};
    vec2_t right = (vec2_t){pos.x - nt_dims.x, pos.z};
    vec2_t top =   (vec2_t){pos.x, pos.z + nt_dims.z};
    vec2_t bot =   (vec2_t){pos.x, pos.z - nt_dims.z};

    if(inout_force->x > 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, left)  
      || M_NavPositionBlocked(s_move_work.gamestate.map, layer, left)))
        inout_force->x = 0.0f;

    if(inout_force->x < 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, right) 
     || M_NavPositionBlocked(s_move_work.gamestate.map, layer, right)))
        inout_force->x = 0.0f;

    if(inout_force->z > 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, top) 
      || M_NavPositionBlocked(s_move_work.gamestate.map, layer, top)))
        inout_force->z = 0.0f;

    if(inout_force->z < 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, bot) 
      || M_NavPositionBlocked(s_move_work.gamestate.map, layer, bot)))
        inout_force->z = 0.0f;
}

static vec2_t point_seek_vpref(uint32_t uid, const struct flock *flock, 
                               vec2_t vdes, bool has_dest_los, float speed)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force;
    for(int prio = 0; prio < 3; prio++) {

        switch(prio) {
        case 0: 
            steer_force = point_seek_total_force(uid, flock, vdes, has_dest_los); 
            break;
        case 1: 
            steer_force = separation_force(uid, SEPARATION_BUFFER_DIST); 
            break;
        case 2: 
            steer_force = arrive_force_point(uid, flock->target_xz, vdes, has_dest_los); 
            break;
        }

        nullify_impass_components(uid, &steer_force);
        if(PFM_Vec2_Len(&steer_force) > SCALED_MAX_FORCE * 0.01)
            break;
    }

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, speed / hz_count(s_move_work.hz));

    return new_vel;
}

static vec2_t cell_arrival_seek_vpref(uint32_t uid, vec2_t cell_pos, float speed, vec2_t vdes,
                                      vec2_t cohesion, vec2_t alignment, vec2_t drag)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force;
    for(int prio = 0; prio < 3; prio++) {

        switch(prio) {
        case 0: 
            steer_force = cell_seek_total_force(uid, cell_pos, vdes, cohesion, alignment); 
            break;
        case 1: 
            steer_force = separation_force(uid, SEPARATION_BUFFER_DIST); 
            break;
        case 2: 
            steer_force = arrive_force_cell(uid, cell_pos, vdes); 
            break;
        }

        nullify_impass_components(uid, &steer_force);
        if(PFM_Vec2_Len(&steer_force) > SCALED_MAX_FORCE * 0.01)
            break;
    }

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, speed / hz_count(s_move_work.hz));
    if(PFM_Vec2_Len(&drag) > EPSILON) {
        vec2_truncate(&new_vel, (speed * 0.75) / hz_count(s_move_work.hz));
    }

    return new_vel;
}

static vec2_t enemy_seek_vpref(uint32_t uid, float speed, vec2_t vdes)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force = enemy_seek_total_force(uid, vdes);

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, speed / hz_count(s_move_work.hz));

    return new_vel;
}

static vec2_t formation_point_seek_total_force(uint32_t uid, const struct flock *flock, vec2_t vdes,
                                               vec2_t cohesion, vec2_t alignment, bool has_dest_los)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t arrive = arrive_force_point(uid, flock->target_xz, vdes, has_dest_los);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);
    PFM_Vec2_Scale(&alignment,  ALIGNMENT_FORCE_SCALE,     &alignment);

    vec2_t ret = (vec2_t){0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);
    PFM_Vec2_Add(&ret, &cohesion, &ret);

    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t formation_seek_vpref(uint32_t uid, const struct flock *flock, float speed,
                                   vec2_t vdes, vec2_t cohesion, vec2_t alignment, vec2_t drag,
                                   bool has_dest_los)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force;
    for(int prio = 0; prio < 3; prio++) {

        switch(prio) {
        case 0: 
            steer_force = formation_point_seek_total_force(uid, flock, 
                vdes, cohesion, alignment, has_dest_los); 
            break;
        case 1: 
            steer_force = separation_force(uid, SEPARATION_BUFFER_DIST); 
            break;
        case 2: 
            steer_force = arrive_force_point(uid, flock->target_xz, vdes, has_dest_los); 
            break;
        }

        nullify_impass_components(uid, &steer_force);
        if(PFM_Vec2_Len(&steer_force) > SCALED_MAX_FORCE * 0.01)
            break;
    }

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, speed / hz_count(s_move_work.hz));
    if(PFM_Vec2_Len(&drag) > EPSILON) {
        vec2_truncate(&new_vel, (speed * 0.75) / hz_count(s_move_work.hz));
    }

    return new_vel;
}

static void update_vel_hist(struct movestate *ms, vec2_t vnew)
{
    ASSERT_IN_MAIN_THREAD();

    assert(ms->vel_hist_idx >= 0 && ms->vel_hist_idx < VEL_HIST_LEN);
    ms->vel_hist[ms->vel_hist_idx] = vnew;
    ms->vel_hist_idx = ((ms->vel_hist_idx+1) % VEL_HIST_LEN);
}

/* Simple Moving Average */
static vec2_t vel_sma(const struct movestate *ms)
{
    vec2_t ret = {0};
    for(int i = 0; i < VEL_HIST_LEN; i++)
        PFM_Vec2_Add(&ret, (vec2_t*)&ms->vel_hist[i], &ret); 
    PFM_Vec2_Scale(&ret, 1.0f/VEL_HIST_LEN, &ret);
    return ret;
}

/* Weighted Moving Average */
static vec2_t vel_wma(const struct movestate *ms)
{
    vec2_t ret = {0};
    float denom = 0.0f;

    for(int i = 0; i < VEL_HIST_LEN; i++) {

        vec2_t term = ms->vel_hist[(ms->vel_hist_idx + i) % VEL_HIST_LEN];
        PFM_Vec2_Scale(&term, VEL_HIST_LEN-i, &term);
        PFM_Vec2_Add(&ret, &term, &ret);
        denom += (VEL_HIST_LEN-i);
    }

    if(denom > EPSILON) {
        PFM_Vec2_Scale(&ret, 1.0f/denom, &ret);
    }
    return ret;
}

static bool uids_match(void *arg, struct move_cmd *cmd)
{
    uint32_t desired_uid = (uintptr_t)arg;
    uint32_t actual_uid = cmd->args[0].val.as_int;
    return (desired_uid == actual_uid);
}

static struct move_cmd *snoop_most_recent_command(enum move_cmd_type type, void *arg,
                                                  bool (*pred)(void*, struct move_cmd*),
                                                  bool remove)
{
    if(queue_size(s_move_commands) == 0)
        return NULL;

    size_t left = queue_size(s_move_commands);
    for(int i = s_move_commands.itail; left > 0;) {
        struct move_cmd *curr = &s_move_commands.mem[i];
        if(!curr->deleted && curr->type == type) {
            if(pred(arg, curr)) {
                curr->deleted = remove;
                return curr;
            }
        }
        i--;
        left--;
        if(i < 0) {
            i = s_move_commands.capacity - 1; /* Wrap around */
        }
    }
    return NULL;
}

static bool snoop_still(uint32_t uid)
{
    if(queue_size(s_move_commands) == 0) {
        struct movestate *ms = movestate_get(uid);
        assert(ms);
        return (ms->state == STATE_ARRIVED);
    }

    size_t left = queue_size(s_move_commands);
    for(int i = s_move_commands.itail; left > 0;) {
        struct move_cmd *curr = &s_move_commands.mem[i];
        switch(curr->type) {
        case MOVE_CMD_SET_DEST:
        case MOVE_CMD_CHANGE_DIRECTION:
        case MOVE_CMD_SET_ENTER_RANGE:
        case MOVE_CMD_SET_SEEK_ENEMIES:
        case MOVE_CMD_SET_SURROUND_ENTITY: {
            if(curr->args[0].val.as_int == uid)
                return false;
            break;
        }
        case MOVE_CMD_STOP:
            if(curr->args[0].val.as_int == uid)
                return true;
            break;
        default:
            break;
        }
        i--;
        left--;
        if(i < 0) {
            i = s_move_commands.capacity - 1; /* Wrap around */
        }
    }

    struct movestate *ms = movestate_get(uid);
    assert(ms);
    return (ms->state == STATE_ARRIVED);
}

static void flush_update_pos_commands(uint32_t uid)
{
    struct move_cmd *cmd;
    while((cmd = snoop_most_recent_command(MOVE_CMD_UPDATE_POS, 
        (void*)(uintptr_t)uid, uids_match, true))) {

        uint32_t uid = cmd->args[0].val.as_int;
        vec2_t pos = cmd->args[1].val.as_vec2;
        do_update_pos(uid, pos);
    }
}

static bool arrived(uint32_t uid, vec2_t xz_pos)
{
    vec2_t diff_to_target;
    struct flock *flock = flock_for_ent(uid);
    assert(flock);

    PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &xz_pos, &diff_to_target);
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    float arrive_thresh = radius * 1.5f;
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    if(PFM_Vec2_Len(&diff_to_target) < arrive_thresh
    || (M_NavIsAdjacentToImpassable(s_map, layer, xz_pos) 
        && M_NavIsMaximallyClose(s_map, layer, xz_pos, flock->target_xz, arrive_thresh))) {
        return true;
    }

    vec2_t nearest;
    if(M_NavClosestPathable(s_map, layer, flock->target_xz, &nearest)) {
        vec2_t delta;
        PFM_Vec2_Sub(&nearest, &xz_pos, &delta);
        if(PFM_Vec2_Len(&delta) < arrive_thresh)
            return true;
    }

    return false;
}

static float unit_height(uint32_t uid, vec2_t pos)
{
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    if(flags & ENTITY_FLAG_WATER)
        return 0.0f;
    if(flags & ENTITY_FLAG_AIR) {
        return M_HeightAtPoint(s_map, pos) + AIR_UNIT_HEIGHT;
    }
    return M_HeightAtPoint(s_map, pos);
}

static int hz_count(enum movement_hz hz)
{
    switch(hz) {
    case MOVE_HZ_20:    return 20;
    case MOVE_HZ_10:    return 10;
    case MOVE_HZ_5:     return 5;
    case MOVE_HZ_1:     return 1;
    default: assert(0);
    }
    return 0;
}

static vec3_t interpolate_positions(vec3_t from, vec3_t to, float fraction)
{
    assert(fraction >= 0.0f && fraction <= 1.0f);

    if(fabs(1.0 - fraction) < EPSILON)
        return to;

    vec3_t delta;
    PFM_Vec3_Sub(&to, &from, &delta);
    PFM_Vec3_Scale(&delta, fraction, &delta);

    vec3_t ret;
    PFM_Vec3_Add(&from, &delta, &ret);
    return ret;
}

static quat_t interpolate_rotations(quat_t from, quat_t to, float fraction)
{
    assert(fraction >= 0.0f && fraction <= 1.0f);

    if(fabs(1.0 - fraction) < EPSILON)
        return to;

    return PFM_Quat_Slerp(&from, &to, fraction);
}

/* Derive the patch that should be applied onto the movestate 
 * as a result of the current navigation tick. The patch can be
 * generated asynchronously, but applied synchronously.
 */
static void entity_compute_update(enum movement_hz hz, uint32_t uid, vec2_t new_vel, vec2_t vdes,
                                  const struct move_work_in *in, struct movestate_patch *out)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);
    out->flags = 0;

    /* Flush the interpolation if was not completed */
    if(ms->left > 0) {
        out->flags |= UPDATE_SET_POSITION | UPDATE_SET_ROTATION | UPDATE_SET_LEFT;
        out->next_pos = ms->next_pos;
        out->next_rot = ms->next_rot;
        out->next_left = 0;
    }

    assert(hz_count(hz) <= 20);
    assert(20 % hz_count(hz) == 0);

    vec2_t new_pos_xz = new_pos_for_vel(uid, new_vel);
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    if(flags & ENTITY_FLAG_GARRISONED) {
        if(!ent_still(ms)) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = false;
        }
        return;
    }

    if(PFM_Vec2_Len(&new_vel) > 0
    && M_NavPositionPathable(s_move_work.gamestate.map, layer, new_pos_xz)) {
    
        vec3_t new_pos = (vec3_t){new_pos_xz.x, unit_height(uid, new_pos_xz), new_pos_xz.z};

        out->flags |= UPDATE_SET_PREV_POS | UPDATE_SET_NEXT_POS | UPDATE_SET_STEP | UPDATE_SET_LEFT;
        out->next_ppos = ms->next_pos;
        out->next_npos = new_pos;
        out->next_step = 1.0f / (20 / hz_count(hz));
        out->next_left = (20 / hz_count(hz)) - 1;

        if(out->next_left == 0) {
            out->flags |= UPDATE_SET_POSITION;
            out->next_pos = new_pos;
        }else{
            vec3_t intermediate = interpolate_positions(out->next_ppos, out->next_npos, ms->step);
            new_pos_xz = (vec2_t){intermediate.x, intermediate.z};
            out->flags |= UPDATE_SET_POSITION;
            out->next_pos = intermediate;
        }

        out->flags |= UPDATE_SET_VELOCITY;
        out->next_velocity = new_vel;

        /* Use a weighted average of past velocities ot set the entity's orientation. 
         * This means that the entity's visible orientation lags behind its' true orientation 
         * slightly. However, this greatly smooths the turning of the entity, giving a more 
         * natural look to the movemment. 
         */

        out->flags |= UPDATE_SET_PREV_ROT;
        out->next_prot = ms->next_rot;

        vec2_t wma = vel_wma(ms);
        if(PFM_Vec2_Len(&wma) > EPSILON) {
            out->flags |= UPDATE_SET_NEXT_ROT;
            out->next_nrot = dir_quat_from_velocity(wma);
        }else{
            out->flags |= UPDATE_SET_NEXT_ROT;
            out->next_nrot = ms->prev_rot;
        }
        out->flags |= UPDATE_SET_ROTATION;
        out->next_rot = ms->next_rot;

    }else{
        out->flags |= UPDATE_SET_VELOCITY;
        out->next_velocity = (vec2_t){0.0f, 0.0f};
    }

    /* If the entity's current position isn't pathable, simply keep it 'stuck' there in
     * the same state it was in before. Under normal conditions, no entity can move from 
     * pathable terrain to non-pathable terrain, but an this violation is possible by 
     * forcefully setting the entity's position from a scripting call. 
     */
    if(!M_NavPositionPathable(s_move_work.gamestate.map, layer, new_pos_xz))
        return;

    switch(ms->state) {
    case STATE_MOVING: 
    case STATE_MOVING_IN_FORMATION: {

        if((in->fstate.fid != NULL_FID) && !in->fstate.assignment_ready)
            break;

        if(in->fstate.fid != NULL_FID
        && in->fstate.assigned_to_cell
        && in->fstate.in_range_of_cell) {

            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVING_TO_CELL;
            break;
        }

        struct flock *flock = flock_for_ent(uid);
        assert(flock);

        if(arrived(uid, new_pos_xz)) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
        }

        STALLOC(uint32_t, adjacent, kh_size(flock->ents));
        size_t num_adj = adjacent_flock_members(uid, flock, adjacent);

        bool done = false;
        for(int j = 0; j < num_adj; j++) {

            struct movestate *adj_ms = movestate_get(adjacent[j]);
            assert(adj_ms);

            if(adj_ms->state == STATE_ARRIVED) {

                out->flags |= UPDATE_SET_STATE;
                out->next_state = STATE_ARRIVED;
                out->next_block = true;
                done = true;
                break;
            }
        }
        STFREE(adjacent);

        if(done) {
            break;
        }

        /* If we've not hit a condition to stop or give up but our desired velocity 
         * is zero, that means the navigation system is currently not able to guide
         * the entity any closer to its' goal. Stop and wait, re-requesting the  path 
         * after some time. 
         */
        if(PFM_Vec2_Len(&vdes) < EPSILON) {

            assert(flock_for_ent(uid));
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_WAITING;
            out->next_block = true;
            break;
        }
        break;
    }
    case STATE_SEEK_ENEMIES: {

        if(PFM_Vec2_Len(&vdes) < EPSILON) {

            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_WAITING;
            out->next_block = true;
        }
        break;
    }
    case STATE_SURROUND_ENTITY: {

        if(ms->surround_target_uid == NULL_UID) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
        }

        if(!entity_exists(ms->surround_target_uid)
        ||  M_NavObjAdjacent(s_move_work.gamestate.map, uid, ms->surround_target_uid)) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
        }

        vec2_t target_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, 
            ms->surround_target_uid);
        vec2_t dest = ms->surround_nearest_prev;

        vec2_t delta;
        PFM_Vec2_Sub(&target_pos, &ms->surround_target_prev, &delta);
        if(PFM_Vec2_Len(&delta) > EPSILON || PFM_Vec2_Len(&ms->velocity) < EPSILON) {

            bool hasdest = M_NavClosestReachableAdjacentPos(s_move_work.gamestate.map, layer, 
                new_pos_xz, ms->surround_target_uid, &dest);

            if(!hasdest) {
                out->flags |= UPDATE_SET_STATE;
                out->next_state = STATE_ARRIVED;
                out->next_block = true;
                break;
            }
        }

        struct flock *flock = flock_for_ent(uid);
        assert(flock);

        vec2_t diff;
        PFM_Vec2_Sub(&flock->target_xz, &dest, &diff);
        ms->surround_target_prev = target_pos;
        ms->surround_nearest_prev = dest;

        if(PFM_Vec2_Len(&diff) > EPSILON) {
            out->flags |= UPDATE_SET_DEST | UPDATE_SET_STATE;
            out->next_dest = dest;
            out->next_attack = false;
            out->next_state = STATE_SURROUND_ENTITY;
            break;
        }

        if(PFM_Vec2_Len(&vdes) < EPSILON) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_WAITING;
            out->next_block = true;
        }
        break;
    }
    case STATE_ENTER_ENTITY_RANGE: {

        if(ms->surround_target_uid == NULL_UID) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
        }

        vec2_t xz_target = G_Pos_GetXZFrom(s_move_work.gamestate.positions, 
            ms->surround_target_uid);

        vec2_t delta;
        PFM_Vec2_Sub(&new_pos_xz, &xz_target, &delta);

        if(PFM_Vec2_Len(&delta) <= ms->target_range
        || (M_NavIsAdjacentToImpassable(s_move_work.gamestate.map, layer, new_pos_xz) 
            && M_NavIsMaximallyClose(s_move_work.gamestate.map, layer, new_pos_xz, xz_target, 0.0f))) {
        
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_WAITING;
            out->next_block = true;
            break;
        }

        vec2_t target_delta;
        PFM_Vec2_Sub(&xz_target, &ms->target_prev_pos, &target_delta);

        if(PFM_Vec2_Len(&target_delta) > 5.0f) {
            out->flags |= UPDATE_SET_DEST | UPDATE_SET_TARGET_PREV;
            out->next_dest = xz_target;
            out->next_attack = false;
            out->next_target_prev = xz_target;
        }

        break;
    }
    case STATE_TURNING: {

        /* find the angle between the two quaternions */
        quat_t ent_rot = Entity_GetRot(uid);
        float angle_diff = PFM_Quat_PitchDiff(&ent_rot, &ms->target_dir);
        float degrees = RAD_TO_DEG(angle_diff);

        /* if it's within a tolerance, stop turning */
        if(fabs(degrees) <= 5.0f) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
        }

        /* If not, find the amount we should turn by around the Y axis */
        float turn_deg = MIN(MAX_TURN_RATE, fabs(degrees)) * -SIGNUM(degrees);
        float turn_rad = DEG_TO_RAD(turn_deg);
        mat4x4_t rotmat;
        PFM_Mat4x4_MakeRotY(turn_rad, &rotmat);
        quat_t rot;
        PFM_Quat_FromRotMat(&rotmat, &rot);

        /* Turn */
        quat_t final;
        PFM_Quat_MultQuat(&rot, &ent_rot, &final);
        PFM_Quat_Normal(&final, &final);

        out->flags |= UPDATE_SET_ROTATION | UPDATE_SET_PREV_ROT;
        out->next_rot = final;
        out->next_prot = final;

        break;
    }
    case STATE_WAITING: {

        assert(ms->wait_ticks_left > 0);
        ms->wait_ticks_left--;
        if(ms->wait_ticks_left == 0) {

            assert(ms->wait_prev == STATE_MOVING 
                || ms->wait_prev == STATE_MOVING_IN_FORMATION
                || ms->wait_prev == STATE_SEEK_ENEMIES
                || ms->wait_prev == STATE_SURROUND_ENTITY);

            out->flags |= UPDATE_SET_MOVING;
            out->next_state = ms->wait_prev;
        }
        break;
    }
    case STATE_ARRIVED:
        break;
    case STATE_ARRIVING_TO_CELL: {
        if(in->fstate.fid == NULL_FID) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_MOVING;
            break;
        }
        if(!in->fstate.assignment_ready)
            break;
        if(!in->fstate.in_range_of_cell) {
            /* We got pushed off of the cell arrival field */
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_MOVING_IN_FORMATION;
            break;
        }
        if(in->fstate.arrived_at_cell) {
            out->flags |= UPDATE_SET_STATE | UPDATE_SET_TARGET_DIR;
            out->next_target_dir = in->fstate.target_orientation;
            out->next_state = STATE_TURNING;
            break;
        }
        break;
    }
    default: 
        assert(0);
    }
}

static void ent_update_using_surround_field(uint32_t uid, struct movestate *ms)
{
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t target_pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, ms->surround_target_uid);
    float dx = fabs(target_pos_xz.x - pos_xz.x);
    float dz = fabs(target_pos_xz.z - pos_xz.z);

    if(!ms->using_surround_field) {
        if(dx < SURROUND_LOW_WATER_X && dz < SURROUND_LOW_WATER_Z) {
            ms->using_surround_field = true;
        }
    }else{
        if(dx >= SURROUND_HIGH_WATER_X || dz >= SURROUND_HIGH_WATER_Z) {
            ms->using_surround_field = false;
        }
    }
}

static void entity_apply_update(uint32_t uid, const struct movestate_patch *patch)
{
    ASSERT_IN_MAIN_THREAD();

    if(!G_EntityExists(uid) || G_EntityIsZombie(uid))
        return;

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    if(patch->flags & UPDATE_SET_STATE) {

        if(patch->next_state == STATE_ARRIVED || (patch->next_state == STATE_WAITING)) {
            entity_finish_moving(uid, patch->next_state, patch->next_block);
        }else{
            ms->state = patch->next_state;
        }
    }

    if(patch->flags & UPDATE_SET_VELOCITY) {
        ms->velocity = patch->next_velocity;
        update_vel_hist(ms, ms->velocity);
    }

    if(patch->flags & UPDATE_SET_POSITION)
        G_Pos_Set(uid, patch->next_pos);

    if(patch->flags & UPDATE_SET_ROTATION)
        Entity_SetRot(uid, patch->next_rot);

    if(patch->flags & UPDATE_SET_PREV_POS)
        ms->prev_pos = patch->next_ppos;

    if(patch->flags & UPDATE_SET_NEXT_POS)
        ms->next_pos = patch->next_npos;

    if(patch->flags & UPDATE_SET_STEP)
        ms->step = patch->next_step;

    if(patch->flags & UPDATE_SET_LEFT)
        ms->left = patch->next_left;

    if(patch->flags & UPDATE_SET_PREV_ROT)
        ms->prev_rot = patch->next_prot;

    if(patch->flags & UPDATE_SET_NEXT_ROT)
        ms->next_rot = patch->next_nrot;

    if(patch->flags & UPDATE_SET_TARGET_PREV)
        ms->target_prev_pos = patch->next_target_prev;

    if(patch->flags & UPDATE_SET_TARGET_DIR)
        ms->target_dir = patch->next_target_dir;

    if(patch->flags & UPDATE_SET_STATE
    && (patch->next_state == STATE_ARRIVED || (patch->next_state == STATE_WAITING)))

    if(patch->flags & UPDATE_SET_MOVING) {
        entity_unblock(uid);
        E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
        ms->state = patch->next_state;
    }

    if(ms->state == STATE_SURROUND_ENTITY) {
        ent_update_using_surround_field(uid, ms);
    }
}

static void find_neighbours(uint32_t uid,
                            vec_cp_ent_t *out_dyn,
                            vec_cp_ent_t *out_stat)
{
    /* For the ClearPath algorithm, we only consider entities with
     * ENTITY_FLAG_MOVABLE set, as they are the only ones that may need
     * to be avoided during moving. Here, 'static' entites refer
     * to those entites that are not currently in a 'moving' state,
     * meaning they will not perform collision avoidance maneuvers of
     * their own. */

    uint32_t ent_flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    uint32_t near_ents[512];
    int num_near = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree, 
        s_move_work.gamestate.flags,
        G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid), 
        CLEARPATH_NEIGHBOUR_RADIUS, near_ents, ARR_SIZE(near_ents));

    for(int i = 0; i < num_near; i++) {

        uint32_t curr = near_ents[i];
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);

        if(curr == uid)
            continue;

        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;

        if(G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr) == 0.0f)
            continue;

        if((ent_flags & ENTITY_FLAG_AIR) != (flags & ENTITY_FLAG_AIR))
            continue;

        struct movestate *ms = movestate_get(curr);
        assert(ms);

        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        struct cp_ent newdesc = (struct cp_ent) {
            .xz_pos = curr_xz_pos,
            .xz_vel = ms->velocity,
            .radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr)
        };

        if(ent_still(ms)) {
            if(vec_size(out_stat) < MAX_NEIGHBOURS)
                vec_cp_ent_push(out_stat, newdesc);
        }else {
            if(vec_size(out_dyn) < MAX_NEIGHBOURS)
                vec_cp_ent_push(out_dyn, newdesc);
        }
    }
}

static void disband_empty_flocks(void)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_ENTER();

    uint32_t curr;
    /* Iterate vector backwards so we can delete entries while iterating. */
    for(int i = vec_size(&s_flocks)-1; i >= 0; i--) {

        /* First, decide if we can disband this flock */
        bool disband = true;
        kh_foreach_key(vec_AT(&s_flocks, i).ents, curr, {

            struct movestate *ms = movestate_get(curr);
            assert(ms);

            if(ms->state != STATE_ARRIVED) {
                disband = false;
                break;
            }
        });

        if(disband) {

            struct flock *flock = &vec_AT(&s_flocks, i);
            uint32_t uid;
            kh_foreach_key(flock->ents, uid, {
                G_Formation_RemoveUnit(uid);
            });
            kh_destroy(entity, flock->ents);
            vec_flock_del(&s_flocks, i);
        }
    }
    PERF_RETURN_VOID();
}

static void do_add_entity(uint32_t uid, vec3_t pos, float selection_radius, int faction_id)
{
    ASSERT_IN_MAIN_THREAD();

    int ret;
    khiter_t k = kh_put(pos, s_move_work.gamestate.positions, uid, &ret);
    assert(ret != -1);
    kh_val(s_move_work.gamestate.positions, k) = pos;

    qt_ent_insert(s_move_work.gamestate.postree, pos.x, pos.z, uid);

    k = kh_put(range, s_move_work.gamestate.sel_radiuses, uid, &ret);
    assert(ret != -1);
    kh_value(s_move_work.gamestate.sel_radiuses, k) = selection_radius;

    k = kh_put(id, s_move_work.gamestate.faction_ids, uid, &ret);
    assert(ret != -1);
    kh_value(s_move_work.gamestate.faction_ids, k) = faction_id;

    k = kh_put(id, s_move_work.gamestate.flags, uid, &ret);
    assert(ret != -1);
    kh_value(s_move_work.gamestate.flags, k) = G_FlagsGet(uid);

    struct movestate new_ms = (struct movestate) {
        .velocity = {0.0f}, 
        .blocking = false,
        .state = STATE_ARRIVED,
        .vel_hist_idx = 0,
        .max_speed = 0.0f,
        .left = 0,
        .prev_pos = pos,
        .next_pos = pos,
        .surround_target_prev = (vec2_t){0},
        .surround_nearest_prev = (vec2_t){0},
    };
    memset(new_ms.vel_hist, 0, sizeof(new_ms.vel_hist));

    k = kh_put(state, s_entity_state_table, uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = new_ms;

    entity_block(uid);
}

static void do_remove_entity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return;

    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);

    do_stop(uid);
    if(!(flags & ENTITY_FLAG_GARRISONED)) {
        entity_unblock(uid);
    }

    kh_del(state, s_entity_state_table, k);
}

static void do_stop(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    if(!ent_still(ms)) {
        entity_finish_moving(uid, STATE_ARRIVED, true);
    }

    remove_from_flocks(uid);
    ms->state = STATE_ARRIVED;
}

static void do_set_dest(uint32_t uid, vec2_t dest_xz, bool attack)
{
    ASSERT_IN_MAIN_THREAD();

    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);
    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    dest_xz = M_NavClosestReachableDest(s_map, layer, pos, dest_xz);

    /* If a flock already exists for the entity's destination, 
     * simply add the entity to the flock. If necessary, the
     * right flow fields will be computed on-demand during the
     * next movement update. 
     */
    dest_id_t dest_id;
    if(attack) {
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
        dest_id = M_NavDestIDForPosAttacking(s_map, dest_xz, layer, faction_id);
    }else{
        dest_id = M_NavDestIDForPos(s_map, dest_xz, layer);
    }
    struct flock *fl = flock_for_dest(dest_id);

    if(fl && fl == flock_for_ent(uid)) { 
        struct movestate *ms = movestate_get(uid);
        assert(ms);
        if(ent_still(ms)) {
            entity_unblock(uid);
            E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
        }
        ms->state = STATE_MOVING;
        return;
    }

    if(fl) {

        assert(fl != flock_for_ent(uid));
        remove_from_flocks(uid);
        flock_add(fl, uid);

        struct movestate *ms = movestate_get(uid);
        assert(ms);
        if(ent_still(ms)) {
            entity_unblock(uid);
            E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
        }
        ms->state = STATE_MOVING;
        assert(flock_for_ent(uid));
        return;
    }

    /* Else, create a new flock and request a path for it.
     */
    vec_entity_t flock;
    vec_entity_init(&flock);
    vec_entity_push(&flock, uid);

    enum formation_type type = FORMATION_NONE;
    formation_id_t fid = G_Formation_GetForEnt(uid);
    if(fid != NULL_FID) {
        type = G_Formation_Type(fid);
    }

    make_flock(&flock, dest_xz, layer, attack, type);
    vec_entity_destroy(&flock);
}

static void do_set_change_direction(uint32_t uid, quat_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    if(ent_still(ms)) {
        entity_unblock(uid);
        E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
    }

    ms->state = STATE_TURNING;
    ms->target_dir = target;
}

static void do_set_enter_range(uint32_t uid, uint32_t target, float range)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    vec2_t xz_src = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t xz_dst = G_Pos_GetXZFrom(s_move_work.gamestate.positions, target);
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    range = MAX(0.0f, range - radius);

    vec2_t delta;
    PFM_Vec2_Sub(&xz_src, &xz_dst, &delta);
    if(PFM_Vec2_Len(&delta) <= range) {
        do_stop(uid);
        return;
    }

    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    vec2_t xz_target = M_NavClosestReachableInRange(s_map, 
        Entity_NavLayerWithRadius(flags, radius), xz_src, xz_dst, range - radius);
    do_set_dest(uid, xz_target, false);

    ms->state = STATE_ENTER_ENTITY_RANGE;
    ms->surround_target_uid = target;
    ms->target_prev_pos = xz_dst;
    ms->target_range = range;
}

static bool using_surround_field(uint32_t uid, uint32_t target)
{
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t target_pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, target);

    float dx = fabs(target_pos_xz.x - pos_xz.x);
    float dz = fabs(target_pos_xz.z - pos_xz.z);
    return (dx < SURROUND_LOW_WATER_X && dz < SURROUND_LOW_WATER_Z);
}

static void do_set_surround_entity(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    do_stop(uid);

    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, target);
    do_set_dest(uid, pos, false);

    assert(!ms->blocking);
    ms->state = STATE_SURROUND_ENTITY;
    ms->surround_target_uid = target;
    ms->using_surround_field = using_surround_field(uid, target);
}

static void do_set_seek_enemies(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    remove_from_flocks(uid);

    if(ent_still(ms)) {
        entity_unblock(uid);
        E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
    }

    ms->state = STATE_SEEK_ENEMIES;
}

static void do_update_pos(uint32_t uid, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    vec3_t newpos = {
        pos.x,
        unit_height(uid, pos),
        pos.z
    };

    khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
    assert(k != kh_end(s_move_work.gamestate.positions));
    vec3_t oldpos = kh_val(s_move_work.gamestate.positions, k);
    qt_ent_delete(s_move_work.gamestate.postree, oldpos.x, oldpos.z, uid);
    qt_ent_insert(s_move_work.gamestate.postree, newpos.x, newpos.z, uid);
    kh_val(s_move_work.gamestate.positions, k) = newpos;

    if(!ms->blocking)
        return;

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, faction_id, flags, s_map);
    M_NavBlockersIncref(pos, ms->last_stop_radius, faction_id, flags, s_map);
    ms->last_stop_pos = pos;
    ms->prev_pos = newpos;
    ms->next_pos = newpos;
}

static void do_update_faction_id(uint32_t uid, int oldfac, int newfac)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    khiter_t k = kh_get(id, s_move_work.gamestate.faction_ids, uid);
    assert(k != kh_end(s_move_work.gamestate.faction_ids));
    kh_val(s_move_work.gamestate.faction_ids, k) = newfac;

    if(!ms->blocking)
        return;

    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, oldfac, flags, s_map);
    M_NavBlockersIncref(ms->last_stop_pos, ms->last_stop_radius, newfac, flags, s_map);
}

static void do_update_selection_radius(uint32_t uid, float sel_radius)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    khiter_t k = kh_get(range, s_move_work.gamestate.sel_radiuses, uid);
    assert(k != kh_end(s_move_work.gamestate.sel_radiuses));
    kh_val(s_move_work.gamestate.sel_radiuses, k) = sel_radius;

    if(!ms->blocking)
        return;

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, faction_id, flags, s_map);
    M_NavBlockersIncref(ms->last_stop_pos, sel_radius, faction_id, flags, s_map);
    ms->last_stop_radius = sel_radius;
}

static void do_set_max_speed(uint32_t uid, float speed)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    ms->max_speed = speed;
}

static void do_block(uint32_t uid, vec3_t newpos)
{
    khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
    assert(k != kh_end(s_move_work.gamestate.positions));
    vec3_t oldpos = kh_val(s_move_work.gamestate.positions, k);
    qt_ent_delete(s_move_work.gamestate.postree, oldpos.x, oldpos.z, uid);
    qt_ent_insert(s_move_work.gamestate.postree, newpos.x, newpos.z, uid);
    kh_val(s_move_work.gamestate.positions, k) = newpos;

    entity_block(uid);
}

static void move_push_cmd(struct move_cmd cmd)
{
    queue_cmd_push(&s_move_commands, &cmd);
}

static void move_process_cmds(void)
{
    struct move_cmd cmd;
    while(queue_cmd_pop(&s_move_commands, &cmd)) {

        if(cmd.deleted)
            continue;

        switch(cmd.type) {
        case MOVE_CMD_ADD: {
            uint32_t uid = cmd.args[0].val.as_int;
            vec3_t pos = cmd.args[1].val.as_vec3;
            float radius = cmd.args[2].val.as_float;
            int faction_id = cmd.args[3].val.as_int;
            do_add_entity(uid, pos, radius, faction_id);
            break;
        }
        case MOVE_CMD_REMOVE: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_remove_entity(uid);
            break;
        }
        case MOVE_CMD_STOP: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_stop(uid);
            break;
        }
        case MOVE_CMD_SET_DEST: {
            uint32_t uid = cmd.args[0].val.as_int;
            vec2_t dest_xz = cmd.args[1].val.as_vec2;
            bool attack = cmd.args[2].val.as_bool;
            do_set_dest(uid, dest_xz, attack);
            break;
        }
        case MOVE_CMD_CHANGE_DIRECTION: {
            uint32_t uid = cmd.args[0].val.as_int;
            quat_t target = cmd.args[1].val.as_quat;
            do_set_change_direction(uid, target);
            break;
        }
        case MOVE_CMD_SET_ENTER_RANGE: {
            uint32_t uid = cmd.args[0].val.as_int;
            uint32_t target = cmd.args[1].val.as_int;
            float range = cmd.args[2].val.as_float;
            do_set_enter_range(uid, target, range);
            break;
        }
        case MOVE_CMD_SET_SEEK_ENEMIES: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_set_seek_enemies(uid);
            break;
        }
        case MOVE_CMD_SET_SURROUND_ENTITY: {
            uint32_t uid = cmd.args[0].val.as_int;
            uint32_t target = cmd.args[1].val.as_int;
            do_set_surround_entity(uid, target);
            break;
        }
        case MOVE_CMD_UPDATE_POS: {
            uint32_t uid = cmd.args[0].val.as_int;
            vec2_t pos = cmd.args[1].val.as_vec2;
            do_update_pos(uid, pos);
            break;
        }
        case MOVE_CMD_UPDATE_FACTION_ID: {
            uint32_t uid = cmd.args[0].val.as_int;
            int oldfac = cmd.args[1].val.as_int;
            int newfac = cmd.args[2].val.as_int;
            do_update_faction_id(uid, oldfac, newfac);
            break;
        }
        case MOVE_CMD_UPDATE_SELECTION_RADIUS: {
            uint32_t uid = cmd.args[0].val.as_int;
            float radius = cmd.args[1].val.as_float;
            do_update_selection_radius(uid, radius);
            break;
        }
        case MOVE_CMD_SET_MAX_SPEED: {
            uint32_t uid = cmd.args[0].val.as_int;
            float speed = cmd.args[1].val.as_float;
            do_set_max_speed(uid, speed);
            break;
        }
        case MOVE_CMD_MAKE_FLOCKS: {
            vec_entity_t *sel = (vec_entity_t*)cmd.args[0].val.as_pointer;
            vec2_t target_xz = cmd.args[1].val.as_vec2;
            enum formation_type type = cmd.args[2].val.as_int;
            bool attack = cmd.args[3].val.as_bool;
            vec2_t target_orientation = cmd.args[4].val.as_vec2;
            make_flocks(sel, target_xz, target_orientation, type, attack);
            vec_entity_destroy(sel);
            PF_FREE(sel);
            break;
        }
        case MOVE_CMD_UNBLOCK: {
            uint32_t uid = cmd.args[0].val.as_int;
            struct movestate *ms = movestate_get(uid);
            if(ms && ms->blocking) {
                entity_unblock(uid);
            }
            break;
        }
        case MOVE_CMD_BLOCK: {
            uint32_t uid = cmd.args[0].val.as_int;
            vec3_t pos = cmd.args[1].val.as_vec3;
            struct movestate *ms = movestate_get(uid);
            if(ms && !ms->blocking) {
                do_block(uid, pos);
            }
            break;
        }
        default:
            assert(0);
        }
    }
}

static void *cp_vec_realloc(void *ptr, size_t size)
{
    ASSERT_IN_MAIN_THREAD();
    if(!ptr)
        return stalloc(&s_move_work.mem, size);

    void *ret = stalloc(&s_move_work.mem, size);
    if(!ret)
        return NULL;

    assert(size % 2 == 0);
    memcpy(ret, ptr, size / 2);
    return ret;
}

static void cp_vec_free(void *ptr)
{
    /* no-op */
}

static void move_velocity_work(int begin_idx, int end_idx)
{
    for(int i = begin_idx; i <= end_idx; i++) {
    
        struct move_work_in *in = &s_move_work.in[i];
        struct move_work_out *out = &s_move_work.out[i];

        const struct movestate *ms = movestate_get(in->ent_uid);
        const struct flock *flock = flock_for_ent(in->ent_uid);

        /* Compute the preferred velocity */
        vec2_t vpref = (vec2_t){NAN, NAN};
        switch(ms->state) {
        case STATE_TURNING:
            vpref = (vec2_t){0.0f, 0.0f};
            break;
        case STATE_SEEK_ENEMIES: 
            assert(!flock);
            vpref = enemy_seek_vpref(in->ent_uid, in->speed, in->ent_des_v);
            break;
        case STATE_ARRIVING_TO_CELL:
            assert(flock);
            if(!in->fstate.assignment_ready) {
                vpref = (vec2_t){0.0f, 0.0f};
                break;
            }
            vpref = cell_arrival_seek_vpref(in->ent_uid, in->cell_pos, in->speed,
                in->ent_des_v,
                in->fstate.normal_cohesion_force,
                in->fstate.normal_align_force,
                in->fstate.normal_drag_force);
            break;
        case STATE_MOVING_IN_FORMATION:
            assert(flock);
            if(!in->fstate.assignment_ready) {
                vpref = (vec2_t){0.0f, 0.0f};
                break;
            }
            vpref = formation_seek_vpref(in->ent_uid, flock, in->speed, 
                in->ent_des_v,
                in->fstate.normal_cohesion_force,
                in->fstate.normal_align_force,
                in->fstate.normal_drag_force,
                in->has_dest_los);
            break;
        default:
            assert(flock);
            vpref = point_seek_vpref(in->ent_uid, flock, 
                in->ent_des_v, in->has_dest_los, in->speed);
        }
        assert(vpref.x != NAN && vpref.z != NAN);

        /* Find the entity's neighbours */
        find_neighbours(in->ent_uid, in->dyn_neighbs, in->stat_neighbs);

        /* Compute the velocity constrainted by potential collisions */
        vec2_t new_vel = G_ClearPath_NewVelocity(in->cp_ent, in->ent_uid, 
            vpref, *in->dyn_neighbs, *in->stat_neighbs, in->save_debug);

        out->ent_uid = in->ent_uid;
        out->ent_vel = new_vel;
        vec2_truncate(&out->ent_vel, ms->max_speed / hz_count(s_move_work.hz));
    }
}

static void move_update_work(int begin_idx, int end_idx)
{
    for(int i = begin_idx; i <= end_idx; i++) {
    
        struct move_work_in *in = &s_move_work.in[i];
        struct move_work_out *out = &s_move_work.out[i];

        entity_compute_update(s_move_work.hz, out->ent_uid, out->ent_vel, 
            out->ent_des_v, in, &out->patch);
    }
}

static struct result move_velocity_task(void *arg)
{
    struct move_task_arg *move_arg = arg;
    size_t ncomputed = 0;

    for(int i = move_arg->begin_idx; i <= move_arg->end_idx; i++) {

        move_velocity_work(i, i);
        ncomputed++;

        if(ncomputed % 16 == 0)
            Task_Yield();
    }
    return NULL_RESULT;
}

static struct result move_update_task(void *arg)
{
    struct move_task_arg *move_arg = arg;
    size_t ncomputed = 0;

    for(int i = move_arg->begin_idx; i <= move_arg->end_idx; i++) {

        move_update_work(i, i);
        ncomputed++;

        if(ncomputed % 16 == 0)
            Task_Yield();
    }
    return NULL_RESULT;
}

static void move_complete_cpu_work(void)
{
    for(int i = 0; i < s_move_work.ntasks; i++) {
        while(!Sched_FutureIsReady(&s_move_work.futures[i])) {
            Sched_RunSync(s_move_work.tids[i]);
            Sched_TryYield();
        }
    }
    s_move_work.ntasks = 0;
}

static void move_complete_gpu_velocity_work(void)
{
    Task_RescheduleOnMain();
    ASSERT_IN_MAIN_THREAD();

    size_t nwork = s_move_work.nwork;
    size_t attr_buffsize = nwork * sizeof(vec2_t);

    R_PushCmd((struct rcmd){
        .func = R_GL_MoveReadNewVelocities,
        .nargs = 3,
        .args = {
            [0] = s_move_work.gpu_velocities,
            [1] = R_PushArg(&nwork, sizeof(size_t)),
            [2] = R_PushArg(&attr_buffsize, sizeof(size_t))
        }
    });

    R_PushCmd((struct rcmd){
        .func = R_GL_MoveInvalidateData,
        .nargs = 0
    });

    R_PushCmd((struct rcmd){
        .func = R_GL_PositionsInvalidateData,
        .nargs = 0
    });
}

static khash_t(aabb) *move_copy_aabbs(void)
{
    PERF_ENTER();
    khash_t(aabb) *aabbs = kh_init(aabb);
    if(!aabbs)
        PERF_RETURN(NULL);

    const khash_t(entity) *ents = G_GetAllEntsSet();

    uint32_t uid;
    kh_foreach_key(ents, uid, {
        int ret;
        khiter_t k = kh_put(aabb, aabbs, uid, &ret);
        assert(ret != -1);
        kh_value(aabbs, k) = AL_EntityGet(uid)->identity_aabb;
    });
    PERF_RETURN(aabbs);
}

static void move_init_nav_unit_query_ctx(void)
{
    s_move_work.unit_query_ctx.flags = s_move_work.gamestate.flags;
    s_move_work.unit_query_ctx.positions = s_move_work.gamestate.positions;
    s_move_work.unit_query_ctx.postree = s_move_work.gamestate.postree;
    s_move_work.unit_query_ctx.faction_ids = s_move_work.gamestate.faction_ids;
    s_move_work.unit_query_ctx.aabbs = s_move_work.gamestate.aabbs;
    s_move_work.unit_query_ctx.transforms = s_move_work.gamestate.transforms;
    s_move_work.unit_query_ctx.sel_radiuses = s_move_work.gamestate.sel_radiuses;
    s_move_work.unit_query_ctx.fog_enabled = s_move_work.gamestate.fog_enabled;
    s_move_work.unit_query_ctx.fog_state = s_move_work.gamestate.fog_state;
    s_move_work.unit_query_ctx.dying_set = s_move_work.gamestate.dying_set;
    s_move_work.unit_query_ctx.diptable = (int(*)[MAX_FACTIONS])s_move_work.gamestate.diptable;
    s_move_work.unit_query_ctx.player_controllable = s_move_work.gamestate.player_controllable;
}

static void move_copy_gamestate(void)
{
    PERF_ENTER();
    s_move_work.gamestate.flags = G_FlagsCopyTable();
    s_move_work.gamestate.positions = G_Pos_CopyTable();
    s_move_work.gamestate.postree = G_Pos_CopyQuadTree();
    s_move_work.gamestate.sel_radiuses = G_SelectionRadiusCopyTable();
    s_move_work.gamestate.faction_ids = G_FactionIDCopyTable();
    s_move_work.gamestate.ent_gpu_id_map = G_CopyEntGPUIDMap();
    s_move_work.gamestate.gpu_id_ent_map = G_CopyGPUIDEntMap();
    s_move_work.gamestate.map = M_AL_CopyWithFields(s_map);
    s_move_work.gamestate.transforms = Entity_CopyTransforms();
    s_move_work.gamestate.aabbs = move_copy_aabbs();
    s_move_work.gamestate.fog_enabled = G_Fog_Enabled();
    s_move_work.gamestate.fog_state = G_Fog_CopyState();
    s_move_work.gamestate.dying_set = G_Combat_GetDyingSetCopy();
    s_move_work.gamestate.diptable = G_CopyDiplomacyTable();
    s_move_work.gamestate.player_controllable = G_GetPlayerControlledFactions();

    move_init_nav_unit_query_ctx();
    M_NavSetNavUnitQueryCtx(s_move_work.gamestate.map, &s_move_work.unit_query_ctx);

    PERF_RETURN_VOID();
}

static void move_release_gamestate(void)
{
    PERF_ENTER();
    if(s_move_work.gamestate.flags) {
        kh_destroy(id, s_move_work.gamestate.flags);
        s_move_work.gamestate.flags = NULL;
    }
    if(s_move_work.gamestate.positions) {
        kh_destroy(pos, s_move_work.gamestate.positions);
        s_move_work.gamestate.positions = NULL;
    }
    if(s_move_work.gamestate.postree) {
        G_Pos_DestroyQuadTree(s_move_work.gamestate.postree);
        s_move_work.gamestate.postree = NULL;
    }
    if(s_move_work.gamestate.sel_radiuses) {
        kh_destroy(range, s_move_work.gamestate.sel_radiuses);
        s_move_work.gamestate.sel_radiuses = NULL;
    }
    if(s_move_work.gamestate.faction_ids) {
        kh_destroy(id, s_move_work.gamestate.faction_ids);
        s_move_work.gamestate.faction_ids = NULL;
    }
    if(s_move_work.gamestate.ent_gpu_id_map) {
        kh_destroy(id, s_move_work.gamestate.ent_gpu_id_map);
        s_move_work.gamestate.ent_gpu_id_map = NULL;
    }
    if(s_move_work.gamestate.gpu_id_ent_map) {
        kh_destroy(id, s_move_work.gamestate.gpu_id_ent_map);
        s_move_work.gamestate.gpu_id_ent_map = NULL;
    }
    if(s_move_work.gamestate.map) {
        M_AL_FreeCopyWithFields((struct map*)s_move_work.gamestate.map);
        s_move_work.gamestate.map = NULL;
    }
    if(s_move_work.gamestate.transforms) {
        kh_destroy(trans, s_move_work.gamestate.transforms);
        s_move_work.gamestate.transforms = NULL;
    }
    if(s_move_work.gamestate.aabbs) {
        kh_destroy(aabb, s_move_work.gamestate.aabbs);
        s_move_work.gamestate.aabbs = NULL;
    }
    if(s_move_work.gamestate.fog_state) {
        PF_FREE(s_move_work.gamestate.fog_state);
        s_move_work.gamestate.fog_state = NULL;
    }
    if(s_move_work.gamestate.dying_set) {
        kh_destroy(id, s_move_work.gamestate.dying_set);
        s_move_work.gamestate.dying_set = NULL;
    }
    if(s_move_work.gamestate.diptable) {
        PF_FREE(s_move_work.gamestate.diptable);
        s_move_work.gamestate.diptable = NULL;
    }
    PERF_RETURN_VOID();
}

static void move_update_gamestate(void)
{
    move_release_gamestate();
    move_copy_gamestate();
}

static void move_consume_work_results(void)
{
    PERF_ENTER();

    if(s_move_work.nwork == 0)
        PERF_RETURN_VOID();

    PERF_PUSH("apply movement updates");

    for(int i = 0; i < s_move_work.nwork; i++) {
        struct move_work_out *out = &s_move_work.out[i];
        entity_apply_update(out->ent_uid, &out->patch);
    }

    PERF_POP();

    stalloc_clear(&s_move_work.mem);
    s_move_work.in = NULL;
    s_move_work.out = NULL;
    s_move_work.nwork = 0;
    s_move_work.ntasks = 0;

    PERF_RETURN_VOID();
}

static void move_prepare_work(enum movement_hz hz)
{
    size_t ndynamic = kh_size(G_GetDynamicEntsSet());
    s_move_work.in = stalloc(&s_move_work.mem, ndynamic * sizeof(struct move_work_in));
    s_move_work.out = stalloc(&s_move_work.mem, ndynamic * sizeof(struct move_work_out));
    s_move_work.hz = hz;
    s_move_work.type = (s_use_gpu ? WORK_TYPE_GPU : WORK_TYPE_CPU);
    SDL_AtomicSet(&s_move_work.gpu_velocities_ready, 0);
}

static void move_push_work(struct move_work_in in)
{
    s_move_work.in[s_move_work.nwork++] = in;
}

static void move_submit_cpu_work(task_func_t code)
{
    if(s_move_work.nwork == 0)
        return;

    size_t ntasks = SDL_GetCPUCount();
    if(s_move_work.nwork < 64)
        ntasks = 1;
    ntasks = MIN(ntasks, MAX_MOVE_TASKS);

    for(int i = 0; i < ntasks; i++) {

        struct move_task_arg *arg = stalloc(&s_move_work.mem, sizeof(struct move_task_arg));
        size_t nitems = ceil((float)s_move_work.nwork / ntasks);

        arg->begin_idx = nitems * i;
        arg->end_idx = MIN(nitems * (i + 1) - 1, s_move_work.nwork-1);

        SDL_AtomicSet(&s_move_work.futures[s_move_work.ntasks].status, FUTURE_INCOMPLETE);
        s_move_work.tids[s_move_work.ntasks] = Sched_Create(4, code, arg, 
            "move::work", &s_move_work.futures[s_move_work.ntasks], TASK_BIG_STACK);

        if(s_move_work.tids[s_move_work.ntasks] == NULL_TID) {
            code(arg);
        }else{
            s_move_work.ntasks++;
        }
    }
}

static struct move_work_in *work_input_for_uid(uint32_t uid)
{
    for(int i = 0; i < s_move_work.nwork; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        if(in->ent_uid == uid)
            return in;
    }
    return NULL;
}

static void move_upload_input(size_t nents)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_ENTER();

    /* Setup GPUID dispatch data.
     */
    struct render_workspace *ws = G_GetSimWS();
    const size_t gpuid_buffsize = s_move_work.nwork * sizeof(uint32_t);
    const size_t nactive = s_move_work.nwork;
    void *gpuid_buff = stalloc(&ws->args, gpuid_buffsize);
    unsigned char *cursor = gpuid_buff;

    for(int i = 0; i < s_move_work.nwork; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        uint32_t gpuid = G_GPUIDForEntFrom(s_move_work.gamestate.ent_gpu_id_map, in->ent_uid);
        *((uint32_t*)cursor) = gpuid;
        cursor += sizeof(uint32_t);
    }
    assert(cursor == ((unsigned char*)gpuid_buff) + gpuid_buffsize);

    /* Setup moveattr data.
     */
    const size_t attr_buffsize = nents * sizeof(struct gpu_ent_desc);
    void *attrbuff = stalloc(&ws->args, attr_buffsize);
    cursor = attrbuff;

    for(int gpu_id = 1; gpu_id <= nents; gpu_id++) {

        uint32_t uid = G_EntForGPUIDFrom(s_move_work.gamestate.gpu_id_ent_map, gpu_id);
        const struct movestate *curr = movestate_get(uid);
        assert(curr);

        const struct flock *flock;
        uint32_t flock_id = flock_id_for_ent(uid, &flock);
        uint32_t movestate = curr->state;
        vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        vec2_t dest_xz = flock ? flock->target_xz : (vec2_t){0.0f, 0.0f};

        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);

        struct move_work_in *work = NULL;
        if(!ent_still(curr)) {
            work = work_input_for_uid(uid);
        }

        *((struct gpu_ent_desc*)cursor) = (struct gpu_ent_desc){
            .dest = dest_xz,
            .vdes = work ? work->ent_des_v : (vec2_t){0},
            .cell_pos = work ? work->cell_pos : (vec2_t){0},
            .formation_cohesion_force  = work ? work->fstate.normal_cohesion_force : (vec2_t){0},
            .formation_align_force = work ? work->fstate.normal_align_force : (vec2_t){0},
            .formation_drag_force = work ? work->fstate.normal_drag_force : (vec2_t){0},
            .pos = pos,
            .velocity = curr->velocity,
            .movestate = curr->state,
            .flock_id = flock_id,
            .flags = flags,
            .speed = work ? work->speed : 0.0f,
            .max_speed = curr->max_speed,
            .radius = radius,
            .layer = Entity_NavLayerWithRadius(flags, radius),
            .has_dest_los = work ? work->has_dest_los : false,
            .formation_assignment_ready = work ? work->fstate.assignment_ready : 0,
        };
        cursor += sizeof(struct gpu_ent_desc);
    }
    assert(cursor == ((unsigned char*)attrbuff) + attr_buffsize);

    /* Setup flock data.
     */
    const size_t nflocks = vec_size(&s_flocks);
    const size_t flock_buffsize = nflocks * sizeof(struct gpu_flock_desc);
    void *flockbuff = stalloc(&ws->args, flock_buffsize);
    cursor = flockbuff;

    for(int i = 0; i < nflocks; i++) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);            
        uint32_t uid;
        size_t nents = 0;
        unsigned char *tmp = cursor;

        kh_foreach_key(curr_flock->ents, uid, {

            uint32_t gpuid = G_GPUIDForEntFrom(s_move_work.gamestate.ent_gpu_id_map, uid);
            *((uint32_t*)tmp) = gpuid; 
            tmp += sizeof(uint32_t);

            if(++nents == MAX_GPU_FLOCK_MEMBERS)
                break;
        });
        cursor += MAX_GPU_FLOCK_MEMBERS * sizeof(uint32_t);

        assert(nents == MIN(kh_size(curr_flock->ents), MAX_GPU_FLOCK_MEMBERS));
        *((uint32_t*)cursor) = (uint32_t)nents; 
        cursor += sizeof(uint32_t);

        *((vec2_t*)cursor) = curr_flock->target_xz;
        cursor += sizeof(vec2_t);
    }
    assert(cursor == ((unsigned char*)flockbuff) + flock_buffsize);

    /* Setup navigation data.
     */
    const size_t cost_base_buffsize = M_NavCostBaseBufferSize(s_move_work.gamestate.map);
    void *cost_base_buff = stalloc(&ws->args, cost_base_buffsize);
    M_NavCopyCostBasePacked(s_move_work.gamestate.map, cost_base_buff, cost_base_buffsize);

    const size_t blockers_buffsize = M_NavBlockersBufferSize(s_move_work.gamestate.map);
    void *blockers_buff = stalloc(&ws->args, blockers_buffsize);
    M_NavCopyBlockersPacked(s_move_work.gamestate.map, blockers_buff, blockers_buffsize);

    /* Upload everything.
     */
    R_PushCmd((struct rcmd){
        .func = R_GL_MoveUploadData,
        .nargs = 10,
        .args = {
            gpuid_buff,
            R_PushArg(&nactive, sizeof(nactive)),
            attrbuff,
            R_PushArg(&attr_buffsize, sizeof(attr_buffsize)),
            flockbuff,
            R_PushArg(&flock_buffsize, sizeof(flock_buffsize)),
            cost_base_buff,
            R_PushArg(&cost_base_buffsize, sizeof(cost_base_buffsize)),
            blockers_buff,
            R_PushArg(&blockers_buffsize, sizeof(blockers_buffsize)),
        },
    });

    PERF_RETURN_VOID();
}

static void move_update_uniforms(void)
{
    struct map_resolution res;
    M_GetResolution(s_move_work.gamestate.map, &res);
    vec3_t map_pos = M_GetPos(s_move_work.gamestate.map);
    vec2_t map_pos_xz = (vec2_t){map_pos.x, map_pos.z};
    int ticks = hz_count(s_move_work.hz);
    int nwork = s_move_work.nwork;

    R_PushCmd((struct rcmd){
        .func = R_GL_MoveUpdateUniforms,
        .nargs = 4,
        .args = {
            R_PushArg(&res, sizeof(res)),
            R_PushArg(&map_pos_xz, sizeof(map_pos_xz)),
            R_PushArg(&ticks, sizeof(ticks)),
            R_PushArg(&nwork, sizeof(nwork)),
        },
    });
}

static void move_submit_gpu_velocity_work(void)
{
    assert(Sched_ActiveTID() != NULL_TID);
    Task_RescheduleOnMain();

    size_t nents = G_Pos_UploadFrom(s_move_work.gamestate.positions,
        s_move_work.gamestate.ent_gpu_id_map,
        s_move_work.gamestate.map);
    assert(nents == kh_size(s_entity_state_table));

    move_upload_input(nents);
    move_update_uniforms();

    R_PushCmd((struct rcmd){
        .func = R_GL_MoveDispatchWork,
        .nargs = 1,
        .args = R_PushArg(&s_move_work.nwork, sizeof(s_move_work.nwork))
    });
    Task_Yield();
}

static void nav_tick_submit_work(void)
{
    ASSERT_IN_MAIN_THREAD();

    if(s_move_work.type == WORK_TYPE_GPU) {
        size_t nwork = s_move_work.nwork;
        size_t size = nwork * sizeof(vec2_t);
        s_move_work.gpu_velocities = stalloc(&s_move_work.mem, size);
    }

    SDL_AtomicSet(&s_tick_task_future.status, FUTURE_INCOMPLETE);
    s_tick_task_tid = Sched_Create(0, navigation_tick_task, NULL, 
            "navigation_tick_task", &s_tick_task_future, TASK_BIG_STACK);
    assert(s_tick_task_tid != NULL_TID);
    s_last_tick = g_frame_idx;
}

static enum move_work_status nav_tick_finish_work(void)
{
    if(s_tick_task_tid == NULL_TID) {
        return WORK_COMPLETE;
    }
    while(!Sched_FutureIsReady(&s_tick_task_future)) {
        /* If the task is event-blocked waiting for GPU results,
         * we are not able to run it to completion at this point.
         */
        if(!Sched_RunSync(s_tick_task_tid))
            return WORK_INCOMPLETE;
    }
    s_tick_task_tid = NULL_TID;
    return WORK_COMPLETE;
}

static enum movement_hz event_to_hz(enum eventtype event)
{
    static const enum movement_hz mapping[] = {
        [EVENT_20HZ_TICK] = MOVE_HZ_20,
        [EVENT_10HZ_TICK] = MOVE_HZ_10,
        [EVENT_5HZ_TICK] = MOVE_HZ_5,
        [EVENT_1HZ_TICK] = MOVE_HZ_1,
    };
    return mapping[event];
}

static enum eventtype event_for_hz(enum movement_hz hz)
{
    assert(hz >= 0 && hz <= MOVE_HZ_1);
    static const enum eventtype mapping[] = {
        [MOVE_HZ_20] = EVENT_20HZ_TICK,
        [MOVE_HZ_10] = EVENT_10HZ_TICK,
        [MOVE_HZ_5 ] = EVENT_5HZ_TICK,
        [MOVE_HZ_1 ] = EVENT_1HZ_TICK,
    };
    return mapping[hz];
}

static void register_callback_for_hz(enum movement_hz hz)
{
    enum eventtype event = event_for_hz(hz);
    E_Global_Register(event, move_tick, (void*)(uintptr_t)event, G_RUNNING);
}

static void unregister_callback_for_hz(enum movement_hz hz)
{
    assert(hz >= 0 && hz <= MOVE_HZ_1);
    enum eventtype event = event_for_hz(hz);
    E_Global_Unregister(event, move_tick);
}

static void move_handle_hz_update(enum eventtype curr)
{
    if(!s_move_hz_dirty)
        return;

    s_move_hz_dirty = false;

    enum eventtype next = event_for_hz(s_move_hz);

    if(curr == next)
        return;

    enum movement_hz curr_hz = event_to_hz(curr);
    enum movement_hz next_hz = s_move_hz;

    unregister_callback_for_hz(curr_hz);
    register_callback_for_hz(next_hz);
}

static void entity_interpolation_step(uint32_t uid, int steps)
{
    ASSERT_IN_MAIN_THREAD();
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    if(ms->left == 0)
        return;

    steps = MIN(steps, ms->left);
    ms->left -= steps;
    float fraction = 1.0 - (ms->step * ms->left);
    assert(fraction >= 0.0f && fraction <= 1.0f);

    vec3_t new_pos = interpolate_positions(ms->prev_pos, ms->next_pos, fraction);
    G_Pos_Set(uid, new_pos);
}

static void interpolate_tick(void *user, void *event)
{
    ASSERT_IN_MAIN_THREAD();

    /* Do not run the interpolation in the same tick as the move tick */
    if(g_frame_idx == s_last_tick)
        return;

    if(s_move_tick_queued)
        return;

    /* Perform a maximum of one interpolation per frame. */
    if(g_frame_idx == s_last_interpolate_tick)
        return;

    /* No need to perform the interpolation if we've got the next movement
     * tick coming right up.
     */
    enum eventtype type = event_for_hz(s_move_hz);
    if(E_QueuedThisFrame(type)) {
        s_last_interpolate_tick = g_frame_idx;
        return;
    }

    PERF_ENTER();
    bool coalese = E_QueuedThisFrame(EVENT_20HZ_TICK);

    /* Iterate over all the entities and advance the position forward
     * by one interpolated step */
    uint32_t key;
    kh_foreach_key(s_entity_state_table, key, {
        /* The entity has been removed already */
        if(!G_EntityExists(key))
            continue;

        /* Coalese together queued updates when possible */
        int steps = coalese ? 2 : 1;
        entity_interpolation_step(key, steps);
    });

    s_last_interpolate_tick = g_frame_idx;
    PERF_RETURN_VOID();
}

static void compute_async_fields(void)
{
    /* The field computations can read various navigation state
     * from different threads. This is okay so long as nothing
     * concurrently mutates it.
     */
    N_PrepareAsyncWork();

    for(int i = 0; i < s_move_work.nwork; i++) {
        struct move_work_in *in = &s_move_work.in[i];
        request_async_field(in->ent_uid);
        Sched_TryYield();
    }
    N_AwaitAsyncFields();
}

static void compute_desired_velocity(void)
{
    for(int i = 0; i < s_move_work.nwork; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        struct move_work_out *out = &s_move_work.out[i];

        PERF_PUSH("desired velocity");
        in->ent_des_v = ent_desired_velocity(in->ent_uid, in->cell_arrival_vdes);
        out->ent_des_v = in->ent_des_v;
        PERF_POP();

		Sched_TryYield();
    }
}

static void fork_join_velocity_computations(void)
{
    switch(s_move_work.type) {
    case WORK_TYPE_CPU:
        move_submit_cpu_work(move_velocity_task);
        move_complete_cpu_work();
        break;
    case WORK_TYPE_GPU:
        move_submit_gpu_velocity_work();
        break;
    default: assert(0);
    }
}

static void fork_join_state_updates(void)
{
    PERF_PUSH("move::submit state updates");
    move_submit_cpu_work(move_update_task);
    PERF_POP();

    Sched_TryYield();

    PERF_PUSH("move::complete state updates");
    move_complete_cpu_work();
    PERF_POP();

    PERF_RETURN_VOID();
}

static void await_gpu_completion(uint32_t timeout_ms)
{
    uint32_t begin = SDL_GetTicks();
    while(!SDL_AtomicGet(&s_move_work.gpu_velocities_ready)) {

        Task_RescheduleOnMain();
        R_PushCmd((struct rcmd){
            .func = R_GL_MovePollCompletion,
            .nargs = 1,
            .args = {
                [0] = &s_move_work.gpu_velocities_ready
            }
        });

        int source;
        Task_AwaitEvent(EVENT_UPDATE_START, &source);

        uint32_t now = SDL_GetTicks();
        if(SDL_TICKS_PASSED(now, begin + timeout_ms))
            break;
    }
}

static void await_gpu_download(void)
{
    /* We need to wait for 2 frames after the download command 
     * is queued. In one tick, it will be executed by the render
     * thread. In 2 ticks, it is guaranteed to have completed.
     */
    unsigned long start_frame = g_frame_idx;
    while((g_frame_idx - start_frame) < 2) {
        int source;
        Task_AwaitEvent(EVENT_UPDATE_START, &source);
    }
}

static void copy_gpu_results(void)
{
    PERF_ENTER();
    size_t nents = kh_size(s_entity_state_table);
    for(int i = 0; i < s_move_work.nwork; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        struct move_work_out *out = &s_move_work.out[i];

        out->ent_uid = in->ent_uid;
        out->ent_vel = s_move_work.gpu_velocities[i];
    }
    PERF_RETURN_VOID();
}

static struct result navigation_tick_task(void *arg)
{
    compute_async_fields();
    compute_desired_velocity();
    fork_join_velocity_computations();

    if(s_move_work.type == WORK_TYPE_GPU) {

        uint32_t period_ms = (1.0f / hz_count(s_move_work.hz)) * 1000;
        await_gpu_completion(period_ms);
        move_complete_gpu_velocity_work();

        await_gpu_download();
        copy_gpu_results();
    }

    fork_join_state_updates();
    return NULL_RESULT;
}

static void move_do_tick(enum eventtype curr_event, enum movement_hz hz)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_PUSH("movement::tick");

    move_consume_work_results();
    move_handle_hz_update(curr_event);
    move_process_cmds();
    G_SwapFieldCaches(s_move_work.gamestate.map);
    move_release_gamestate();
    disband_empty_flocks();

    /* Run the navigation updates synchronous to the movement tick */
    G_UpdateMap();

    move_prepare_work(hz);
    move_copy_gamestate();

    PERF_PUSH("submit move work");
    uint32_t curr;
    kh_foreach_key(s_entity_state_table, curr, {

        struct movestate *ms = movestate_get(curr);
        assert(ms);

        if(ent_still(ms))
            continue;

        struct flock *flock = flock_for_ent(curr);
        vec_cp_ent_t *dyn, *stat;
        dyn = stalloc(&s_move_work.mem, sizeof(vec_cp_ent_t));
        stat = stalloc(&s_move_work.mem, sizeof(vec_cp_ent_t));

        vec_cp_ent_init_alloc(dyn, cp_vec_realloc, cp_vec_free);
        vec_cp_ent_init_alloc(stat, cp_vec_realloc, cp_vec_free);

        vec_cp_ent_resize(dyn, MAX_NEIGHBOURS);
        vec_cp_ent_resize(stat, MAX_NEIGHBOURS);

        vec2_t pos = (vec2_t){ms->prev_pos.x, ms->prev_pos.z};
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);

        struct cp_ent curr_cp = (struct cp_ent) {
            .xz_pos = pos,
            .xz_vel = ms->velocity,
            .radius = radius
        };

        vec2_t cell_pos = (vec2_t){0.0f, 0.0f};
        vec2_t cell_arrival_vdes = {0};
        if(ms->state == STATE_ARRIVING_TO_CELL) {
            cell_pos = G_Formation_CellPosition(curr);
            if(!G_Formation_CanUseArrivalField(curr)) {
                cell_arrival_vdes = G_Formation_ApproximateDesiredArrivalVelocity(curr);
            }else{
                G_Formation_UpdateFieldIfNeeded(curr);
                cell_arrival_vdes = G_Formation_DesiredArrivalVelocity(curr);
            }
        }

        formation_id_t fid = G_Formation_GetForEnt(curr);
        move_push_work((struct move_work_in){
            .ent_uid = curr,
            .speed = entity_speed(curr),
            .cell_pos = cell_pos,
            .cp_ent = curr_cp,
            .save_debug = G_ClearPath_ShouldSaveDebug(curr),
            .stat_neighbs = stat,
            .dyn_neighbs = dyn,
            .has_dest_los = (flock 
                         && (ms->state != STATE_SURROUND_ENTITY || !ms->using_surround_field)) 
                          ? M_NavHasDestLOS(s_map, flock->dest_id, pos) : false,
            .fstate.fid = fid,
            .fstate.assignment_ready = 
                (fid != NULL_FID) ? G_Formation_AssignmentReady(curr)
                                  : false,
            .fstate.assigned_to_cell = 
                (fid != NULL_FID) ? G_Formation_AssignedToCell(curr)
                                  : false,
            .fstate.in_range_of_cell = 
                (fid != NULL_FID) ? G_Formation_InRangeOfCell(curr)
                                  : false,
            .fstate.arrived_at_cell = 
                (fid != NULL_FID) ? G_Formation_ArrivedAtCell(curr)
                                  : false,
            .fstate.normal_cohesion_force = 
                ((fid != NULL_FID) ? G_Formation_CohesionForce(curr) 
                                   : (vec2_t){0.0f, 0.0f}),
            .fstate.normal_align_force = 
                ((fid != NULL_FID) ? G_Formation_AlignmentForce(curr)
                                   : (vec2_t){0.0f, 0.0f}),
            .fstate.normal_drag_force = 
                ((fid != NULL_FID) ? G_Formation_DragForce(curr)
                                   : (vec2_t){0.0f, 0.0f}),
            .fstate.target_orientation = 
                ((fid != NULL_FID) ? G_Formation_TargetOrientation(curr)
                                   : (quat_t){0.0f, 0.0f, 0.0f, 0.0f}),
            .cell_arrival_vdes = cell_arrival_vdes
        });
    });
    PERF_POP();

    nav_tick_submit_work();
    PERF_POP();
}

static void move_tick(void *user, void *event)
{
    /* If we are backed up, drop excess events */
    if(g_frame_idx == s_last_tick)
        return;

    enum eventtype curr_event = (uintptr_t)user;
    enum movement_hz hz = event_to_hz(curr_event);

    enum move_work_status status = nav_tick_finish_work();
    if(status == WORK_INCOMPLETE) {
        s_move_tick_queued = true;
        return;
    }

    s_move_tick_queued = false;
    move_do_tick(curr_event, hz);
}

static void handle_queued_tick(void)
{
    if(!s_move_tick_queued)
        return;

    enum move_work_status status = nav_tick_finish_work();
    if(status == WORK_INCOMPLETE)
        return;

    enum movement_hz hz = s_move_work.hz;
    enum eventtype curr_event = event_for_hz(hz);

    s_move_tick_queued = false;
    move_do_tick(curr_event, hz);
}

static void on_update(void *user, void *event)
{
    stalloc_clear(&s_eventargs);
    handle_queued_tick();
}

static void nav_cancel_gpu_work(void)
{
    /* Handle the case where the work task is blocked on an event. We 
     * cannot run it yet, as the new velocity data from the GPU won't 
     * be available until the next frame. Kill off the task.
     */
    assert(s_tick_task_tid != NULL_TID);
    Sched_TryCancel(s_tick_task_tid);
    s_tick_task_tid = NULL_TID;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Move_Init(const struct map *map)
{
    assert(map);
    if(NULL == (s_entity_state_table = kh_init(state))) {
        return false;
    }

    memset(&s_move_work, 0, sizeof(s_move_work));
    if(!stalloc_init(&s_move_work.mem)) {
        kh_destroy(state, s_entity_state_table);
        return NULL;
    }

    if(!queue_cmd_init(&s_move_commands, 256)) {
        stalloc_destroy(&s_move_work.mem);
        kh_destroy(state, s_entity_state_table);
        return NULL;
    }

    if(!stalloc_init(&s_eventargs)) {
        stalloc_destroy(&s_move_work.mem);
        kh_destroy(state, s_entity_state_table);
        queue_cmd_destroy(&s_move_commands);
        return NULL;
    }

    vec_entity_init(&s_move_markers);
    vec_flock_init(&s_flocks);

    E_Global_Register(EVENT_UPDATE_START, on_update, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONUP, on_mouseup, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEMOTION, on_mousemotion, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    register_callback_for_hz(s_move_hz);
    E_Global_Register(EVENT_20HZ_TICK, interpolate_tick, NULL, G_RUNNING);

    s_map = map;
    s_attack_on_lclick = false;
    s_move_on_lclick = false;
    s_mouse_dragged = false;
    s_drag_attacking = false;
    move_copy_gamestate();
    return true;
}

void G_Move_Shutdown(void)
{
    if(nav_tick_finish_work() == WORK_INCOMPLETE) {
        nav_cancel_gpu_work();
    }
    s_move_tick_queued = false;
    s_map = NULL;

    unregister_callback_for_hz(s_move_hz);
    E_Global_Unregister(EVENT_20HZ_TICK, interpolate_tick);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(SDL_MOUSEBUTTONUP, on_mouseup);
    E_Global_Unregister(SDL_MOUSEMOTION, on_mousemotion);
    E_Global_Unregister(EVENT_UPDATE_START, on_update);

    for(int i = 0; i < vec_size(&s_move_markers); i++) {
        E_Entity_Unregister(EVENT_ANIM_FINISHED, vec_AT(&s_move_markers, i), on_marker_anim_finish);
        G_RemoveEntity(vec_AT(&s_move_markers, i));
        G_FreeEntity(vec_AT(&s_move_markers, i));
    }

    move_release_gamestate();
    vec_flock_destroy(&s_flocks);
    vec_entity_destroy(&s_move_markers);
    stalloc_destroy(&s_eventargs);
    queue_cmd_destroy(&s_move_commands);
    stalloc_destroy(&s_move_work.mem);
    kh_destroy(state, s_entity_state_table);
}

bool G_Move_HasWork(void)
{
    return (queue_size(s_move_commands) > 0);
}

void G_Move_FlushWork(void)
{
    /* Discard the results of the last 
     * movement tick. 
     */
    if(nav_tick_finish_work() == WORK_INCOMPLETE) {
        nav_cancel_gpu_work();
    }

    stalloc_clear(&s_move_work.mem);
    s_move_work.in = NULL;
    s_move_work.out = NULL;
    s_move_work.nwork = 0;
    s_move_work.ntasks = 0;

    move_process_cmds();
}

void G_Move_AddEntity(uint32_t uid, vec3_t pos, float sel_radius, int faction_id)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_ADD,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = pos
        },
        .args[2] = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = sel_radius
        },
        .args[3] = (struct attr){
            .type = TYPE_INT,
            .val.as_float = faction_id
        }
    });
}

void G_Move_RemoveEntity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_REMOVE,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

void G_Move_Stop(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_STOP,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

bool G_Move_GetDest(uint32_t uid, vec2_t *out_xz, bool *out_attack)
{
    struct move_cmd *cmd = snoop_most_recent_command(MOVE_CMD_SET_DEST,
        (void*)(uintptr_t)uid, uids_match, false);

    if(cmd) {
        *out_xz = cmd->args[1].val.as_vec2;
        *out_attack = cmd->args[2].val.as_bool;
        return true;
    }

    struct flock *fl = flock_for_ent(uid);
    if(!fl)
        return false;
    *out_xz = fl->target_xz;
    *out_attack = N_DestIDIsAttacking(fl->dest_id);
    return true;
}

bool G_Move_GetSurrounding(uint32_t uid, uint32_t *out_uid)
{
    struct move_cmd *cmd = snoop_most_recent_command(MOVE_CMD_SET_SURROUND_ENTITY,
        (void*)(uintptr_t)uid, uids_match, false);

    if(cmd) {
        *out_uid = cmd->args[1].val.as_int;
        return true;
    }

    struct movestate *ms = movestate_get(uid);
    assert(ms);
    if(ms->state != STATE_SURROUND_ENTITY)
        return false;
    *out_uid = ms->surround_target_uid;
    return true;
}

bool G_Move_Still(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return true;
    return snoop_still(uid);
}

void G_Move_SetDest(uint32_t uid, vec2_t dest_xz, bool attack)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_DEST,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = dest_xz
        },
        .args[2] = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = attack
        }
    });
}

void G_Move_SetChangeDirection(uint32_t uid, quat_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_CHANGE_DIRECTION,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = target
        }
    });
}

void G_Move_SetEnterRange(uint32_t uid, uint32_t target, float range)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_ENTER_RANGE,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = target
        },
        .args[2] = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = range
        } 
    });
}

void G_Move_SetMoveOnLeftClick(void)
{
    s_attack_on_lclick = false;
    s_move_on_lclick = true;
}

void G_Move_SetAttackOnLeftClick(void)
{
    s_attack_on_lclick = true;
    s_move_on_lclick = false;
}

void G_Move_SetSeekEnemies(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SEEK_ENEMIES,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

void G_Move_SetSurroundEntity(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SURROUND_ENTITY,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = target
        }
    });
}

void G_Move_UpdatePos(uint32_t uid, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_POS,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = pos
        }
    });
}

void G_Move_Unblock(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UNBLOCK,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

void G_Move_BlockAt(uint32_t uid, vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_BLOCK,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = pos
        }
    });
}

void G_Move_UpdateFactionID(uint32_t uid, int oldfac, int newfac)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_FACTION_ID,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = oldfac
        },
        .args[2] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = newfac
        }
    });
}

void G_Move_UpdateSelectionRadius(uint32_t uid, float sel_radius)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_SELECTION_RADIUS,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float  = sel_radius
        }
    });
}

bool G_Move_InTargetMode(void)
{
    return (s_move_on_lclick || s_attack_on_lclick);
}

void G_Move_SetClickEnabled(bool on)
{
    s_click_move_enabled = on;
}

bool G_Move_GetClickEnabled(void)
{
    return s_click_move_enabled;
}

bool G_Move_GetMaxSpeed(uint32_t uid, float *out)
{
    struct move_cmd *cmd = snoop_most_recent_command(MOVE_CMD_SET_MAX_SPEED,
        (void*)(uintptr_t)uid, uids_match, false);

    if(cmd) {
        *out = cmd->args[1].val.as_float;
        return true;
    }

    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return false;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    *out = ms->max_speed;
    return true;
}

bool G_Move_SetMaxSpeed(uint32_t uid, float speed)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_MAX_SPEED,
        .args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = speed
        }
    });
    return true;
}

void G_Move_ArrangeInFormation(vec_entity_t *ents, vec2_t target, 
                               vec2_t orientation, enum formation_type type)
{
    ASSERT_IN_MAIN_THREAD();
    vec_entity_t *copy = malloc(sizeof(vec_entity_t));
    vec_entity_init(copy);
    vec_entity_copy(copy, ents);

    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_MAKE_FLOCKS,
        .args[0] = (struct attr){
            .type = TYPE_POINTER,
            .val.as_pointer = copy
        },
        .args[1] = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = target,
        },
        .args[2] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = type
        },
        .args[3] = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = false
        },
        .args[4] = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = orientation
        }
    });
}

void G_Move_AttackInFormation(vec_entity_t *ents, vec2_t target, 
                              vec2_t orientation, enum formation_type type)
{
    ASSERT_IN_MAIN_THREAD();
    vec_entity_t *copy = malloc(sizeof(vec_entity_t));
    vec_entity_init(copy);
    vec_entity_copy(copy, ents);

    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_MAKE_FLOCKS,
        .args[0] = (struct attr){
            .type = TYPE_POINTER,
            .val.as_pointer = copy
        },
        .args[1] = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = target,
        },
        .args[2] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = type
        },
        .args[3] = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = true
        },
        .args[4] = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = orientation
        }
    });
}

void G_Move_SetTickHz(enum movement_hz hz)
{
    s_move_hz_dirty = (s_move_hz != hz);
    s_move_hz = hz;
}

int G_Move_GetTickHz(void)
{
    return hz_count(s_move_hz);
}

void G_Move_SetUseGPU(bool use)
{
    s_use_gpu = use;
}

bool G_Move_SaveState(struct SDL_RWops *stream)
{
    struct attr click_move_enabled = (struct attr){
        .type = TYPE_BOOL,
        .val.as_bool = s_click_move_enabled
    };
    CHK_TRUE_RET(Attr_Write(stream, &click_move_enabled, "click_move_enabled"));

    /* save flock info */
    struct attr num_flocks = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&s_flocks)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_flocks, "num_flocks"));

    Sched_TryYield();

    for(int i = 0; i < vec_size(&s_flocks); i++) {

        const struct flock *curr_flock = &vec_AT(&s_flocks, i);

        struct attr num_flock_ents = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr_flock->ents)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_flock_ents, "num_flock_ents"));

        uint32_t uid;
        kh_foreach_key(curr_flock->ents, uid, {
        
            struct attr flock_ent = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            CHK_TRUE_RET(Attr_Write(stream, &flock_ent, "flock_ent"));
        });
        Sched_TryYield();

        struct attr flock_target = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr_flock->target_xz
        };
        CHK_TRUE_RET(Attr_Write(stream, &flock_target, "flock_target"));

        struct attr flock_dest = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr_flock->dest_id
        };
        CHK_TRUE_RET(Attr_Write(stream, &flock_dest, "flock_dest"));
        Sched_TryYield();
    }

    /* save the movement state */
    struct attr num_ents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_ents, "num_ents"));
    Sched_TryYield();

    uint32_t key;
    struct movestate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = key
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));

        struct attr state = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.state
        };
        CHK_TRUE_RET(Attr_Write(stream, &state, "state"));

        struct attr max_speed = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.max_speed
        };
        CHK_TRUE_RET(Attr_Write(stream, &max_speed, "max_speed"));

        struct attr velocity = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.velocity
        };
        CHK_TRUE_RET(Attr_Write(stream, &velocity, "velocity"));

        struct attr next_pos = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr.next_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &next_pos, "next_pos"));

        struct attr prev_pos = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr.prev_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &prev_pos, "prev_pos"));

        struct attr next_rot = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = curr.next_rot
        };
        CHK_TRUE_RET(Attr_Write(stream, &next_rot, "next_rot"));

        struct attr prev_rot = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = curr.prev_rot
        };
        CHK_TRUE_RET(Attr_Write(stream, &prev_rot, "prev_rot"));

        struct attr step = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.step
        };
        CHK_TRUE_RET(Attr_Write(stream, &step, "step"));

        struct attr left = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.left
        };
        CHK_TRUE_RET(Attr_Write(stream, &left, "left"));

        struct attr blocking = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.blocking
        };
        CHK_TRUE_RET(Attr_Write(stream, &blocking, "blocking"));

        /* last_stop_pos and last_stop_radius are loaded in 
         * along with the entity's position. No need to overwrite
         * it and risk some inconsistency */

        struct attr wait_prev = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.wait_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &wait_prev, "wait_prev"));

        struct attr wait_ticks_left = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.wait_ticks_left
        };
        CHK_TRUE_RET(Attr_Write(stream, &wait_ticks_left, "wait_ticks_left"));

        for(int i = 0; i < VEL_HIST_LEN; i++) {
        
            struct attr hist_entry = (struct attr){
                .type = TYPE_VEC2,
                .val.as_vec2 = curr.vel_hist[i]
            };
            CHK_TRUE_RET(Attr_Write(stream, &hist_entry, "hist_entry"));
        }

        struct attr vel_hist_idx = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.vel_hist_idx
        };
        CHK_TRUE_RET(Attr_Write(stream, &vel_hist_idx, "vel_hist_idx"));

        struct attr surround_target_uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.surround_target_uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &surround_target_uid, "surround_target_uid"));

        struct attr surround_target_prev = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.surround_target_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &surround_target_prev, "surround_target_prev"));

        struct attr surround_nearest_prev = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.surround_nearest_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &surround_nearest_prev, "surround_nearest_prev"));

        struct attr using_surround_field = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.using_surround_field
        };
        CHK_TRUE_RET(Attr_Write(stream, &using_surround_field, "using_surround_field"));

        struct attr target_prev_pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.target_prev_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_prev_pos, "target_prev_pos"));

        struct attr target_range = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.target_range
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_range, "target_range"));

        struct attr target_dir = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = curr.target_dir
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_dir, "target_dir"));
        Sched_TryYield();
    });

    return true;
}

bool G_Move_LoadState(struct SDL_RWops *stream)
{
    /* Flush the commands submitted during loading */
    move_update_gamestate();
    move_process_cmds();

    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    s_click_move_enabled = attr.val.as_bool;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_flocks = attr.val.as_int;
    Sched_TryYield();

    assert(vec_size(&s_flocks) == 0);
    for(int i = 0; i < num_flocks; i++) {

        struct flock new_flock;
        new_flock.ents = kh_init(entity);
        CHK_TRUE_RET(new_flock.ents);

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_flock);
        const int num_flock_ents = attr.val.as_int;

        for(int j = 0; j < num_flock_ents; j++) {

            CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
            CHK_TRUE_JMP(attr.type == TYPE_INT, fail_flock);

            uint32_t flock_ent_uid = attr.val.as_int;
            flock_add(&new_flock, flock_ent_uid);
        }

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
        CHK_TRUE_JMP(attr.type == TYPE_VEC2, fail_flock);
        new_flock.target_xz = attr.val.as_vec2;

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_flock);
        new_flock.dest_id = attr.val.as_int;

        vec_flock_push(&s_flocks, new_flock);
        Sched_TryYield();
        continue;

    fail_flock:
        kh_destroy(entity, new_flock.ents);
        return false;
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_ents = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_ents; i++) {

        uint32_t uid;
        struct movestate *ms;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uid = attr.val.as_int;

        /* The entity should have already been loaded by the scripting state */
        khiter_t k = kh_get(state, s_entity_state_table, uid);
        CHK_TRUE_RET(k != kh_end(s_entity_state_table));
        ms = &kh_value(s_entity_state_table, k);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->state = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ms->max_speed = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->velocity = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        ms->next_pos = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        ms->prev_pos = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        ms->next_rot = attr.val.as_quat;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        ms->prev_rot = attr.val.as_quat;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ms->step = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->left = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);

        const bool blocking = attr.val.as_bool;
        assert(ms->blocking);
        if(!blocking) {
            entity_unblock(uid);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->wait_prev = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->wait_ticks_left = attr.val.as_int;

        for(int i = 0; i < VEL_HIST_LEN; i++) {
        
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_VEC2);
            ms->vel_hist[i] = attr.val.as_vec2;
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->vel_hist_idx = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->surround_target_uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->surround_target_prev = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->surround_nearest_prev = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        ms->using_surround_field = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->target_prev_pos = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ms->target_range = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        ms->target_dir = attr.val.as_quat;

        Sched_TryYield();
    }

    return true;
}

