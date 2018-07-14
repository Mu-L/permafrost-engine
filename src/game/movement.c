/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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
 */

#include "movement.h"
#include "public/game.h"
#include "../config.h"
#include "../camera.h"
#include "../asset_load.h"
#include "../event.h"
#include "../entity.h"
#include "../script/public/script.h"
#include "../render/public/render.h"
#include "../map/public/map.h"
#include "../lib/public/kvec.h"
#include "../lib/public/khash.h"
#include "../anim/public/anim.h"

#include <assert.h>
#include <SDL.h>


KHASH_MAP_INIT_INT(entity, struct entity *)
KHASH_MAP_INIT_INT(velocity, vec2_t)

struct flock{
    khash_t(entity) *ents;
    vec2_t           target_xz;
};

/* For the purpose of movement simulation, all entities have the same mass,
 * meaning they are accelerate the same amount when applied equal forces. */
#define ENTITY_MASS (1.0f)
#define EPSILON     (1.0f/1024)
#define MAX_FORCE   (1.0f)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

kvec_t(struct entity*)  s_move_markers;
kvec_t(struct flock)    s_flocks;
khash_t(velocity)      *s_entity_velocity_table;
const struct map       *s_map;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool entities_equal(struct entity **a, struct entity **b)
{
    return (0 == memcmp(*a, *b, sizeof(struct entity)));
}

static void vec2_truncate(vec2_t *inout, float max_len)
{
    if(PFM_Vec2_Len(inout) > max_len) {

        PFM_Vec2_Normal(inout, inout);
        PFM_Vec2_Scale(inout, max_len, inout);
    }
}

static void on_marker_anim_finish(void *user, void *event)
{
    int idx;
    struct entity *ent = user;
    assert(ent);

    kv_indexof(struct entity*, s_move_markers, ent, entities_equal, idx);
    assert(idx != -1);
    kv_del(struct entity*, s_move_markers, idx);

    E_Entity_Unregister(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish);
    AL_EntityFree(ent);
}

static bool make_flock_from_selection(const pentity_kvec_t *sel, vec2_t target_xz)
{
    /* First remove the entities in the selection from any active flocks */
    for(int i = 0; i < kv_size(*sel); i++) {

        const struct entity *curr_ent = kv_A(*sel, i);
        if(curr_ent->flags & ENTITY_FLAG_STATIC || curr_ent->max_speed == 0.0f)
            continue;
        /* Remove any flocks which may have become empty. Iterate vector in backwards order 
         * so that we can delete while iterating, since the last element in the vector takes
         * the place of the deleted one. */
        for(int j = kv_size(s_flocks)-1; j >= 0; j--) {

            khiter_t k;
            struct flock *curr_flock = &kv_A(s_flocks, j);
            if((k = kh_get(entity, curr_flock->ents, curr_ent->uid)) != kh_end(curr_flock->ents))
                kh_del(entity, curr_flock->ents, k);

            if(kh_size(curr_flock->ents) == 0) {
                kh_destroy(entity, curr_flock->ents);
                kv_del(struct flock, s_flocks, j);
            }
        }
    }
    
    struct flock new_flock = (struct flock) {
        .ents = kh_init(entity),
        .target_xz = target_xz,
    };

    if(!new_flock.ents)
        return false;

    for(int i = 0; i < kv_size(*sel); i++) {

        int ret;
        const struct entity *curr_ent = kv_A(*sel, i);

        khiter_t k = kh_put(entity, new_flock.ents, curr_ent->uid, &ret);
        assert(ret != -1 && ret != 0);
        kh_value(new_flock.ents, k) = (struct entity*)curr_ent;

        /* When entities are moved from one flock to another, they keep their existing velocity. 
         * Otherwise, entities start out with a velocity of 0. */
        if(kh_get(velocity, s_entity_velocity_table, curr_ent->uid) == kh_end(s_entity_velocity_table)) {

            k = kh_put(velocity, s_entity_velocity_table, curr_ent->uid, &ret);
            assert(ret != -1 && ret != 0);
            kh_value(s_entity_velocity_table, k) = (vec2_t){0.0f};
        }
    }

    kv_push(struct flock, s_flocks, new_flock);
    return true;
}

static void move_marker_add(vec3_t pos)
{
    extern const char *g_basepath;
    char path[256];
    strcpy(path, g_basepath);
    strcat(path, "assets/models/arrow");

    struct entity *ent = AL_EntityFromPFObj(path, "arrow-green.pfobj", "__move_marker__");
    assert(ent);

    ent->pos = pos;
    ent->scale = (vec3_t){2.0f, 2.0f, 2.0f};
    E_Entity_Register(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish, ent);

    A_InitCtx(ent, "Converge", 48);
    A_SetActiveClip(ent, "Converge", ANIM_MODE_ONCE, 48);

    kv_push(struct entity*, s_move_markers, ent);
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    if(mouse_event->button != SDL_BUTTON_RIGHT)
        return;

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    vec3_t mouse_coord;
    if(!M_Raycast_IntersecCoordinate(&mouse_coord))
        return;

    const pentity_kvec_t *sel = G_Sel_Get();
    if(kv_size(*sel) > 0) {

        move_marker_add(mouse_coord);
        make_flock_from_selection(sel, (vec2_t){mouse_coord.x, mouse_coord.z});
    }
}

static void on_render_3d(void *user, void *event)
{
    for(int i = 0; i < kv_size(s_move_markers); i++) {

        const struct entity *curr = kv_A(s_move_markers, i);
        if(curr->flags & ENTITY_FLAG_ANIMATED)
            A_Update(curr);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);
        R_GL_Draw(curr->render_private, &model);
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

static vec2_t seek_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = (vec2_t){ent->pos.x, ent->pos.z};

    PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &pos_xz, &desired_velocity);
    PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
    PFM_Vec2_Scale(&desired_velocity, ent->max_speed / tick_res, &desired_velocity);

    khiter_t k = kh_get(velocity, s_entity_velocity_table, ent->uid);
    assert(k != kh_end(s_entity_velocity_table));
    PFM_Vec2_Sub(&desired_velocity, &kh_value(s_entity_velocity_table, k), &ret);

    return ret;
}

static vec2_t arrive_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    const float SLOWING_RADIUS = 10.0f;

    vec2_t ret, desired_velocity;
    vec2_t pos_xz = (vec2_t){ent->pos.x, ent->pos.z};
    float distance;

    PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &pos_xz, &desired_velocity);
    distance = PFM_Vec2_Len(&desired_velocity);

    PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
    PFM_Vec2_Scale(&desired_velocity, ent->max_speed / tick_res, &desired_velocity);

    if(distance < SLOWING_RADIUS) {
        PFM_Vec2_Scale(&desired_velocity, distance / SLOWING_RADIUS, &desired_velocity);
    }

    khiter_t k = kh_get(velocity, s_entity_velocity_table, ent->uid);
    assert(k != kh_end(s_entity_velocity_table));
    PFM_Vec2_Sub(&desired_velocity, &kh_value(s_entity_velocity_table, k), &ret);

    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Alignment is a behavior that causes a particular agent to line up with agents close by.
 */
static vec2_t alignment_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    const float NEIGHBOUR_RADIUS = ent->selection_radius * 3.0f;

    vec2_t ret = (vec2_t){0.0f};
    size_t neighbour_count = 0;

    uint32_t key;
    struct entity *curr;
    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
        vec2_t curr_xz_pos = (vec2_t){curr->pos.x, curr->pos.z};

        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);
        if(PFM_Vec2_Len(&diff) < NEIGHBOUR_RADIUS) {

            khiter_t k = kh_get(velocity, s_entity_velocity_table, curr->uid);
            assert(kh_exist(s_entity_velocity_table, k));

            if(PFM_Vec2_Len(&kh_value(s_entity_velocity_table, curr->uid)) < EPSILON)
                continue; 

            vec2_t velocity = kh_value(s_entity_velocity_table, k);
            PFM_Vec2_Add(&ret, &velocity, &ret);
            neighbour_count++;
        }
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    khiter_t k = kh_get(velocity, s_entity_velocity_table, ent->uid);
    assert(kh_exist(s_entity_velocity_table, k));
    vec2_t velocity = kh_value(s_entity_velocity_table, k);

    PFM_Vec2_Scale(&ret, 1.0f / neighbour_count, &ret);
    PFM_Vec2_Sub(&ret, &velocity, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Cohesion is a behavior that causes agents to steer towards the center of mass of nearby agents.
 */
static vec2_t cohesion_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    const float NEIGHBOUR_RADIUS = 50.0f;

    vec2_t COM = (vec2_t){0.0f};
    size_t neighbour_count = 0;

    uint32_t key;
    struct entity *curr;
    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
        vec2_t curr_xz_pos = (vec2_t){curr->pos.x, curr->pos.z};

        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);
        if(PFM_Vec2_Len(&diff) < NEIGHBOUR_RADIUS) {

            vec2_t xz_pos = (vec2_t){curr->pos.x, curr->pos.z};
            PFM_Vec2_Add(&COM, &xz_pos, &COM);
            neighbour_count++;
        }
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    vec2_t xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
    PFM_Vec2_Scale(&COM, 1.0f / neighbour_count, &COM);

    vec2_t ret;
    PFM_Vec2_Sub(&COM, &xz_pos, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Separation is a behavior that causes agents to steer away from nearby agents.
 */
static vec2_t separation_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    const float NEIGHBOUR_RADIUS = ent->selection_radius * 3.0f;

    vec2_t ret = (vec2_t){0.0f};
    size_t neighbour_count = 0;

    uint32_t key;
    struct entity *curr;
    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
        vec2_t curr_xz_pos = (vec2_t){curr->pos.x, curr->pos.z};

        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);
        if(PFM_Vec2_Len(&diff) < NEIGHBOUR_RADIUS) {

            float frac = 1.0f - (PFM_Vec2_Len(&diff) / NEIGHBOUR_RADIUS);
            PFM_Vec2_Scale(&diff, frac, &diff);
            PFM_Vec2_Add(&ret, &diff, &ret);
            neighbour_count++;
        }
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    PFM_Vec2_Scale(&ret, -1.0f / neighbour_count, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

static vec2_t total_steering_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    vec2_t separation = separation_force(ent, flock, tick_res);
    PFM_Vec2_Scale(&separation, 1.0f, &separation);

    vec2_t arrive = arrive_force(ent, flock, tick_res);
    PFM_Vec2_Scale(&arrive, 0.6f, &arrive);

    vec2_t cohesion = cohesion_force(ent, flock, tick_res);
    PFM_Vec2_Scale(&cohesion, 0.1f, &cohesion);

    vec2_t alignment = alignment_force(ent, flock, tick_res);
    PFM_Vec2_Scale(&alignment, 0.1f, &alignment);

    vec2_t ret = (vec2_t){0.0f};
    PFM_Vec2_Add(&ret, &separation, &ret);
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &cohesion, &ret);
    PFM_Vec2_Add(&ret, &alignment, &ret);

    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

static void on_30hz_tick(void *user, void *event)
{
    const int TICK_RES = 30;

    for(int i = 0; i < kv_size(s_flocks); i++) {

        uint32_t key;
        struct entity *curr;
        kh_foreach(kv_A(s_flocks, i).ents, key, curr, {
            curr->max_speed = 20.0f; //TODO temp
        
            vec2_t steer_accel, new_velocity; 
            vec2_t steer_force = total_steering_force(curr, &kv_A(s_flocks, i), TICK_RES);
            PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &steer_accel);

            khiter_t k = kh_get(velocity, s_entity_velocity_table, curr->uid);
            assert(kh_exist(s_entity_velocity_table, k));
            vec2_t old_velocity = kh_value(s_entity_velocity_table, k); 

            PFM_Vec2_Add(&old_velocity, &steer_accel, &new_velocity);
            vec2_truncate(&new_velocity, curr->max_speed / TICK_RES);
            kh_value(s_entity_velocity_table, k) = new_velocity;

            vec2_t xz_pos = (vec2_t){curr->pos.x, curr->pos.z};
            vec2_t new_xz_pos;
            PFM_Vec2_Add(&xz_pos, &new_velocity, &new_xz_pos);
            new_xz_pos = M_ClampedMapCoordinate(s_map, new_xz_pos);
            curr->pos = (vec3_t){new_xz_pos.raw[0], M_HeightAtPoint(s_map, new_xz_pos), new_xz_pos.raw[1]};

            if(PFM_Vec2_Len(&new_velocity) > EPSILON) {
                curr->rotation = dir_quat_from_velocity(new_velocity);
            }
        });
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Move_Init(const struct map *map)
{
    assert(map);
    if(NULL == (s_entity_velocity_table = kh_init(velocity)))
        return false;
    kv_init(s_move_markers);
    kv_init(s_flocks);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL);
    E_Global_Register(EVENT_RENDER_3D, on_render_3d, NULL);
    E_Global_Register(EVENT_30HZ_TICK, on_30hz_tick, NULL);

    s_map = map;
    return true;
}

void G_Move_Shutdown(void)
{
    s_map = NULL;

    E_Global_Unregister(EVENT_30HZ_TICK, on_30hz_tick);
    E_Global_Unregister(EVENT_RENDER_3D, on_render_3d);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);

    for(int i = 0; i < kv_size(s_move_markers); i++) {
        E_Entity_Unregister(EVENT_ANIM_FINISHED, kv_A(s_move_markers, i)->uid, on_marker_anim_finish);
        AL_EntityFree(kv_A(s_move_markers, i));
    }
    kv_destroy(s_move_markers);
    kv_destroy(s_flocks);
    kh_destroy(velocity, s_entity_velocity_table);
}
