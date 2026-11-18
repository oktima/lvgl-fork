/**
 * @file lv_draw_g2d_img.c
 *
 */

/**
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: MIT
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_g2d.h"

#if LV_USE_DRAW_G2D
#include <math.h>
#include <g2d.h>
#include "../../../misc/lv_area_private.h"
#include "../../lv_draw_image_private.h"
#include "../../../image/lv_image_decoder_private.h"
#include "lv_g2d_utils.h"
#include "lv_g2d_buf_map.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void _g2d_draw_core_cb(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                              const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup, const lv_area_t * img_coords,
                              const lv_area_t * clipped_img_area);

static struct g2d_buf * _g2d_handle_src_buf(const lv_draw_buf_t * data);

static void _g2d_set_src_surf(struct g2d_surface * surf, struct g2d_buf * buf, const lv_area_t * area,
                              const lv_draw_image_dsc_t * dsc, const lv_draw_buf_t * decoded);

static void _g2d_set_tmp_surf(struct g2d_surface * tmp_surf, struct g2d_buf * buf, const lv_area_t * area,
                              lv_color_format_t cf);

static void _g2d_set_dst_surf(struct g2d_surface * surf, struct g2d_buf * buf, const lv_area_t * area,
                              const lv_draw_buf_t * draw_buf);

/* Blit simple w/ opa and alpha channel */
static void _g2d_blit(void * handle, struct g2d_surface * dst_surf, struct g2d_surface * src_surf);

static void _g2d_blit_two_steps(void * handle, struct g2d_surface * dst_surf, struct g2d_surface * src_surf,
                                struct g2d_surface * tmp_surf);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_g2d_img(lv_draw_task_t * t)
{
    lv_draw_image_dsc_t * dsc = t->draw_dsc;

    if(dsc->opa <= (lv_opa_t)LV_OPA_MIN)
        return;

    lv_area_t * coords = &t->area;

    bool is_tiled = (dsc->tile != 0);

    if(is_tiled)
        lv_draw_image_tiled_helper(t, dsc, coords, _g2d_draw_core_cb, NULL);
    else
        lv_draw_image_normal_helper(t, dsc, coords, _g2d_draw_core_cb, NULL);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void _g2d_draw_core_cb(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                              const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup, const lv_area_t * img_coords,
                              const lv_area_t * clipped_img_area)
{
    LV_UNUSED(sup);
    lv_draw_buf_t * draw_buf = t->target_layer->draw_buf;
    const lv_draw_buf_t * decoded = decoder_dsc->decoded;

    lv_area_t rel_clip_area;
    rel_clip_area = *clipped_img_area;
    lv_area_move(&rel_clip_area, -img_coords->x1, -img_coords->y1);

    void * handle = g2d_get_handle();

    struct g2d_buf * src_buf = _g2d_handle_src_buf(decoded);
    struct g2d_buf * dst_buf = g2d_search_buf_map(draw_buf->data);
    struct g2d_surface dst_surf;
    _g2d_set_dst_surf(&dst_surf, dst_buf, clipped_img_area, draw_buf);

    if(draw_dsc->rotation != 0) {
        // The following code has not been verified because rotation is not currently supported

        /** If the image has rotation, then blit in two steps:
         *   1. Source with rotation to temporary surface.
         *   2. Temporary with other transformations (if any) to destination (frame buffer).
         */
        lv_area_t src_area;
        src_area = rel_clip_area;
        lv_color_format_t src_cf = decoded->header.cf;
        struct g2d_surface src_surf;
        _g2d_set_src_surf(&src_surf, src_buf, &src_area, draw_dsc, decoded);
        struct g2d_buf * tmp_buf = g2d_alloc(lv_area_get_width(&src_area) * lv_area_get_height(
                                                 &src_area) *  lv_color_format_get_size(src_cf), 1);
        G2D_ASSERT_MSG(tmp_buf, "Failed to alloc temporary buffer.");
        struct g2d_surface tmp_surf;
        _g2d_set_tmp_surf(&tmp_surf, tmp_buf, &src_area, src_cf);
        _g2d_blit_two_steps(handle, &dst_surf, &src_surf, &tmp_surf);
        g2d_free(tmp_buf);
    } else if(draw_dsc->scale_x != LV_SCALE_NONE || draw_dsc->scale_y != LV_SCALE_NONE) {
        const lv_image_header_t * header = &decoded->header;

        // Inverse-map rel_clip_area (already clipped to the scaled image bounds
        // by lv_draw_image_normal_helper) back to source coordinates.
        // Scaling is applied around the pivot: dst = pivot + (src - pivot) * scale / LV_SCALE_NONE
        // so the inverse is: src = pivot + (dst - pivot) * LV_SCALE_NONE / scale
        // Round x1/y1 down and x2/y2 up to avoid missing edge pixels.
        lv_area_t src_area;
        src_area = rel_clip_area;
        lv_area_move(&src_area, -draw_dsc->pivot.x, -draw_dsc->pivot.y);
        src_area.x1 = (src_area.x1 * LV_SCALE_NONE) / draw_dsc->scale_x;
        src_area.y1 = (src_area.y1 * LV_SCALE_NONE) / draw_dsc->scale_y;
        src_area.x2 = ((src_area.x2 * LV_SCALE_NONE) + draw_dsc->scale_x - 1) / draw_dsc->scale_x;
        src_area.y2 = ((src_area.y2 * LV_SCALE_NONE) + draw_dsc->scale_y - 1) / draw_dsc->scale_y;
        lv_area_move(&src_area, draw_dsc->pivot.x, draw_dsc->pivot.y);

        // Clamp to source image bounds
        src_area.x1 = LV_MAX(src_area.x1, 0);
        src_area.y1 = LV_MAX(src_area.y1, 0);
        src_area.x2 = LV_MIN(src_area.x2, header->w - 1);
        src_area.y2 = LV_MIN(src_area.y2, header->h - 1);

        struct g2d_surface src_surf;
        _g2d_set_src_surf(&src_surf, src_buf, &src_area, draw_dsc, decoded);

        // Single blit: G2D scales src_area -> dst rect in hardware
        _g2d_blit(handle, &dst_surf, &src_surf);
    } else {
        lv_area_t src_area;
        src_area = rel_clip_area;
        struct g2d_surface src_surf;
        _g2d_set_src_surf(&src_surf, src_buf, &src_area, draw_dsc, decoded);

        // If rotation is not involved, blit in one step.
        _g2d_blit(handle, &dst_surf, &src_surf);
    }
}

static struct g2d_buf * _g2d_handle_src_buf(const lv_draw_buf_t * img_dsc)
{
    struct g2d_buf * src_buf = g2d_search_buf_map((void *)img_dsc->data);

    if(src_buf == NULL) {
        src_buf = g2d_alloc(img_dsc->data_size, 1);
        G2D_ASSERT_MSG(src_buf, "Failed to alloc source buffer.");
        memcpy((uint8_t *)src_buf->buf_vaddr, img_dsc->data, img_dsc->data_size);
        g2d_cache_op(src_buf, G2D_CACHE_FLUSH);
        g2d_insert_buf_map((void *)img_dsc->data, src_buf);
    }

    return src_buf;
}

static void _g2d_set_surf(struct g2d_surface * surf, struct g2d_buf * buf, const lv_area_t * area,
                          const lv_image_header_t * header)
{
    lv_color_format_t cf = header->cf;
    // G2D takes stride in pixels
    const uint8_t pixel_size = lv_color_format_get_size(cf); // pixel size in bytes
    LV_ASSERT(pixel_size != 0);
    uint32_t stride = header->stride == 0 ? header->w * pixel_size : header->stride; // stride in bytes
    stride /= pixel_size; // stride in pixels

    surf->format = g2d_get_buf_format(cf);
    surf->planes[0] = buf->buf_paddr;    
    surf->left   = area->x1;
    surf->top    = area->y1;
    surf->right  = area->x1 + lv_area_get_width(area);
    surf->bottom = area->y1 + lv_area_get_height(area);
    surf->stride = stride;
    surf->width  = header->w;
    surf->height = header->h;
    surf->clrcolor = g2d_rgba_to_u32(lv_color_black());    
}

static void _g2d_set_src_surf(struct g2d_surface * surf, struct g2d_buf * buf, const lv_area_t * area,
                              const lv_draw_image_dsc_t * dsc, const lv_draw_buf_t * decoded)
{
    bool has_rotation = (dsc->rotation != 0);
    enum g2d_rotation g2d_angle = G2D_ROTATION_0;

    if(has_rotation) {
        switch(dsc->rotation) {
            case 0:
                g2d_angle = G2D_ROTATION_0;
                break;
            case 900:
                g2d_angle = G2D_ROTATION_270;
                break;
            case 1800:
                g2d_angle = G2D_ROTATION_180;
                break;
            case 2700:
                g2d_angle = G2D_ROTATION_90;
                break;
            default:
                g2d_angle = G2D_ROTATION_0;
        }
    }

    _g2d_set_surf(surf, buf, area, &decoded->header);
    surf->rot = g2d_angle;
    surf->global_alpha = dsc->opa;
    surf->blendfunc = G2D_ONE | G2D_PRE_MULTIPLIED_ALPHA;
}

static void _g2d_set_dst_surf(struct g2d_surface * surf, struct g2d_buf * buf, const lv_area_t * area,
                              const lv_draw_buf_t * draw_buf)
{
    _g2d_set_surf(surf, buf, area, &draw_buf->header);
    surf->rot = G2D_ROTATION_0;
    surf->global_alpha = 0xff;
    surf->blendfunc = G2D_ONE_MINUS_SRC_ALPHA | G2D_PRE_MULTIPLIED_ALPHA;
}

static void _g2d_set_tmp_surf(struct g2d_surface * tmp_surf, struct g2d_buf * buf, const lv_area_t * area,
                              lv_color_format_t cf)
{
    tmp_surf->format = g2d_get_buf_format(cf);

    int32_t dest_w = lv_area_get_width(area);
    int32_t dest_h = lv_area_get_height(area);

    tmp_surf->left   = area->x1;
    tmp_surf->top    = area->y1;
    tmp_surf->right  = area->x1 + dest_w;
    tmp_surf->bottom = area->y1 + dest_h;
    tmp_surf->stride = dest_w;
    tmp_surf->width  = dest_w;
    tmp_surf->height = dest_h;

    tmp_surf->planes[0] = buf->buf_paddr;
    tmp_surf->rot = G2D_ROTATION_0;

    tmp_surf->clrcolor = g2d_rgba_to_u32(lv_color_black());
    tmp_surf->global_alpha = 0x00;
    tmp_surf->blendfunc = G2D_ONE_MINUS_SRC_ALPHA | G2D_PRE_MULTIPLIED_ALPHA;
}

static void _g2d_blit(void * handle, struct g2d_surface * dst_surf, struct g2d_surface * src_surf)
{
    g2d_enable(handle, G2D_BLEND);
    g2d_enable(handle, G2D_GLOBAL_ALPHA);
    g2d_blit(handle, src_surf, dst_surf);
    g2d_finish(handle);
    g2d_disable(handle, G2D_GLOBAL_ALPHA);
    g2d_disable(handle, G2D_BLEND);
}

static void _g2d_blit_two_steps(void * handle, struct g2d_surface * dst_surf, struct g2d_surface * src_surf,
                                struct g2d_surface * tmp_surf)
{
    g2d_clear(handle, tmp_surf);

    g2d_enable(handle, G2D_BLEND);
    g2d_enable(handle, G2D_GLOBAL_ALPHA);
    g2d_blit(handle, src_surf, tmp_surf);
    g2d_finish(handle);
    g2d_disable(handle, G2D_GLOBAL_ALPHA);
    g2d_disable(handle, G2D_BLEND);

    /**After first blit, change blending and global alpha for temporary surface
     * since the surface now acts as source.
     */
    tmp_surf->blendfunc = G2D_ONE | G2D_PRE_MULTIPLIED_ALPHA;
    tmp_surf->global_alpha = 0xFF;

    g2d_enable(handle, G2D_BLEND);
    g2d_enable(handle, G2D_GLOBAL_ALPHA);
    g2d_blit(handle, tmp_surf, dst_surf);
    g2d_finish(handle);
    g2d_disable(handle, G2D_GLOBAL_ALPHA);
    g2d_disable(handle, G2D_BLEND);
}
#endif /*LV_USE_DRAW_G2D*/
