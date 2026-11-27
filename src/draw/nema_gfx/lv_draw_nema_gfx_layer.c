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
 * @file lv_draw_nema_gfx_layer.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_draw_nema_gfx.h"

#if LV_USE_NEMA_GFX

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool apply_mask(const lv_draw_image_dsc_t * draw_dsc, lv_layer_t * layer_to_draw, const lv_area_t * coords);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_nema_gfx_layer(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc, const lv_area_t * coords)
{
    lv_layer_t * layer_to_draw = (lv_layer_t *)draw_dsc->src;

    /*It can happen that nothing was draw on a layer and therefore its buffer is not allocated.
     *In this case just return. */
    if(layer_to_draw->draw_buf == NULL) return;

    /*The GPU reads the layer from the memory: clean the data cache, where the software renderer leaves what it
     *drew in the layer*/
    lv_draw_buf_flush_cache(layer_to_draw->draw_buf, NULL);

    lv_draw_image_dsc_t new_draw_dsc = *draw_dsc;
    new_draw_dsc.src = layer_to_draw->draw_buf;

    /*The stencil of the blit scales only the alpha of the source, while a premultiplied layer needs its color scaled
     *as well: the mask is applied to the layer first, as the software renderer does, and the layer is composed
     *without it*/
    if(draw_dsc->bitmap_mask_src && layer_to_draw->draw_buf->header.cf == LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED) {
        if(!apply_mask(draw_dsc, layer_to_draw, coords)) return;
        new_draw_dsc.bitmap_mask_src = NULL;
    }

    lv_draw_nema_gfx_img(t, &new_draw_dsc, coords);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/*Multiply every channel of the layer by the mask with NEMA_BL_DST_IN, the mask read as an A8 texture, and clear the
 *part of the layer outside the mask. The commands go in the command list before those of the blit of the layer, so
 *the GPU runs them first. Return false if the layer is all outside the mask.*/
static bool apply_mask(const lv_draw_image_dsc_t * draw_dsc, lv_layer_t * layer_to_draw, const lv_area_t * coords)
{
    const lv_image_dsc_t * mask = draw_dsc->bitmap_mask_src;
    if(mask->header.cf != LV_COLOR_FORMAT_A8 && mask->header.cf != LV_COLOR_FORMAT_L8) {
        LV_LOG_WARN("The mask image is not A8/L8 format. The mask is not applied.");
        return true;
    }

    /*Align the mask to the center of the whole image area, as the stencil and the software renderer do*/
    const lv_area_t * image_area = lv_area_get_width(&draw_dsc->image_area) < 0 ? coords : &draw_dsc->image_area;
    lv_area_t mask_area;
    lv_area_set(&mask_area, 0, 0, mask->header.w - 1, mask->header.h - 1);
    lv_area_align(image_area, &mask_area, LV_ALIGN_CENTER, 0, 0);

    lv_draw_buf_t * draw_buf = layer_to_draw->draw_buf;
    const lv_area_t * buf_area = &layer_to_draw->buf_area;
    int32_t buf_w = lv_area_get_width(buf_area);
    int32_t buf_h = lv_area_get_height(buf_area);

    lv_area_t masked_area;
    if(!lv_area_intersect(&masked_area, &mask_area, buf_area)) return false;

    nema_bind_dst_tex((uintptr_t)NEMA_VIRT2PHYS(draw_buf->data), buf_w, buf_h,
                      lv_nemagfx_cf_to_nema(draw_buf->header.cf), draw_buf->header.stride);
    nema_set_clip(0, 0, buf_w, buf_h);

    /*Outside the mask the layer is transparent*/
    nema_set_blend_fill(NEMA_BL_SRC);
    int32_t masked_x1 = masked_area.x1 - buf_area->x1;
    int32_t masked_y1 = masked_area.y1 - buf_area->y1;
    int32_t masked_x2 = masked_area.x2 - buf_area->x1;
    int32_t masked_y2 = masked_area.y2 - buf_area->y1;
    if(masked_y1 > 0) nema_fill_rect(0, 0, buf_w, masked_y1, 0);
    if(masked_y2 < buf_h - 1) nema_fill_rect(0, masked_y2 + 1, buf_w, buf_h - 1 - masked_y2, 0);
    if(masked_x1 > 0) nema_fill_rect(0, masked_y1, masked_x1, masked_y2 - masked_y1 + 1, 0);
    if(masked_x2 < buf_w - 1) nema_fill_rect(masked_x2 + 1, masked_y1, buf_w - 1 - masked_x2,
                                                 masked_y2 - masked_y1 + 1, 0);

    /*Inside, every channel times the mask: the bytes of an L8 mask read as A8 are its alpha*/
    uint32_t mask_stride = mask->header.stride ? mask->header.stride : mask->header.w;
    const uint8_t * mask_buf = mask->data;
    mask_buf += mask_stride * (masked_area.y1 - mask_area.y1) + (masked_area.x1 - mask_area.x1);
    nema_bind_src_tex((uintptr_t)NEMA_VIRT2PHYS(mask_buf), lv_area_get_width(&masked_area),
                      lv_area_get_height(&masked_area), NEMA_A8, (int32_t)mask_stride, NEMA_FILTER_PS);
    nema_set_blend_blit(NEMA_BL_DST_IN);
    nema_blit(masked_x1, masked_y1);

    return true;
}

#endif /*LV_USE_NEMA_GFX*/
