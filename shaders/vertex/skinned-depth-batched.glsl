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

#version 330 core

#define MAX_JOINTS 96

layout (location = 0) in vec3  in_pos;
layout (location = 4) in ivec3 in_joint_indices0;
layout (location = 5) in ivec3 in_joint_indices1;
layout (location = 6) in vec3  in_joint_weights0;
layout (location = 7) in vec3  in_joint_weights1;
layout (location = 8) in int   in_draw_id;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform mat4 light_space_transform;
uniform vec4 clip_plane0;

/* The per-instance static attributes have the follwing layout in the buffer:
 *
 *  +--------------------------------------------------+ <-- base
 *  | mat4x4_t (16 floats)                             | (model matrix)
 *  +--------------------------------------------------+
 *  | vec2_t[16] (32 floats)                           | (material:texture mapping)
 *  +--------------------------------------------------+
 *  | {float, float, vec3_t, vec3_t}[16] (128 floats)  | (material properties)
 *  +--------------------------------------------------+
 *  | mat4x4_t (16 floats)                             | (normal matrix)
 *  +--------------------------------------------------+
 *  | int (1 float)                                    | (inv. bind pose offset)
 *  +--------------------------------------------------+
 *  | int (1 float)                                    | (curr. pose offset)
 *  +--------------------------------------------------+
 *
 * In total, 194 floats (776 bytes) are pushed per instance.
 */

uniform samplerBuffer attrbuff;
uniform samplerBuffer posebuff;
uniform int attrbuff_offset;
uniform int attr_stride;
uniform int attr_offset;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

int inst_attr_base(int draw_id)
{
    int size = textureSize(attrbuff);
    int inst_offset = (attr_offset > 0) ? (attr_offset + gl_InstanceID) * attr_stride 
                                        : draw_id * attr_stride;
    return (attrbuff_offset / 4 + inst_offset) % size;
}

vec4 attr_read_vec4(int base)
{
    int size = textureSize(attrbuff);
    return vec4(
        texelFetch(attrbuff, (base + 0) % size).r,
        texelFetch(attrbuff, (base + 1) % size).r,
        texelFetch(attrbuff, (base + 2) % size).r,
        texelFetch(attrbuff, (base + 3) % size).r
    );
}

mat4 attr_read_mat4(int base)
{
    return mat4(
        attr_read_vec4(base +  0),
        attr_read_vec4(base +  4),
        attr_read_vec4(base +  8),
        attr_read_vec4(base + 12)
    );
}

int attr_read_index(int base)
{
    int size = textureSize(attrbuff);
	return int(texelFetch(attrbuff, base % size).r);
}

vec4 pose_read_vec4(int base)
{
    int size = textureSize(posebuff);
    return vec4(
        texelFetch(posebuff, (base + 0) % size).r,
        texelFetch(posebuff, (base + 1) % size).r,
        texelFetch(posebuff, (base + 2) % size).r,
        texelFetch(posebuff, (base + 3) % size).r
    );
}

mat4 pose_read_mat4(int base)
{
    return mat4(
        pose_read_vec4(base +  0),
        pose_read_vec4(base +  4),
        pose_read_vec4(base +  8),
        pose_read_vec4(base + 12)
    );
}

mat4 anim_curr_pose_mats(int joint_idx)
{
    int base = inst_attr_base(in_draw_id) + 176 + 16 + 1;
	int pose_offset = attr_read_index(base);
    return pose_read_mat4((pose_offset / 4) + (16 * joint_idx));
}

mat4 anim_inv_bind_mats(int joint_idx)
{
    int base = inst_attr_base(in_draw_id) + 176 + 16;
	int pose_offset = attr_read_index(base);
    return pose_read_mat4((pose_offset / 4) + (16 * joint_idx));
}

void main()
{
    int base = inst_attr_base(in_draw_id);
    mat4 model = attr_read_mat4(base);

    float tot_weight = in_joint_weights0[0] + in_joint_weights0[1] + in_joint_weights0[2]
                     + in_joint_weights1[0] + in_joint_weights1[1] + in_joint_weights1[2];

    /* If all weights are 0, treat this vertex as a static one.
     * Non-animated vertices will have their weights explicitly zeroed out. 
     */
    if(tot_weight == 0.0) {

        gl_Position = light_space_transform * model * vec4(in_pos, 1.0);
        gl_ClipDistance[0] = dot(model * gl_Position, clip_plane0);

    }else {

        vec3 new_pos =  vec3(0.0, 0.0, 0.0);
        vec3 new_normal = vec3(0.0, 0.0, 0.0);

        for(int w_idx = 0; w_idx < 6; w_idx++) {

            int joint_idx = int(w_idx < 3 ? in_joint_indices0[w_idx % 3]
                                          : in_joint_indices1[w_idx % 3]);

            mat4 inv_bind_mat = anim_inv_bind_mats (joint_idx);
            mat4 pose_mat     = anim_curr_pose_mats(joint_idx);

            float weight = w_idx < 3 ? in_joint_weights0[w_idx % 3]
                                     : in_joint_weights1[w_idx % 3];
            float fraction = weight / tot_weight;

            mat4 bone_mat = fraction * pose_mat * inv_bind_mat;
            mat3 rot_mat = fraction * mat3(transpose(inverse(pose_mat * inv_bind_mat)));
            
            new_pos += (bone_mat * vec4(in_pos, 1.0)).xyz;
        }

        gl_Position = light_space_transform * model * vec4(new_pos, 1.0f);
        gl_ClipDistance[0] = dot(model * gl_Position, clip_plane0);
    }
}

