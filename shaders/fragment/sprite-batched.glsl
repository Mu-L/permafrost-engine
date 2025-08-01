/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2025 Eduard Permyakov 
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

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag{
         vec2 uv;
    flat uint frame;
}from_vertex;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform sampler2D sprite_sheet;
uniform int sprite_nrows;
uniform int sprite_ncols;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

vec4 sprite_texture(uint frame_idx, vec2 uv)
{
    int row = int(frame_idx) / sprite_ncols;
    row = (sprite_nrows-1) - row;
    int col = int(frame_idx) % sprite_ncols;

    int row_height = (textureSize(sprite_sheet, 0).y / sprite_nrows);
    int col_width = (textureSize(sprite_sheet, 0).x  / sprite_ncols);

    int u_offset = col * col_width;
    int v_offset = row * row_height;

    /* Normalize the new coordinate to the [0, 1] range */
    float u_coord = u_offset + (uv.x * col_width);
    float v_coord = v_offset + (uv.y * row_height);
    return texture(sprite_sheet, vec2(u_coord / textureSize(sprite_sheet, 0).x,
                                      v_coord / textureSize(sprite_sheet, 0).y));
}

void main()
{
    vec4 color = sprite_texture(from_vertex.frame, from_vertex.uv);
    if(color.a <= 0.5)
        discard;
    o_frag_color = color;
}

