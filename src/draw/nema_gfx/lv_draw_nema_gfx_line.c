/**
 * MIT License
 *
 * -----------------------------------------------------------------------------
 * Copyright (c) 2008-24 Think Silicon Single Member PC
 * -----------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next paragraph)
 * shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/**
 * @file lv_draw_nema_gfx_line.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_draw_nema_gfx.h"

#if LV_USE_NEMA_GFX

/*MEASUREMENT PROTOTYPE (oneplatform, trend-pen-style/01-misura.md), not for a PR.
 *With LV_NEMA_GFX_LINE_BATCHING the line task binds the destination and sets the blend once, and submits once:
 *each piece sets only its clip and draws. Without it each piece does everything, as before.
 *Round caps are drawn with nema_fill_circle_aa, and a piece with p1 == p2 and round caps is a dot*/
#ifndef LV_NEMA_GFX_LINE_BATCHING
    #define LV_NEMA_GFX_LINE_BATCHING 0
#endif

/**********************
 *   STATIC FUNCTIONS
 **********************/

static bool has_round_caps(const lv_draw_line_dsc_t * dsc)
{
    return dsc->round_start || dsc->round_end;
}

/*The clip of a piece in the coordinates of the layer buffer: false when nothing of it is drawn*/
static bool get_piece_clip(lv_draw_task_t * t, const lv_draw_line_dsc_t * dsc, lv_area_t * clip_area)
{
    if(dsc->width == 0)
        return false;
    if(dsc->opa <= (lv_opa_t)LV_OPA_MIN)
        return false;
    if(dsc->p1.x == dsc->p2.x && dsc->p1.y == dsc->p2.y && !has_round_caps(dsc))
        return false;

    lv_layer_t * layer = t->target_layer;
    /*The antialiased edge of the line reaches up to 1 px past its width*/
    clip_area->x1 = LV_MIN(dsc->p1.x, dsc->p2.x) - dsc->width / 2 - 1;
    clip_area->x2 = LV_MAX(dsc->p1.x, dsc->p2.x) + dsc->width / 2 + 1;
    clip_area->y1 = LV_MIN(dsc->p1.y, dsc->p2.y) - dsc->width / 2 - 1;
    clip_area->y2 = LV_MAX(dsc->p1.y, dsc->p2.y) + dsc->width / 2 + 1;

    if(!lv_area_intersect(clip_area, clip_area, &t->clip_area))
        return false; /*Fully clipped, nothing to do*/

    lv_area_move(clip_area, -layer->buf_area.x1, -layer->buf_area.y1);
    return true;
}

static void bind_destination(lv_draw_task_t * t, const lv_draw_line_dsc_t * dsc)
{
    lv_layer_t * layer = t->target_layer;
    lv_color_format_t dst_cf = layer->draw_buf->header.cf;
    uint32_t dst_nema_cf = lv_nemagfx_cf_to_nema(dst_cf);

    /* the stride should be computed internally for NEMA_TSC images and images missing a stride value */
    int32_t stride = (dst_cf >= LV_COLOR_FORMAT_NEMA_TSC_START && dst_cf <= LV_COLOR_FORMAT_NEMA_TSC_END) ?
                     -1 : lv_area_get_width(&(layer->buf_area)) * lv_color_format_get_size(dst_cf);

    nema_bind_dst_tex((uintptr_t)NEMA_VIRT2PHYS(layer->draw_buf->data), lv_area_get_width(&(layer->buf_area)),
                      lv_area_get_height(&(layer->buf_area)), dst_nema_cf, stride);

    lv_color32_t col32 = lv_color_to_32(dsc->color, dsc->opa);
    if(col32.alpha < 255U) {
        nema_set_blend_fill(NEMA_BL_SIMPLE);
    }
    else {
        nema_set_blend_fill(NEMA_BL_SRC);
    }
}

/*The segment and its round caps, inside the clip already set*/
static void draw_piece(lv_draw_task_t * t, const lv_draw_line_dsc_t * dsc)
{
    lv_layer_t * layer = t->target_layer;
    lv_point_t point1 = {dsc->p1.x - layer->buf_area.x1, dsc->p1.y - layer->buf_area.y1};
    lv_point_t point2 = {dsc->p2.x - layer->buf_area.x1, dsc->p2.y - layer->buf_area.y1};

    lv_color32_t col32 = lv_color_to_32(dsc->color, dsc->opa);
    uint32_t bg_color = nema_rgba(col32.red, col32.green, col32.blue, col32.alpha);

    /*Odd widths centered on the middle of the pixel, as the software renderer draws them*/
    float offset = (dsc->width & 1) ? 0.5f : 0.0f;
    bool is_dot = point1.x == point2.x && point1.y == point2.y;
    if(!is_dot) {
        nema_draw_line_aa(point1.x + offset, point1.y + offset, point2.x + offset, point2.y + offset, dsc->width,
                          bg_color);
    }

    float radius = (float)dsc->width / 2.0f;
    if(dsc->round_start || (is_dot && dsc->round_end)) {
        nema_fill_circle_aa(point1.x + offset, point1.y + offset, radius, bg_color);
    }
    if(dsc->round_end && !is_dot) {
        nema_fill_circle_aa(point2.x + offset, point2.y + offset, radius, bg_color);
    }
}

#if LV_NEMA_GFX_LINE_BATCHING
static void draw_batched_piece(lv_draw_task_t * t, const lv_draw_line_dsc_t * dsc)
{
    lv_area_t clip_area;
    if(!get_piece_clip(t, dsc, &clip_area))
        return;

    nema_set_clip(clip_area.x1, clip_area.y1, lv_area_get_width(&clip_area), lv_area_get_height(&clip_area));
    draw_piece(t, dsc);
}
#endif

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void lv_draw_nema_gfx_line(lv_draw_task_t * t, const lv_draw_line_dsc_t * dsc)
{
    lv_area_t clip_area;
    if(!get_piece_clip(t, dsc, &clip_area))
        return;

    lv_draw_nema_gfx_unit_t * draw_nema_gfx_unit = (lv_draw_nema_gfx_unit_t *)t->draw_unit;

    nema_set_clip(clip_area.x1, clip_area.y1, lv_area_get_width(&clip_area), lv_area_get_height(&clip_area));
    bind_destination(t, dsc);
    draw_piece(t, dsc);

    nema_cl_submit(&(draw_nema_gfx_unit->cl));
}

void lv_draw_nema_gfx_line_task(lv_draw_task_t * t, lv_draw_line_dsc_t * dsc)
{
#if LV_NEMA_GFX_LINE_BATCHING
    if(dsc->width == 0 || dsc->opa <= (lv_opa_t)LV_OPA_MIN)
        return;

    lv_draw_nema_gfx_unit_t * draw_nema_gfx_unit = (lv_draw_nema_gfx_unit_t *)t->draw_unit;
    bind_destination(t, dsc);
    lv_draw_line_iterate(t, dsc, draw_batched_piece);
    nema_cl_submit(&(draw_nema_gfx_unit->cl));
#else
    lv_draw_line_iterate(t, dsc, lv_draw_nema_gfx_line);
#endif
}
#endif
