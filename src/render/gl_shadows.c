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

#include "public/render.h"
#include "render_private.h"
#include "gl_render.h"
#include "gl_state.h"
#include "gl_assert.h"
#include "gl_shader.h"
#include "gl_perf.h"
#include "gl_anim.h"
#include "../main.h"
#include "../pf_math.h"
#include "../config.h"
#include "../settings.h"
#include "../camera.h"
#include "../phys/public/collision.h"
#include "../game/public/game.h"

#include <GL/glew.h>
#include <assert.h>


#define LIGHT_EXTRA_HEIGHT    (300.0f)
#define LIGHT_VISIBILITY_ZOOM (75.0f)

struct shadow_gl_state{
    GLint viewport[4];
    GLint fb;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint         s_depth_map_FBO;
static GLuint         s_depth_map_tex;
static bool           s_depth_pass_active = false;
static struct shadow_gl_state s_saved;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void make_light_frustum(vec3_t light_pos, vec3_t cam_pos, vec3_t cam_dir, 
                               struct frustum *out, mat4x4_t *out_view_mat)
{
    float t = cam_pos.y / cam_dir.y;
    vec3_t cam_ray_ground_isec = (vec3_t){cam_pos.x - t * cam_dir.x, 0.0f, cam_pos.z - t * cam_dir.z};

    vec3_t light_dir = light_pos;
    PFM_Vec3_Normal(&light_dir, &light_dir);
    PFM_Vec3_Scale(&light_dir, -1.0f, &light_dir);

    vec3_t right = (vec3_t){-1.0f, 0.0f, 0.0f}, up;
    PFM_Vec3_Cross(&light_dir, &right, &up);

    t = fabs((cam_pos.y + LIGHT_EXTRA_HEIGHT)/ light_dir.y);
    vec3_t light_origin, delta;
    PFM_Vec3_Scale(&light_dir, -t, &delta);
    PFM_Vec3_Add(&cam_ray_ground_isec, &delta, &light_origin);

    vec3_t target;
    PFM_Vec3_Add(&light_origin, &light_dir, &target);

    /* Since, for shadow mapping, we treat our light source as a directional light, 
     * we only care about direction of the light rays, not the absolute position of 
     * the light source. Thus, we render the shadow map from a fixed height, looking 
     * at the position where the camera ray intersects the ground plane. 
     */
    mat4x4_t light_view;
    PFM_Mat4x4_MakeLookAt(&light_origin, &target, &up, &light_view);

    if(out) {
        C_MakeFrustum(light_origin, up, light_dir, 1.0f, M_PI/4.0f, 0.1f, CONFIG_SHADOW_DRAWDIST, out);
    }
    if(out_view_mat) {
        *out_view_mat = light_view;
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_InitShadows(void)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    glGenTextures(1, &s_depth_map_tex);
    glBindTexture(GL_TEXTURE_2D, s_depth_map_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32, 
                 CONFIG_SHADOW_MAP_RES, CONFIG_SHADOW_MAP_RES, 
                 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    /* Don't enable deptph comparisons as we will use a sampler2D and 
     * manually perform comparison and filtering in the shader.
     */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLint old;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old);

    glGenFramebuffers(1, &s_depth_map_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, s_depth_map_FBO);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, s_depth_map_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glBindFramebuffer(GL_FRAMEBUFFER, old);  
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DepthPassBegin(const vec3_t *light_pos, const vec3_t *cam_pos, const vec3_t *cam_dir)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    GL_PERF_PUSH_GROUP(0, "depth pass");

    assert(!s_depth_pass_active);
    s_depth_pass_active = true;

    glGetIntegerv(GL_VIEWPORT, s_saved.viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s_saved.fb);

    mat4x4_t light_proj;
    PFM_Mat4x4_MakeOrthographic(-CONFIG_SHADOW_FOV, CONFIG_SHADOW_FOV, 
        CONFIG_SHADOW_FOV, -CONFIG_SHADOW_FOV, 0.1f, CONFIG_SHADOW_DRAWDIST, &light_proj);

    mat4x4_t light_view;
    make_light_frustum(*light_pos, *cam_pos, *cam_dir, NULL, &light_view);

    mat4x4_t light_space_trans;
    PFM_Mat4x4_Mult4x4(&light_proj, &light_view, &light_space_trans);
    R_GL_SetLightSpaceTrans(&light_space_trans);

    glViewport(0, 0, CONFIG_SHADOW_MAP_RES, CONFIG_SHADOW_MAP_RES);
    glBindFramebuffer(GL_FRAMEBUFFER, s_depth_map_FBO);
    glClear(GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DepthPassEnd(void)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    assert(s_depth_pass_active);
    s_depth_pass_active = false;

    glViewport(s_saved.viewport[0], s_saved.viewport[1], s_saved.viewport[2], s_saved.viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, s_saved.fb);
    glCullFace(GL_BACK);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_RenderDepthMap(const void *render_private, mat4x4_t *model)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    assert(s_depth_pass_active);

    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *model
    });

    const struct render_private *priv = render_private;
    R_GL_Shader_InstallProg(priv->shader_prog_dp);
    R_GL_AnimBindPoseBuff();

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_ShadowMapBind(void)
{
    R_GL_StateSet(GL_U_SHADOW_MAP, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = SHADOW_MAP_TUNIT - GL_TEXTURE0
    });
    R_GL_StateInstall(GL_U_SHADOW_MAP, R_GL_Shader_GetCurrActive());
    glActiveTexture(SHADOW_MAP_TUNIT);
    glBindTexture(GL_TEXTURE_2D, s_depth_map_tex);
    GL_ASSERT_OK();
}

void R_GL_SetShadowsEnabled(const bool *on)
{
    R_GL_StateSet(GL_U_SHADOWS_ON, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = *on
    });
}

void R_LightVisibilityFrustum(const struct camera *cam, struct frustum *out)
{
    vec3_t pos = Camera_GetPos(cam);
    vec3_t dir = Camera_GetDir(cam);

    vec3_t delta = dir;
    PFM_Vec3_Scale(&delta, -1.0f, &delta);
    PFM_Vec3_Scale(&delta, LIGHT_VISIBILITY_ZOOM, &delta);

    vec3_t new_pos;
    PFM_Vec3_Add(&pos, &delta, &new_pos);

    struct camera *zoomed_out = Camera_New();
    Camera_SetPos(zoomed_out, new_pos);
    Camera_SetDir(zoomed_out, dir);
    Camera_MakeFrustum(zoomed_out, out);
    Camera_Free(zoomed_out);
}

