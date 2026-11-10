/**
 * @file lv_draw_opengles.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_opengles.h"
#if LV_USE_DRAW_OPENGLES

#if LV_USE_DRAW_NANOVG
    #error "LV_USE_DRAW_NANOVG and LV_USE_DRAW_OPENGLES cannot be enabled at the same time. Disable one of them in lv_conf.h or Kconfig."
#endif

#include "../lv_draw_private.h"
#include "../../misc/cache/lv_cache_entry_private.h"
#include "../../drivers/opengles/lv_opengles_debug.h"
#include "../../drivers/opengles/lv_opengles_texture.h"
#include "../../drivers/opengles/lv_opengles_driver.h"
#include "../../drivers/opengles/lv_opengles_private.h"
#include "../../draw/lv_draw_label.h"
#include "../../draw/lv_draw_rect.h"
#include "../../draw/lv_draw_arc.h"
#include "../../draw/lv_draw_image.h"
#include "../../draw/lv_draw_triangle.h"
#include "../../draw/lv_draw_line.h"
#include "../../draw/lv_draw_3d.h"
#include "../../core/lv_obj.h"
#include "../../core/lv_refr_private.h"
#include "../../display/lv_display_private.h"
#include "../../stdlib/lv_string.h"
#include "../../misc/lv_area_private.h"

/*********************
 *      DEFINES
 *********************/

#define DRAW_UNIT_ID_OPENGLES 6

#define USE_MY_LOG 0

#if USE_MY_LOG
    #define MY_LOG(...) LV_LOG_WARN(__VA_ARGS__)
#else
    #define MY_LOG(...) ((void)0)
#endif

/**
 * Compare two draw descriptor structs of `dsc_type`, byte-wise, but skip `field`
 * (e.g. a pointer compared separately). Matches the generic path after `lv_draw_dsc_base_t`.
 */
#define LV_DRAW_DSC_MEMCMP_SKIP_FIELD(lhs, rhs, dsc_type, field) \
    do { \
        int cmp_res = lv_memcmp((const uint8_t *)(lhs) + sizeof(lv_draw_dsc_base_t), \
                                (const uint8_t *)(rhs) + sizeof(lv_draw_dsc_base_t), \
                                offsetof(dsc_type, field) - sizeof(lv_draw_dsc_base_t)); \
        if(cmp_res != 0) return cmp_res > 0 ? 1 : -1; \
        const size_t after_field_offset = offsetof(dsc_type, field) + sizeof((lhs)->field); \
        cmp_res = lv_memcmp((const uint8_t *)(lhs) + after_field_offset, \
                            (const uint8_t *)(rhs) + after_field_offset, \
                            sizeof(dsc_type) - after_field_offset); \
        if(cmp_res != 0) return cmp_res > 0 ? 1 : -1; \
    } while(0)

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    lv_draw_unit_t base_unit;
    lv_draw_task_t * task_act;
    lv_cache_t * texture_cache;
    unsigned int framebuffer;
    lv_draw_buf_t render_draw_buf;
    int max_texture_size;
} lv_draw_opengles_unit_t;

typedef struct {
    lv_cache_slot_size_t slot;
    lv_obj_t * obj; // Set only for dynamic parts
    lv_part_t part; // Set only for dynamic parts
    lv_draw_task_type_t task_type;
    lv_draw_dsc_base_t * draw_dsc;
    int32_t w;
    int32_t h;
    unsigned int texture;
} cache_data_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool opengles_texture_cache_create_cb(cache_data_t * cached_data, void * user_data);
static void opengles_texture_cache_free_cb(cache_data_t * cached_data, void * user_data);
static lv_cache_compare_res_t opengles_texture_cache_compare_cb(const cache_data_t * lhs, const cache_data_t * rhs);
static lv_cache_compare_res_t compare_dynamic_part(const cache_data_t * lhs, const cache_data_t * rhs);
static lv_cache_compare_res_t compare_static_part(const cache_data_t * lhs, const cache_data_t * rhs);
static lv_cache_compare_res_t compare_image_dsc(const lv_draw_image_dsc_t * lhs, const lv_draw_image_dsc_t * rhs);
static lv_cache_compare_res_t compare_label_dsc(const lv_draw_label_dsc_t * lhs, const lv_draw_label_dsc_t * rhs);

static void blend_texture_layer(lv_draw_task_t * t);
static bool is_task_for_dynamic_part(const lv_draw_task_t * task);
static lv_cache_entry_t * acquire_or_create_cache_entry(lv_draw_opengles_unit_t * u, cache_data_t * data_to_find);
static void draw_from_cached_texture(lv_draw_task_t * t);

static void execute_drawing(lv_draw_opengles_unit_t * u);

static int32_t delete(lv_draw_unit_t * draw_unit);
static int32_t dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer);

static int32_t evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task);
static lv_area_t get_texture_area(const lv_draw_task_t * task);
static unsigned int draw_to_texture(lv_draw_opengles_unit_t * u, cache_data_t * cache_data, lv_area_t * out_texture_area);
static lv_area_t get_blit_area(const lv_draw_task_t * task);
static void draw_texture_to_framebuffer(lv_draw_opengles_unit_t * u, unsigned int texture, lv_opa_t opa,
                                        const lv_area_t * blit_area);
static void draw_to_framebuffer(lv_draw_opengles_unit_t * u);

static unsigned int layer_get_texture(lv_layer_t * layer);
static unsigned int get_framebuffer(lv_draw_opengles_unit_t * u);
static unsigned int create_texture(int32_t w, int32_t h, const void * data);

#if LV_USE_3DTEXTURE
    static void lv_draw_opengles_3d(lv_draw_task_t * t, const lv_draw_3d_dsc_t * dsc, const lv_area_t * coords);
#endif

#if USE_MY_LOG
static const char * task_type_to_string(lv_draw_task_type_t type);
static const char * part_to_string(lv_part_t part);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

static lv_draw_opengles_unit_t * g_unit;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_opengles_init(void)
{
    if(g_unit) return;

    lv_draw_opengles_unit_t * draw_opengles_unit = lv_draw_create_unit(sizeof(lv_draw_opengles_unit_t));
    draw_opengles_unit->base_unit.dispatch_cb = dispatch;
    draw_opengles_unit->base_unit.evaluate_cb = evaluate;
    draw_opengles_unit->base_unit.delete_cb = delete;
    draw_opengles_unit->base_unit.name = "OPENGLES";
    draw_opengles_unit->texture_cache = lv_cache_create(&lv_cache_class_lru_rb_size,
    sizeof(cache_data_t), LV_DRAW_OPENGLES_TEXTURE_CACHE_SIZE, (lv_cache_ops_t) {
        .compare_cb = (lv_cache_compare_cb_t)opengles_texture_cache_compare_cb,
        .create_cb = (lv_cache_create_cb_t)opengles_texture_cache_create_cb,
        .free_cb = (lv_cache_free_cb_t)opengles_texture_cache_free_cb,
    });
    lv_cache_set_name(draw_opengles_unit->texture_cache, "OPENGLES_TEXTURE");

    lv_draw_buf_init(&draw_opengles_unit->render_draw_buf, 0, 0, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO, NULL, 0);

    g_unit = draw_opengles_unit;
}

void lv_draw_opengles_deinit(void)
{
    if(!g_unit) return;

    lv_draw_remove_unit((lv_draw_unit_t *)g_unit);
    g_unit = NULL;
}

size_t lv_draw_opengles_get_texture_cache_size(void) {
    if(!g_unit) return 0;

    return lv_cache_get_size(g_unit->texture_cache, NULL);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static bool opengles_texture_cache_create_cb(cache_data_t * cached_data, void * user_data)
{
    LV_PROFILER_DRAW_BEGIN;
    bool ret = draw_to_texture((lv_draw_opengles_unit_t *)user_data, cached_data, NULL) != 0;
    LV_PROFILER_DRAW_END;
    return ret;
}

static void opengles_texture_cache_free_cb(cache_data_t * cached_data, void * user_data)
{
    LV_UNUSED(user_data);
    LV_PROFILER_DRAW_BEGIN;

    /*Frees only what draw_to_texture() allocated. draw_dsc is NULL when the copy of the descriptor
     *failed to allocate: then there is no texture either*/
    if(cached_data->draw_dsc == NULL) {
        LV_PROFILER_DRAW_END;
        return;
    }

    MY_LOG("Removing cached texture %d of size %d x %d for task type %s, obj %p, part %s",
           cached_data->texture, cached_data->w, cached_data->h,
           task_type_to_string(cached_data->task_type),
           cached_data->obj, part_to_string(cached_data->part));

    switch(cached_data->task_type) {
        case LV_DRAW_TASK_TYPE_IMAGE: {
                lv_draw_image_dsc_t * image_dsc = (lv_draw_image_dsc_t *)cached_data->draw_dsc;
                lv_image_src_t src_type = lv_image_src_get_type(image_dsc->src);
                if(src_type == LV_IMAGE_SRC_FILE || src_type == LV_IMAGE_SRC_SYMBOL) {
                    lv_free((void *)image_dsc->src);
                    image_dsc->src = NULL;
                }
            }
            break;
        case LV_DRAW_TASK_TYPE_LABEL: {
                lv_draw_label_dsc_t * label_dsc = (lv_draw_label_dsc_t *)cached_data->draw_dsc;
                if(label_dsc->text != NULL) {
                    lv_free((void *)label_dsc->text);
                    label_dsc->text = NULL;
                }
            }
            break;
        default:
            break;
    }

    lv_free(cached_data->draw_dsc);

    GL_CALL(glDeleteTextures(1, &cached_data->texture));

    LV_PROFILER_DRAW_END;
}

static lv_cache_compare_res_t opengles_texture_cache_compare_cb(const cache_data_t * lhs, const cache_data_t * rhs)
{
    if(lhs == rhs) return 0;

    if(lhs->obj != NULL && rhs->obj != NULL) {
        return compare_dynamic_part(lhs, rhs);
    } else if(lhs->obj == NULL && rhs->obj == NULL) {
        return compare_static_part(lhs, rhs);
    } else {
        return lhs->obj == NULL ? -1 : 1;
    }
}

static lv_cache_compare_res_t compare_dynamic_part(const cache_data_t * lhs, const cache_data_t * rhs)
{
    if(lhs == rhs) return 0;

    if(lhs->obj != rhs->obj) {
        return lhs->obj > rhs->obj ? 1 : -1;
    }

    if(lhs->part != rhs->part) {
        return lhs->part > rhs->part ? 1 : -1;
    }

    if(lhs->task_type != rhs->task_type) {
        return lhs->task_type > rhs->task_type ? 1 : -1;
    }

    return 0;
}

static lv_cache_compare_res_t compare_static_part(const cache_data_t * lhs, const cache_data_t * rhs)
{
    if(lhs == rhs) return 0;

    if(lhs->task_type != rhs->task_type) {
        return lhs->task_type > rhs->task_type ? 1 : -1;
    }

    if(lhs->w != rhs->w) {
        return lhs->w > rhs->w ? 1 : -1;
    }
    if(lhs->h != rhs->h) {
        return lhs->h > rhs->h ? 1 : -1;
    }

    if(lhs->draw_dsc == NULL || rhs->draw_dsc == NULL) {
        if(lhs->draw_dsc == rhs->draw_dsc) return 0;
        if(lhs->draw_dsc == NULL) return -1;
        return 1;
    }

    uint32_t lhs_dsc_size = lhs->draw_dsc->dsc_size;
    uint32_t rhs_dsc_size = rhs->draw_dsc->dsc_size;

    if(lhs_dsc_size != rhs_dsc_size) {
        return lhs_dsc_size > rhs_dsc_size ? 1 : -1;
    }

    switch(lhs->task_type) {
        case LV_DRAW_TASK_TYPE_IMAGE:
            return compare_image_dsc((const lv_draw_image_dsc_t *)lhs->draw_dsc, (const lv_draw_image_dsc_t *)rhs->draw_dsc);
        case LV_DRAW_TASK_TYPE_LABEL:
            return compare_label_dsc((const lv_draw_label_dsc_t *)lhs->draw_dsc, (const lv_draw_label_dsc_t *)rhs->draw_dsc);
        default:
            break;
    }

    const uint8_t * left_draw_dsc = (const uint8_t *)lhs->draw_dsc;
    const uint8_t * right_draw_dsc = (const uint8_t *)rhs->draw_dsc;
    left_draw_dsc += sizeof(lv_draw_dsc_base_t);
    right_draw_dsc += sizeof(lv_draw_dsc_base_t);

    int cmp_res = lv_memcmp(left_draw_dsc, right_draw_dsc, lhs->draw_dsc->dsc_size - sizeof(lv_draw_dsc_base_t));

    if(cmp_res != 0) {
        return cmp_res > 0 ? 1 : -1;
    }

    return 0;
}

static lv_cache_compare_res_t compare_image_dsc(const lv_draw_image_dsc_t * lhs, const lv_draw_image_dsc_t * rhs)
{
    if(lhs == rhs) return 0;

    LV_DRAW_DSC_MEMCMP_SKIP_FIELD(lhs, rhs, lv_draw_image_dsc_t, src);

    lv_image_src_t lhs_src_type = lv_image_src_get_type(lhs->src);
    lv_image_src_t rhs_src_type = lv_image_src_get_type(rhs->src);
    if(lhs_src_type != rhs_src_type) {
        return lhs_src_type > rhs_src_type ? 1 : -1;
    }

    /*Compare src: string compare for file/symbol sources, pointer compare otherwise*/
    if(lhs_src_type == LV_IMAGE_SRC_FILE || lhs_src_type == LV_IMAGE_SRC_SYMBOL) {
        if(lhs->src == NULL || rhs->src == NULL) {
            if(lhs->src == rhs->src) return 0;
            return lhs->src == NULL ? -1 : 1;
        }
        int cmp_res = lv_strcmp((const char *)lhs->src, (const char *)rhs->src);
        if(cmp_res != 0) return cmp_res > 0 ? 1 : -1;
    }
    else {
        if(lhs->src != rhs->src) return lhs->src > rhs->src ? 1 : -1;
    }

    return 0;
}

static lv_cache_compare_res_t compare_label_dsc(const lv_draw_label_dsc_t * lhs, const lv_draw_label_dsc_t * rhs)
{
    if(lhs == rhs) return 0;

    LV_DRAW_DSC_MEMCMP_SKIP_FIELD(lhs, rhs, lv_draw_label_dsc_t, text);

    if(lhs->text == NULL || rhs->text == NULL) {
        if(lhs->text == rhs->text) return 0;
        return lhs->text == NULL ? -1 : 1;
    }

    int cmp_res = lv_strcmp(lhs->text, rhs->text);
    if(cmp_res != 0) return cmp_res > 0 ? 1 : -1;

    return 0;
}

static int32_t delete(lv_draw_unit_t * draw_unit)
{
    lv_draw_opengles_unit_t * draw_opengles_unit = (lv_draw_opengles_unit_t *) draw_unit;
    lv_free(draw_opengles_unit->render_draw_buf.unaligned_data);
    lv_cache_destroy(draw_opengles_unit->texture_cache, draw_opengles_unit);
    if(draw_opengles_unit->framebuffer != 0) {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        GL_CALL(glDeleteFramebuffers(1, &draw_opengles_unit->framebuffer));
    }

    if(draw_opengles_unit == g_unit) {
        g_unit = NULL;
    }

    return LV_RESULT_OK;
}

static int32_t dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer)
{
    lv_draw_opengles_unit_t * draw_opengles_unit = (lv_draw_opengles_unit_t *) draw_unit;

    /*Return immediately if it's busy with a draw task*/
    if(draw_opengles_unit->task_act) return 0;

    lv_draw_task_t * t = NULL;
    t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_OPENGLES);
    if(t == NULL) return -1;

    unsigned int texture = layer_get_texture(layer);
    if(texture == 0) {
        lv_display_t * disp = lv_refr_get_disp_refreshing();
        LV_ASSERT(layer != disp->layer_head);
        int32_t w = lv_area_get_width(&layer->buf_area);
        int32_t h = lv_area_get_height(&layer->buf_area);

        texture = create_texture(w, h, NULL);
        layer->user_data = (void *)(uintptr_t)texture;
    }

    t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    draw_opengles_unit->task_act = t;

    execute_drawing(draw_opengles_unit);

    draw_opengles_unit->task_act->state = LV_DRAW_TASK_STATE_FINISHED;
    draw_opengles_unit->task_act = NULL;

    /*The draw unit is free now. Request a new dispatching as it can get a new task*/
    lv_draw_dispatch_request();
    return 1;
}

static int32_t evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task)
{
    LV_UNUSED(draw_unit);

    switch(task->type) {
        case LV_DRAW_TASK_TYPE_FILL:
        case LV_DRAW_TASK_TYPE_BORDER:
        case LV_DRAW_TASK_TYPE_BOX_SHADOW:
        case LV_DRAW_TASK_TYPE_LABEL:
        case LV_DRAW_TASK_TYPE_ARC:
        case LV_DRAW_TASK_TYPE_LINE:
        case LV_DRAW_TASK_TYPE_TRIANGLE:
        case LV_DRAW_TASK_TYPE_LAYER:
#if LV_USE_3DTEXTURE
        case LV_DRAW_TASK_TYPE_3D:
#endif
            break;
        case LV_DRAW_TASK_TYPE_IMAGE: {
                if(((lv_draw_image_dsc_t *)task->draw_dsc)->header.cf >= LV_COLOR_FORMAT_PROPRIETARY_START) {
                    return 0;
                }
                break;
            }
        default:
            return 0;
    }

    /*If not refreshing the display probably it's a canvas rendering
     *which his not supported in OpenGL as it's not a texture.*/
    if(lv_refr_get_disp_refreshing() == NULL) return 0;

    if(((lv_draw_dsc_base_t *)task->draw_dsc)->user_data == NULL) {
        task->preference_score = 0;
        task->preferred_draw_unit_id = DRAW_UNIT_ID_OPENGLES;
    }
    return 0;
}

static lv_opa_t replace_opa_in_task(const lv_draw_task_t * task, lv_opa_t opa)
{
    LV_ASSERT_NULL(task);

	switch(task->type) {
        case LV_DRAW_TASK_TYPE_FILL: {
            lv_draw_fill_dsc_t * fill_dsc = (lv_draw_fill_dsc_t *)task->draw_dsc;
			lv_opa_t old_opa = fill_dsc->opa;
            fill_dsc->opa = opa;
            return old_opa;
		}
		case LV_DRAW_TASK_TYPE_BORDER: {
            lv_draw_border_dsc_t * border_dsc = (lv_draw_border_dsc_t *)task->draw_dsc;
			lv_opa_t old_opa = border_dsc->opa;
            border_dsc->opa = opa;
            return old_opa;
		}
        case LV_DRAW_TASK_TYPE_BOX_SHADOW: {
            lv_draw_box_shadow_dsc_t * box_shadow_dsc = (lv_draw_box_shadow_dsc_t *)task->draw_dsc;
			lv_opa_t old_opa = box_shadow_dsc->opa;
            box_shadow_dsc->opa = opa;
            return old_opa;
        }
        case LV_DRAW_TASK_TYPE_LABEL: {
            lv_draw_label_dsc_t * label_dsc = (lv_draw_label_dsc_t *)task->draw_dsc;
			lv_opa_t old_opa = label_dsc->opa;
            label_dsc->opa = opa;
            return old_opa;
        }
        case LV_DRAW_TASK_TYPE_ARC: {
            lv_draw_arc_dsc_t * arc_dsc = (lv_draw_arc_dsc_t *)task->draw_dsc;
			lv_opa_t old_opa = arc_dsc->opa;
            arc_dsc->opa = opa;
            return old_opa;
        }
        case LV_DRAW_TASK_TYPE_LINE: {
            lv_draw_line_dsc_t * line_dsc = (lv_draw_line_dsc_t *)task->draw_dsc;
			lv_opa_t old_opa = line_dsc->opa;
            line_dsc->opa = opa;
            return old_opa;
        }
        case LV_DRAW_TASK_TYPE_TRIANGLE: {
            lv_draw_triangle_dsc_t * triangle_dsc = (lv_draw_triangle_dsc_t *)task->draw_dsc;
			lv_opa_t old_opa = triangle_dsc->opa;
            triangle_dsc->opa = opa;
            return old_opa;
        }
        case LV_DRAW_TASK_TYPE_IMAGE: {
            lv_draw_image_dsc_t * image_dsc = (lv_draw_image_dsc_t *)task->draw_dsc;
			lv_opa_t old_opa = image_dsc->opa;
            image_dsc->opa = opa;
            return old_opa;
        }
        default: {
            LV_LOG_ERROR("Unsupported draw task type: %d", task->type);
            LV_ASSERT(false);
            return LV_OPA_COVER;
        }
    }
}

static lv_area_t get_texture_area(const lv_draw_task_t * task)
{
    if(task->type == LV_DRAW_TASK_TYPE_IMAGE) {
        lv_draw_image_dsc_t * img_dsc = task->draw_dsc;
        if(img_dsc->real_slice_area.x2 != LV_COORD_MIN) {
            return img_dsc->real_slice_area;
        }
    }

    return task->_real_area;
}

static unsigned int draw_to_texture(lv_draw_opengles_unit_t * u, cache_data_t * cache_data, lv_area_t * out_texture_area)
{
    LV_PROFILER_DRAW_BEGIN;
    lv_draw_task_t * task = u->task_act;

    lv_layer_t dest_layer;
    lv_layer_init(&dest_layer);

    lv_area_t texture_area;
    if(cache_data != NULL) {
        texture_area = get_texture_area(task);
    } else {
        lv_area_intersect(&texture_area, &task->_real_area, &task->clip_area);
    }

    int32_t texture_w = lv_area_get_width(&texture_area);
    int32_t texture_h = lv_area_get_height(&texture_area);
    if(texture_w <= 0 || texture_h <= 0) {
        /*Nothing to draw. Never reached when caching: draw_from_cached_texture() leaves before the cache
         *on an empty area. Here cache_data->draw_dsc would still be the task's own descriptor, not a copy,
         *and opengles_texture_cache_free_cb() would free it when the cache discards the entry*/
        LV_ASSERT(cache_data == NULL);
        LV_PROFILER_DRAW_END;
        return 0;
    }

    if(NULL == lv_draw_buf_reshape(&u->render_draw_buf, LV_COLOR_FORMAT_ARGB8888, texture_w, texture_h, LV_STRIDE_AUTO)) {
        uint8_t * data = u->render_draw_buf.unaligned_data;
        uint32_t data_size = LV_DRAW_BUF_SIZE(texture_w, texture_h, LV_COLOR_FORMAT_ARGB8888);
        data = lv_realloc(data, data_size);
        LV_ASSERT_MALLOC(data);
        lv_result_t init_result = lv_draw_buf_init(&u->render_draw_buf, texture_w, texture_h, LV_COLOR_FORMAT_ARGB8888,
                                                   LV_STRIDE_AUTO, data, data_size);
        LV_ASSERT(init_result == LV_RESULT_OK);
    }

    dest_layer.draw_buf = &u->render_draw_buf;
    dest_layer.color_format = LV_COLOR_FORMAT_ARGB8888;

    dest_layer.buf_area = texture_area;
    dest_layer._clip_area = texture_area;
    dest_layer.phy_clip_area = texture_area;
    lv_memzero(u->render_draw_buf.data, lv_area_get_size(&texture_area) * 4);

    lv_display_t * disp = lv_refr_get_disp_refreshing();

    lv_obj_t * obj = ((lv_draw_dsc_base_t *)task->draw_dsc)->obj;
    bool original_send_draw_task_event = false;
    if(obj) {
        original_send_draw_task_event = lv_obj_has_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    }

    if(cache_data != NULL) {
        lv_draw_dsc_base_t * base_dsc = task->draw_dsc;
        cache_data->draw_dsc = lv_malloc(base_dsc->dsc_size);
        LV_ASSERT_MALLOC(cache_data->draw_dsc);
        if(cache_data->draw_dsc == NULL) {
            if(obj) {
                lv_obj_set_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS, original_send_draw_task_event);
            }
            LV_PROFILER_DRAW_END;
            return 0;
        }
        lv_memcpy((void *)cache_data->draw_dsc, base_dsc, base_dsc->dsc_size);
    }

    switch(task->type) {
        case LV_DRAW_TASK_TYPE_FILL: {
                lv_draw_fill_dsc_t * fill_dsc = task->draw_dsc;
                lv_draw_rect_dsc_t rect_dsc;
                lv_draw_rect_dsc_init(&rect_dsc);
                rect_dsc.base.user_data = (void *)(uintptr_t)1;
                rect_dsc.bg_color = fill_dsc->color;
                rect_dsc.bg_grad = fill_dsc->grad;
                rect_dsc.radius = fill_dsc->radius;
                rect_dsc.bg_opa = fill_dsc->opa;
                lv_draw_rect(&dest_layer, &rect_dsc, &task->area);
            }
            break;
        case LV_DRAW_TASK_TYPE_BORDER: {
                lv_draw_border_dsc_t * border_dsc = task->draw_dsc;
                lv_draw_rect_dsc_t rect_dsc;
                lv_draw_rect_dsc_init(&rect_dsc);
                rect_dsc.base.user_data = (void *)(uintptr_t)1;
                rect_dsc.bg_opa = LV_OPA_TRANSP;
                rect_dsc.radius = border_dsc->radius;
                rect_dsc.border_color = border_dsc->color;
                rect_dsc.border_opa = border_dsc->opa;
                rect_dsc.border_side = border_dsc->side;
                rect_dsc.border_width = border_dsc->width;
                lv_draw_rect(&dest_layer, &rect_dsc, &task->area);
                break;
            }
        case LV_DRAW_TASK_TYPE_BOX_SHADOW: {
                lv_draw_box_shadow_dsc_t * box_shadow_dsc = task->draw_dsc;
                lv_draw_rect_dsc_t rect_dsc;
                lv_draw_rect_dsc_init(&rect_dsc);
                rect_dsc.base.user_data = (void *)(uintptr_t)1;
                rect_dsc.bg_opa = LV_OPA_0;
                rect_dsc.radius = box_shadow_dsc->radius;
                rect_dsc.bg_color = box_shadow_dsc->color;
                rect_dsc.shadow_opa = box_shadow_dsc->opa;
                rect_dsc.shadow_width = box_shadow_dsc->width;
                rect_dsc.shadow_spread = box_shadow_dsc->spread;
                rect_dsc.shadow_offset_x = box_shadow_dsc->ofs_x;
                rect_dsc.shadow_offset_y = box_shadow_dsc->ofs_y;
                lv_draw_rect(&dest_layer, &rect_dsc, &task->area);
                break;
            }
        case LV_DRAW_TASK_TYPE_LABEL: {
                lv_draw_label_dsc_t label_dsc;
                lv_memcpy(&label_dsc, task->draw_dsc, sizeof(label_dsc));
                label_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_label(&dest_layer, &label_dsc, &task->area);

                if(cache_data != NULL) {
                    lv_draw_label_dsc_t * cached_label_dsc = (lv_draw_label_dsc_t *)cache_data->draw_dsc;
                    if(label_dsc.text != NULL) {
                        cached_label_dsc->text = lv_strdup(label_dsc.text);
                    }
                }
            }
            break;
        case LV_DRAW_TASK_TYPE_ARC: {
                lv_draw_arc_dsc_t arc_dsc;
                lv_memcpy(&arc_dsc, task->draw_dsc, sizeof(arc_dsc));
                arc_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_arc(&dest_layer, &arc_dsc);
            }
            break;
        case LV_DRAW_TASK_TYPE_LINE: {
                lv_draw_line_dsc_t line_dsc;
                lv_memcpy(&line_dsc, task->draw_dsc, sizeof(line_dsc));
                line_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_line(&dest_layer, &line_dsc);
            }
            break;
        case LV_DRAW_TASK_TYPE_TRIANGLE: {
                lv_draw_triangle_dsc_t triangle_dsc;
                lv_memcpy(&triangle_dsc, task->draw_dsc, sizeof(triangle_dsc));
                triangle_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_triangle(&dest_layer, &triangle_dsc);
            }
            break;
        case LV_DRAW_TASK_TYPE_IMAGE: {
                lv_draw_image_dsc_t image_dsc;
                lv_memcpy(&image_dsc, task->draw_dsc, sizeof(image_dsc));
                image_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_image(&dest_layer, &image_dsc, &task->area);

                if(cache_data != NULL) {
                    lv_draw_image_dsc_t * cached_image_dsc = (lv_draw_image_dsc_t *)cache_data->draw_dsc;
                    lv_image_src_t src_type = lv_image_src_get_type(image_dsc.src);
                    if(src_type == LV_IMAGE_SRC_FILE || src_type == LV_IMAGE_SRC_SYMBOL) {
                        cached_image_dsc->src = lv_strdup(image_dsc.src);
                    }
                }
            }
            break;
        default:
            /*The malloced cache_data->draw_dsc will be freed automatically on failure
            *in opengles_texture_cache_free_cb*/
            LV_ASSERT(false);
            if(obj) {
                lv_obj_set_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS, original_send_draw_task_event);
            }
            LV_PROFILER_DRAW_END;
            return 0;
    }

    while(dest_layer.draw_task_head) {
        lv_draw_dispatch_layer(disp, &dest_layer);
        if(dest_layer.draw_task_head) {
            lv_draw_dispatch_wait_for_request();
        }
    }

    unsigned int texture = create_texture(texture_w, texture_h, u->render_draw_buf.data);

    if(cache_data != NULL) {
        cache_data->texture = texture;

        MY_LOG("Caching %s texture %d of size %d x %d for task type %s, obj %p, part %s",
               cache_data->obj != NULL ? "dynamic" : "static",
               cache_data->texture, cache_data->w, cache_data->h,
               task_type_to_string(cache_data->task_type),
               obj, part_to_string(((lv_draw_dsc_base_t *)task->draw_dsc)->part));
    }

    if(obj) {
        lv_obj_set_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS, original_send_draw_task_event);
    }

    if(out_texture_area != NULL) {
        *out_texture_area = texture_area;
    }

    LV_PROFILER_DRAW_END;
    return texture;
}

static void blend_texture_layer(lv_draw_task_t * t)
{
    LV_PROFILER_DRAW_BEGIN;
    lv_draw_image_dsc_t * draw_dsc = t->draw_dsc;
    lv_draw_opengles_unit_t * u = (lv_draw_opengles_unit_t *)t->draw_unit;
    lv_area_t area;
    area.x1 = -draw_dsc->pivot.x;
    area.y1 = -draw_dsc->pivot.y;
    area.x1 = (area.x1 * draw_dsc->scale_x) / 256;
    area.y1 = (area.y1 * draw_dsc->scale_y) / 256;
    area.x1 += t->area.x1 + draw_dsc->pivot.x;
    area.y1 += t->area.y1 + draw_dsc->pivot.y;
    lv_area_set_width(&area, lv_area_get_width(&t->area) * draw_dsc->scale_x / 256);
    lv_area_set_height(&area, lv_area_get_height(&t->area) * draw_dsc->scale_y / 256);

    lv_layer_t * src_layer = (lv_layer_t *)draw_dsc->src;
    unsigned int src_texture = layer_get_texture(src_layer);


    lv_layer_t * dest_layer = t->target_layer;
    unsigned int target_texture = layer_get_texture(dest_layer);
    int32_t targ_tex_w = lv_area_get_width(&dest_layer->buf_area);
    int32_t targ_tex_h = lv_area_get_height(&dest_layer->buf_area);

    if(target_texture) {
        unsigned int framebuffer = get_framebuffer(u);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0));
    }

    lv_opengles_viewport(0, 0, targ_tex_w, targ_tex_h);
    // TODO rotation
    bool h_flip = false;
    bool v_flip = false;
#if LV_USE_3DTEXTURE
    if(t->type == LV_DRAW_TASK_TYPE_3D) {
        lv_draw_3d_dsc_t * _3d_dsc = (lv_draw_3d_dsc_t *)t->draw_dsc;
        h_flip = _3d_dsc->h_flip;
        v_flip = _3d_dsc->v_flip;
    }
#endif
    lv_opengles_render_texture(src_texture, &area, draw_dsc->opa, targ_tex_w, targ_tex_h, &t->clip_area, h_flip,
                               !v_flip);

    if(target_texture) {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }

    GL_CALL(glDeleteTextures(1, &src_texture));
    LV_PROFILER_DRAW_END;
}

static void draw_texture_to_framebuffer(lv_draw_opengles_unit_t * u, unsigned int texture, lv_opa_t opa,
                                        const lv_area_t * blit_area)
{
    lv_draw_task_t * t = u->task_act;

    bool h_flip = false;
    bool v_flip = false;
#if LV_USE_3DTEXTURE
    if(t->type == LV_DRAW_TASK_TYPE_3D) {
        lv_draw_3d_dsc_t * _3d_dsc = (lv_draw_3d_dsc_t *)t->draw_dsc;
        h_flip = _3d_dsc->h_flip;
        v_flip = _3d_dsc->v_flip;
    }
#endif

    lv_layer_t * dest_layer = t->target_layer;
    unsigned int target_texture = layer_get_texture(dest_layer);
    int32_t targ_tex_w = lv_area_get_width(&dest_layer->buf_area);
    int32_t targ_tex_h = lv_area_get_height(&dest_layer->buf_area);

    if(target_texture) {
        unsigned int framebuffer = get_framebuffer(u);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0));
    }

    lv_opengles_viewport(0, 0, targ_tex_w, targ_tex_h);
    lv_area_move(&t->clip_area, -dest_layer->buf_area.x1, -dest_layer->buf_area.y1);
    /*Translate blit_area from screen coordinates to dest_layer coordinates.*/
    lv_area_t render_area = *blit_area;
    lv_area_move(&render_area, -dest_layer->buf_area.x1, -dest_layer->buf_area.y1);

    if(opa != LV_OPA_COVER) {
        GL_CALL(glEnable(GL_BLEND));
        GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
    }

    lv_opengles_render_texture(texture, &render_area, opa, targ_tex_w, targ_tex_h, &t->clip_area, h_flip, v_flip);

    if(opa != LV_OPA_COVER) {
        GL_CALL(glDisable(GL_BLEND));
    }

    if(target_texture) {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }
}

static void draw_to_framebuffer(lv_draw_opengles_unit_t * u)
{
    lv_area_t draw_area;
    unsigned int texture = draw_to_texture(u, NULL, &draw_area);
    if(texture == 0) {
        /*Texture creation failed; nothing to render to the framebuffer.*/
        return;
    }
    /*Use LV_OPA_COVER to ensure the texture is drawn to the framebuffer without any blending.*/
    draw_texture_to_framebuffer(u, texture, LV_OPA_COVER, &draw_area);
    GL_CALL(glDeleteTextures(1, &texture));
}

static lv_area_t get_blit_area(const lv_draw_task_t * task)
{
    /*For images with a valid real_slice_area the cached texture covers only that slice,
     *not the full _real_area. After `draw_from_cached_texture` has offset the descriptor for the
     *cache key, `real_slice_area` is relative to `task->area`; convert back to absolute for the blit.
     *Otherwise return `_real_area` (already absolute).*/
    if(task->type == LV_DRAW_TASK_TYPE_IMAGE) {
        const lv_draw_image_dsc_t * img_dsc = (const lv_draw_image_dsc_t *)task->draw_dsc;
        if(img_dsc->real_slice_area.x2 != LV_COORD_MIN) {
            lv_area_t abs_slice_area = img_dsc->real_slice_area;
            lv_area_move(&abs_slice_area, task->area.x1, task->area.y1);
            return abs_slice_area;
        }
    }

    return task->_real_area;
}

static bool is_task_for_dynamic_part(const lv_draw_task_t * task)
{
    if(task->type == LV_DRAW_TASK_TYPE_LABEL) {
        return true;
    }

    const lv_draw_dsc_base_t * base_dsc = (const lv_draw_dsc_base_t *)task->draw_dsc;
    if(base_dsc->obj == NULL) {
        return false;
    }

    return base_dsc->part == LV_PART_INDICATOR || base_dsc->part == LV_PART_SCROLLBAR;
}

static lv_cache_entry_t * acquire_or_create_cache_entry(lv_draw_opengles_unit_t * u, cache_data_t * data_to_find)
{
    /*For dynamic parts, returns the existing entry if the draw descriptor is the same;
     *otherwise, replaces it with the new one.*/
    if(data_to_find->obj != NULL) {
        lv_cache_entry_t * entry_cached = lv_cache_acquire(u->texture_cache, data_to_find, u);
        if(entry_cached) {
            cache_data_t * data_cached = lv_cache_entry_get_data(entry_cached);
            if(compare_static_part(data_to_find, data_cached) == 0) {
                return entry_cached;
            }
            
            MY_LOG("Replacing cached texture %d of size %d x %d for task type %s, obj %p, part %s",
                   data_cached->texture, data_cached->w, data_cached->h,
                   task_type_to_string(data_cached->task_type),
                   data_cached->obj, part_to_string(data_cached->part));

            lv_cache_release(u->texture_cache, entry_cached, u);
            lv_cache_drop(u->texture_cache, data_to_find, u);
        }
    }

    return lv_cache_acquire_or_create(u->texture_cache, data_to_find, u);
}

static void draw_from_cached_texture(lv_draw_task_t * t)
{
    LV_PROFILER_DRAW_BEGIN;
    lv_draw_opengles_unit_t * u = (lv_draw_opengles_unit_t *)t->draw_unit;
    lv_draw_dsc_base_t * base_dsc = (lv_draw_dsc_base_t *)t->draw_dsc;

    cache_data_t data_to_find;
    lv_memset(&data_to_find, 0, sizeof(data_to_find));
    if(is_task_for_dynamic_part(t)) {
        data_to_find.obj = base_dsc->obj;
        data_to_find.part = base_dsc->part;
    }
    data_to_find.task_type = t->type;
    data_to_find.draw_dsc = base_dsc;
    lv_area_t texture_area = get_texture_area(t);
    data_to_find.w = lv_area_get_width(&texture_area);
    data_to_find.h = lv_area_get_height(&texture_area);
    if(data_to_find.w <= 0 || data_to_find.h <= 0) {
        /*An empty area: nothing to draw, and nothing to cache*/
        LV_PROFILER_DRAW_END;
        return;
    }
    data_to_find.slot.size = data_to_find.w * data_to_find.h * 4;

    /*user_data stores the renderer to differentiate it from SW rendered tasks.
     *However the cached texture is independent from the renderer so use NULL user_data*/
    void * user_data_saved = data_to_find.draw_dsc->user_data;
    data_to_find.draw_dsc->user_data = NULL;

    /*img_dsc->image_area is an absolute coordinate so it's different
     *for the same image on a different position. So make it relative before using for cache. */
    lv_area_t a = t->area;
    if(t->type == LV_DRAW_TASK_TYPE_IMAGE) {
        lv_draw_image_dsc_t * img_dsc = (lv_draw_image_dsc_t *)data_to_find.draw_dsc;
        lv_area_move(&img_dsc->image_area, -t->area.x1, -t->area.y1);

        if(img_dsc->real_slice_area.x2 != LV_COORD_MIN) {
            /*Same reason as img_dsc->image_area above: make img_dsc->real_slice_area relative
             *before using for cache*/
            lv_area_move(&img_dsc->real_slice_area, -t->area.x1, -t->area.y1);
        }
    }
    else if(t->type == LV_DRAW_TASK_TYPE_TRIANGLE) {
        lv_draw_triangle_dsc_t * tri_dsc = (lv_draw_triangle_dsc_t *)data_to_find.draw_dsc;
        tri_dsc->p[0].x -= t->area.x1;
        tri_dsc->p[0].y -= t->area.y1;
        tri_dsc->p[1].x -= t->area.x1;
        tri_dsc->p[1].y -= t->area.y1;
        tri_dsc->p[2].x -= t->area.x1;
        tri_dsc->p[2].y -= t->area.y1;
    }
    else if(t->type == LV_DRAW_TASK_TYPE_LINE) {
        lv_draw_line_dsc_t * line_dsc = (lv_draw_line_dsc_t *)data_to_find.draw_dsc;
        line_dsc->p1.x -= t->area.x1;
        line_dsc->p1.y -= t->area.y1;
        line_dsc->p2.x -= t->area.x1;
        line_dsc->p2.y -= t->area.y1;
    }
    else if(t->type == LV_DRAW_TASK_TYPE_ARC) {
        lv_draw_arc_dsc_t * arc_dsc = (lv_draw_arc_dsc_t *)data_to_find.draw_dsc;
        arc_dsc->center.x -= t->area.x1;
        arc_dsc->center.y -= t->area.y1;
    }

    lv_area_move(&t->area, -a.x1, -a.y1);
    lv_area_move(&t->_real_area, -a.x1, -a.y1);
    lv_opa_t orig_opa = replace_opa_in_task(t, LV_OPA_COVER);

    lv_cache_entry_t * entry_cached = acquire_or_create_cache_entry(u, &data_to_find);

    lv_area_move(&t->area, a.x1, a.y1);
    lv_area_move(&t->_real_area, a.x1, a.y1);
    replace_opa_in_task(t, orig_opa);

    data_to_find.draw_dsc->user_data = user_data_saved;

    if(!entry_cached) {
        LV_PROFILER_DRAW_END;
        return;
    }

    cache_data_t * data_cached = lv_cache_entry_get_data(entry_cached);
    unsigned int texture = data_cached->texture;
    lv_area_t blit_area = get_blit_area(t);
    draw_texture_to_framebuffer(u, texture, orig_opa, &blit_area);

    lv_cache_release(u->texture_cache, entry_cached, u);

    LV_PROFILER_DRAW_END;
}

static void execute_drawing(lv_draw_opengles_unit_t * u)
{
    lv_draw_task_t * t = u->task_act;
    t->draw_unit = (lv_draw_unit_t *)u;

    if(u->max_texture_size == 0) {
        GL_CALL(glGetIntegerv(GL_MAX_TEXTURE_SIZE, &u->max_texture_size));
    }

    /* the shader-based fill is not working reliably with EGL. */
    switch(t->type) {
        case LV_DRAW_TASK_TYPE_FILL: {
                lv_draw_fill_dsc_t * fill_dsc = t->draw_dsc;
                if(fill_dsc->radius == 0 && fill_dsc->grad.dir == LV_GRAD_DIR_NONE) {
                    lv_layer_t * layer = t->target_layer;
                    lv_area_t fill_area = t->area;
                    lv_area_intersect(&fill_area, &fill_area, &t->clip_area);
                    lv_area_move(&fill_area, -layer->buf_area.x1, -layer->buf_area.y1);

                    unsigned int target_texture = layer_get_texture(layer);
                    int32_t targ_tex_w = lv_area_get_width(&layer->buf_area);
                    int32_t targ_tex_h = lv_area_get_height(&layer->buf_area);

                    if(target_texture) {
                        unsigned int framebuffer = get_framebuffer(u);
                        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
                        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0));
                    }

                    if(fill_dsc->opa >= LV_OPA_MAX) {
                        float tex_w = (float)lv_area_get_width(&fill_area);
                        float tex_h = (float)lv_area_get_height(&fill_area);
                        GL_CALL(glEnable(GL_SCISSOR_TEST));
                        GL_CALL(glScissor(fill_area.x1, targ_tex_h - fill_area.y1 - tex_h, tex_w, tex_h));
                        /* swap red and blue channels here as they will be swapped back during flushing*/
                        GL_CALL(glClearColor((float)fill_dsc->color.blue / 255.0f, (float)fill_dsc->color.green / 255.0f,
                                             (float)fill_dsc->color.red / 255.0f, 1.0f));
                        GL_CALL(glClearDepthf(1.0f));
                        GL_CALL(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
                        GL_CALL(glDisable(GL_SCISSOR_TEST));
                    }
                    else {
                        lv_opengles_viewport(0, 0, targ_tex_w, targ_tex_h);
                        lv_opengles_render_fill(fill_dsc->color, &fill_area, fill_dsc->opa, targ_tex_w, targ_tex_h);
                    }

                    if(target_texture) {
                        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
                    }

                    return;
                }
                break;
            }
        case LV_DRAW_TASK_TYPE_IMAGE: {
                /*Do not cache modifiable images as they might change in the next frame
                 *resulting in stale textures in the cache. */
                lv_draw_image_dsc_t * img_dsc = (lv_draw_image_dsc_t *)t->draw_dsc;
                if(img_dsc->header.flags & LV_IMAGE_FLAGS_MODIFIABLE) {
                    draw_to_framebuffer(u);
                    return;
                }
                break;
            }
        case LV_DRAW_TASK_TYPE_LINE: {
                /*Do not cache lines rendered from points as dsc->points will be freed*/
                lv_draw_line_dsc_t * line_dsc = t->draw_dsc;
                if(line_dsc->points) {
                    draw_to_framebuffer(u);
                    return;
                }
                break;
            }
        case LV_DRAW_TASK_TYPE_LAYER: {
                blend_texture_layer(t);
                return;
            }
#if LV_USE_3DTEXTURE
        case LV_DRAW_TASK_TYPE_3D: {
                lv_draw_opengles_3d(t, t->draw_dsc, &t->area);
                return;
            }
#endif
        default:
            break;
    }

    /*Tasks whose real area exceeds the GL texture size limit cannot be stored in a
     *single texture. Skip the cache and render directly using the clipped area.*/
    if(lv_area_get_width(&t->_real_area) > u->max_texture_size ||
       lv_area_get_height(&t->_real_area) > u->max_texture_size) {
        draw_to_framebuffer(u);
        return;
    }

    draw_from_cached_texture(t);
}

static unsigned int layer_get_texture(lv_layer_t * layer)
{
    return (unsigned int)(uintptr_t)layer->user_data;
}

static unsigned int get_framebuffer(lv_draw_opengles_unit_t * u)
{
    if(u->framebuffer == 0) {
        GL_CALL(glGenFramebuffers(1, &u->framebuffer));
    }
    return u->framebuffer;
}

static unsigned int create_texture(int32_t w, int32_t h, const void * data)
{
    LV_PROFILER_DRAW_BEGIN;
    unsigned int texture;
    GL_CALL(glGenTextures(1, &texture));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, texture));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GL_CALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));

    /* LV_COLOR_DEPTH 32, 16 are supported but the cached textures will always
     * have full ARGB pixels since the alpha channel is required for blending.
     */
    GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data));
#if 0
    GL_CALL(glGenerateMipmap(GL_TEXTURE_2D));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 20));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    /* GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST));
     * Alternatively, the above form can be used in some cases for slightly faster performance, but
     * visual quality when using image scales that are not exactly 1:1 (or 2:1 or some other increment)
     * will be not as good.
     */
#endif

    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));

    LV_PROFILER_DRAW_END;
    return texture;
}

#if LV_USE_3DTEXTURE
static void lv_draw_opengles_3d(lv_draw_task_t * t, const lv_draw_3d_dsc_t * dsc, const lv_area_t * coords)
{
    LV_PROFILER_DRAW_BEGIN;
    lv_draw_opengles_unit_t * u = (lv_draw_opengles_unit_t *) t->draw_unit;

    lv_layer_t * dest_layer = t->target_layer;
    unsigned int target_texture = layer_get_texture(dest_layer);
    int32_t targ_tex_w = lv_area_get_width(&dest_layer->buf_area);
    int32_t targ_tex_h = lv_area_get_height(&dest_layer->buf_area);

    if(target_texture) {
        unsigned int framebuffer = get_framebuffer(u);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0));
    }

    lv_opengles_viewport(0, 0, targ_tex_w, targ_tex_h);
    lv_area_t clip_area = t->clip_area;
    lv_area_move(&clip_area, -dest_layer->buf_area.x1, -dest_layer->buf_area.y1);

    lv_opengles_render_params_t params;
    lv_opengles_render_params_init(&params);
    params.texture = dsc->tex_id;
    params.texture_area = coords;
    params.opa = dsc->opa;
    params.disp_w = targ_tex_w;
    params.disp_h = targ_tex_h;
    params.texture_clip_area = &clip_area;
    params.h_flip = dsc->h_flip;
    params.v_flip = dsc->v_flip;
    params.blend_opt = true;
    lv_opengles_render(&params);

    if(target_texture) {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }
    LV_PROFILER_DRAW_END;
}
#endif /*LV_USE_3DTEXTURE*/

#if USE_MY_LOG

static const char * task_type_to_string(lv_draw_task_type_t type)
{
    switch(type) {
        case LV_DRAW_TASK_TYPE_NONE:
            return "NONE";
        case LV_DRAW_TASK_TYPE_FILL:
            return "FILL";
        case LV_DRAW_TASK_TYPE_BORDER:
            return "BORDER";
        case LV_DRAW_TASK_TYPE_BOX_SHADOW:
            return "BOX_SHADOW";
        case LV_DRAW_TASK_TYPE_LETTER:
            return "LETTER";
        case LV_DRAW_TASK_TYPE_LABEL:
            return "LABEL";
        case LV_DRAW_TASK_TYPE_IMAGE:
            return "IMAGE";
        case LV_DRAW_TASK_TYPE_LAYER:
            return "LAYER";
        case LV_DRAW_TASK_TYPE_LINE:
            return "LINE";
        case LV_DRAW_TASK_TYPE_ARC:
            return "ARC";
        case LV_DRAW_TASK_TYPE_TRIANGLE:
            return "TRIANGLE";
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE:
            return "MASK_RECTANGLE";
        case LV_DRAW_TASK_TYPE_MASK_BITMAP:
            return "MASK_BITMAP";
        case LV_DRAW_TASK_TYPE_BLUR:
            return "BLUR";
#if LV_USE_VECTOR_GRAPHIC
        case LV_DRAW_TASK_TYPE_VECTOR:
            return "VECTOR";
#endif
#if LV_USE_3DTEXTURE
        case LV_DRAW_TASK_TYPE_3D:
            return "3D";
#endif
        default:
            return "UNKNOWN";
    }
}

static const char * part_to_string(lv_part_t part)
{
    switch(part) {
        case LV_PART_MAIN:
            return "MAIN";
        case LV_PART_SCROLLBAR:
            return "SCROLLBAR";
        case LV_PART_INDICATOR:
            return "INDICATOR";
        case LV_PART_KNOB:
            return "KNOB";
        case LV_PART_SELECTED:
            return "SELECTED";
        case LV_PART_ITEMS:
            return "ITEMS";
        case LV_PART_CURSOR:
            return "CURSOR";
        case LV_PART_ANY:
            return "ANY";
        default:
            if(part >= LV_PART_CUSTOM_FIRST && part < LV_PART_ANY) {
                return "CUSTOM";
            }
            return "UNKNOWN";
    }
}

#endif /*USE_MY_LOG*/

#endif /*LV_USE_DRAW_OPENGLES*/
