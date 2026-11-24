/**
 * @file lv_slider.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_slider_private.h"

#if LV_USE_SLIDER

#include "../../misc/lv_area_private.h"
#include "../../core/lv_obj_private.h"
#include "../../core/lv_obj_event_private.h"
#include "../../core/lv_obj_class_private.h"
#include "../../lvgl_public.h"
#include "../../indev/lv_indev_private.h"
#include "../../core/lv_observer_private.h"
#include "../../core/lv_obj_style_internal.h"

/*Check dependencies*/
#if LV_USE_BAR == 0
    #error "lv_slider: lv_bar is required. Enable it in lv_conf.h (LV_USE_BAR 1)"
#endif

/*********************
 *      DEFINES
 *********************/
#define MY_CLASS (&lv_slider_class)

/*The edge of the indicator where the knob is, as the coordinate of the boundary between pixels
 *(e.g. `x2 + 1`), so that it is the start of the track with an empty indicator and its end with
 *a full one (see `position_knob()`)*/
#define LV_SLIDER_KNOB_COORD(is_reversed, area) (is_reversed ? area.x1 : area.x2 + 1)
#define LV_SLIDER_KNOB_COORD_VERTICAL(is_reversed, area) (is_reversed ? area.y2 + 1 : area.y1)

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lv_slider_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void lv_slider_event(const lv_obj_class_t * class_p, lv_event_t * e);
static void position_knob(lv_obj_t * obj, lv_area_t * knob_area, const int32_t knob_size, const bool hor);
static void draw_knob(lv_event_t * e);
static bool is_slider_horizontal(lv_obj_t * obj);
static void drag_start(lv_obj_t * obj);
static void update_knob_pos(lv_obj_t * obj, bool check_drag);
static void get_track(lv_obj_t * obj, bool hor, int32_t * start, int32_t * len);
static int32_t get_knob_len(lv_obj_t * obj, bool hor);
static int32_t knob_center_to_indic_edge(lv_obj_t * obj, bool hor, int32_t center);

#if LV_USE_OBSERVER
    static void slider_value_changed_event_cb(lv_event_t * e);
    static void slider_value_observer_cb(lv_observer_t * observer, lv_subject_t * subject);
#endif /*LV_USE_OBSERVER*/

/**********************
 *  STATIC VARIABLES
 **********************/

#if LV_USE_OBJ_PROPERTY
static const lv_property_ops_t lv_slider_properties[] = {
    {
        .id = LV_PROPERTY_SLIDER_VALUE,
        .setter = lv_slider_set_value,
        .getter = lv_slider_get_value,
    },
    {
        .id = LV_PROPERTY_SLIDER_LEFT_VALUE,
        .setter = lv_slider_set_start_value,
        .getter = lv_slider_get_left_value,
    },
    {
        .id = LV_PROPERTY_SLIDER_RANGE,
        .setter = lv_slider_set_range,
        .getter = NULL,
    },
    {
        .id = LV_PROPERTY_SLIDER_MIN_VALUE,
        .setter = lv_slider_set_min_value,
        .getter = lv_slider_get_min_value,
    },
    {
        .id = LV_PROPERTY_SLIDER_MAX_VALUE,
        .setter = lv_slider_set_max_value,
        .getter = lv_slider_get_max_value,
    },
    {
        .id = LV_PROPERTY_SLIDER_MODE,
        .setter = lv_slider_set_mode,
        .getter = lv_slider_get_mode,
    },
    {
        .id = LV_PROPERTY_SLIDER_IS_DRAGGED,
        .setter = NULL,
        .getter = lv_slider_is_dragged,
    },
    {
        .id = LV_PROPERTY_SLIDER_IS_SYMMETRICAL,
        .setter = NULL,
        .getter = lv_slider_is_symmetrical,
    },
};
#endif

const lv_obj_class_t lv_slider_class = {
    .constructor_cb = lv_slider_constructor,
    .event_cb = lv_slider_event,
    .editable = LV_OBJ_CLASS_EDITABLE_TRUE,
    .group_def = LV_OBJ_CLASS_GROUP_DEF_TRUE,
    .instance_size = sizeof(lv_slider_t),
    .base_class = &lv_bar_class,
    .name = "lv_slider",
    LV_PROPERTY_CLASS_FIELDS(slider, SLIDER)
};

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * lv_slider_create(lv_obj_t * parent)
{
    LV_LOG_INFO("begin");
    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

bool lv_slider_is_dragged(const lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return false);
    lv_slider_t * slider = (lv_slider_t *)obj;

    return slider->dragging;
}

void lv_slider_set_value(lv_obj_t * obj, int32_t value, lv_anim_enable_t anim)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return);

    lv_bar_set_value(obj, value, anim);
}

void lv_slider_set_start_value(lv_obj_t * obj, int32_t value, lv_anim_enable_t anim)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return);

    lv_bar_set_start_value(obj, value, anim);
}

void lv_slider_set_range(lv_obj_t * obj, int32_t min, int32_t max)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return);

    lv_bar_set_range(obj, min, max);
}

void lv_slider_set_min_value(lv_obj_t * obj, int32_t min)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return);

    lv_bar_set_min_value(obj, min);
}

void lv_slider_set_max_value(lv_obj_t * obj, int32_t max)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return);

    lv_bar_set_max_value(obj, max);
}

void lv_slider_set_mode(lv_obj_t * obj, lv_slider_mode_t mode)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return);

    lv_bar_set_mode(obj, (lv_bar_mode_t)mode);
}

void lv_slider_set_orientation(lv_obj_t * obj, lv_slider_orientation_t orientation)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return);

    lv_bar_set_orientation(obj, (lv_bar_orientation_t)orientation);
}

int32_t lv_slider_get_value(const lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return 0);

    return lv_bar_get_value(obj);
}

int32_t lv_slider_get_left_value(const lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return 0);

    return lv_bar_get_start_value(obj);
}

int32_t lv_slider_get_min_value(const lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return 0);

    return lv_bar_get_min_value(obj);
}

int32_t lv_slider_get_max_value(const lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return 0);

    return lv_bar_get_max_value(obj);
}

lv_slider_mode_t lv_slider_get_mode(lv_obj_t * slider)
{
    LV_CHECK_OBJ(slider, MY_CLASS, return LV_SLIDER_MODE_NORMAL);

    lv_bar_mode_t mode = lv_bar_get_mode(slider);
    if(mode == LV_BAR_MODE_SYMMETRICAL) return LV_SLIDER_MODE_SYMMETRICAL;
    else if(mode == LV_BAR_MODE_RANGE) return LV_SLIDER_MODE_RANGE;
    else return LV_SLIDER_MODE_NORMAL;
}

lv_slider_orientation_t lv_slider_get_orientation(lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return LV_SLIDER_ORIENTATION_AUTO);

    lv_bar_orientation_t ori = lv_bar_get_orientation(obj);
    if(ori == LV_BAR_ORIENTATION_HORIZONTAL) return LV_SLIDER_ORIENTATION_HORIZONTAL;
    else if(ori == LV_BAR_ORIENTATION_VERTICAL) return LV_SLIDER_ORIENTATION_VERTICAL;
    else return LV_SLIDER_ORIENTATION_AUTO;
}

bool lv_slider_is_symmetrical(lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return false);

    return lv_bar_is_symmetrical(obj);
}

#if LV_USE_OBSERVER
lv_observer_t * lv_slider_bind_value(lv_obj_t * obj, lv_subject_t * subject)
{
    LV_CHECK_OBJ(obj, MY_CLASS, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(
        subject->type == LV_SUBJECT_TYPE_INT || subject->type == LV_SUBJECT_TYPE_FLOAT,
        return NULL,
        "Incompatible subject type: %d", subject->type);

    lv_obj_add_event_cb(obj, slider_value_changed_event_cb, LV_EVENT_VALUE_CHANGED, subject);
    lv_observer_t * observer = lv_subject_add_observer_obj(subject, slider_value_observer_cb, obj, NULL);
    return observer;
}
#endif /*LV_USE_OBSERVER*/


/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_slider_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);
    LV_ASSERT(obj != NULL);
    lv_slider_t * slider = (lv_slider_t *)obj;

    /*Initialize the allocated 'slider'*/
    slider->value_to_set = NULL;
    slider->dragging = 0U;
    slider->left_knob_focus = 0U;

    lv_obj_set_scroll_chain_hor(obj, false);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_scroll_on_focus(obj, true);
    lv_obj_set_ext_click_area(obj, LV_DPX(8));
}

static void lv_slider_event(const lv_obj_class_t * class_p, lv_event_t * e)
{
    LV_UNUSED(class_p);
    LV_ASSERT(e != NULL);

    lv_result_t res;

    /*Call the ancestor's event handler*/
    res = lv_obj_event_base(MY_CLASS, e);
    if(res != LV_RESULT_OK) return;

    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_current_target(e);
    LV_ASSERT(obj != NULL);
    lv_slider_t * slider = (lv_slider_t *)obj;
    lv_slider_mode_t type = lv_slider_get_mode(obj);

    /*Advanced hit testing: react only on dragging the knob(s)*/
    if(code == LV_EVENT_HIT_TEST) {
        lv_hit_test_info_t * info = lv_event_get_param(e);
        int32_t ext_click_area = obj->spec_attr ? obj->spec_attr->ext_click_pad : 0;

        /*Ordinary slider: was the knob area hit?*/
        lv_area_t a;
        a = slider->right_knob_area;
        lv_area_increase(&a, ext_click_area, ext_click_area);
        info->res = lv_area_is_point_on(&a, info->point, 0);

        /*There's still a chance that there is a hit if there is another knob*/
        if((info->res == false) && (type == LV_SLIDER_MODE_RANGE)) {
            a = slider->left_knob_area;
            lv_area_increase(&a, ext_click_area, ext_click_area);
            info->res = lv_area_is_point_on(&a, info->point, 0);
        }
    }
    else if(code == LV_EVENT_PRESSED) {
        /*Save the pressed coordinates*/
        lv_indev_t * indev = lv_event_get_indev(e);
        slider->pressed_point.x = 0;
        slider->pressed_point.y = 0;
        if(indev != NULL) lv_indev_get_point(indev, &slider->pressed_point);
        lv_obj_transform_point(obj, &slider->pressed_point, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE_RECURSIVE);
    }
    else if(code == LV_EVENT_PRESSING) {
        update_knob_pos(obj, true);
    }
    else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        update_knob_pos(obj, false);
        slider->dragging = false;
        slider->value_to_set = NULL;

        lv_obj_invalidate(obj);

        /*Leave edit mode if released. (No need to wait for LONG_PRESS)*/
        lv_group_t * g   = lv_obj_get_group(obj);
        bool editing     = g != NULL && lv_group_get_editing(g);
        lv_indev_t * indev = lv_event_get_indev(e);
        lv_indev_type_t indev_type = indev != NULL ? lv_indev_get_type(indev) : LV_INDEV_TYPE_NONE;
        if(indev_type == LV_INDEV_TYPE_ENCODER) {
            if(editing) {
                if(lv_slider_get_mode(obj) == LV_SLIDER_MODE_RANGE) {
                    if(slider->left_knob_focus == 0) slider->left_knob_focus = 1;
                    else {
                        slider->left_knob_focus = 0;
                        lv_group_set_editing(g, false);
                    }
                }
                else {
                    lv_group_set_editing(g, false);
                }
            }
        }
        else if(indev_type == LV_INDEV_TYPE_POINTER) {
            if(is_slider_horizontal(obj)) lv_obj_set_scroll_chain_ver(obj, true);
            else  lv_obj_set_scroll_chain_hor(obj, true);
        }
    }
    else if(code == LV_EVENT_FOCUSED) {
        lv_indev_t * indev = lv_indev_active();
        lv_indev_type_t indev_type = indev != NULL ? lv_indev_get_type(indev) : LV_INDEV_TYPE_NONE;
        if(indev_type == LV_INDEV_TYPE_ENCODER || indev_type == LV_INDEV_TYPE_KEYPAD) {
            slider->left_knob_focus = 0;
        }
    }
    else if(code == LV_EVENT_SIZE_CHANGED) {
        if(is_slider_horizontal(obj)) {
            lv_obj_set_scroll_chain_ver(obj, true);
            lv_obj_set_scroll_chain_hor(obj, false);
        }
        else {
            lv_obj_set_scroll_chain_hor(obj, true);
            lv_obj_set_scroll_chain_ver(obj, false);
        }
        lv_obj_refresh_ext_draw_size(obj);
    }
    else if(code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        int32_t knob_left = lv_obj_get_style_pad_left_internal(obj, LV_PART_KNOB);
        int32_t knob_right = lv_obj_get_style_pad_right_internal(obj, LV_PART_KNOB);
        int32_t knob_top = lv_obj_get_style_pad_top_internal(obj, LV_PART_KNOB);
        int32_t knob_bottom = lv_obj_get_style_pad_bottom_internal(obj, LV_PART_KNOB);

        /*The smaller size is the knob diameter*/
        int32_t trans_w = lv_obj_get_style_transform_width_internal(obj, LV_PART_KNOB);
        int32_t trans_h = lv_obj_get_style_transform_height_internal(obj, LV_PART_KNOB);
        int32_t knob_size = LV_MIN(lv_obj_get_width(obj) + 2 * trans_w, lv_obj_get_height(obj) + 2 * trans_h) >> 1;
        knob_size += LV_MAX(LV_MAX(knob_left, knob_right), LV_MAX(knob_bottom, knob_top));
        knob_size += 2;         /*For rounding error*/
        knob_size += lv_obj_calculate_ext_draw_size(obj, LV_PART_KNOB);

        /*Indic. size is handled by bar*/
        int32_t * s = lv_event_get_param(e);
        *s  = LV_MAX(*s, knob_size);

    }
    else if(code == LV_EVENT_KEY) {
        uint32_t c = lv_event_get_key(e);

        if(c == LV_KEY_RIGHT || c == LV_KEY_UP) {
            if(!slider->left_knob_focus) lv_slider_set_value(obj, lv_slider_get_value(obj) + 1, LV_ANIM_ON);
            else lv_slider_set_start_value(obj, lv_slider_get_left_value(obj) + 1, LV_ANIM_ON);
        }
        else if(c == LV_KEY_LEFT || c == LV_KEY_DOWN) {
            if(!slider->left_knob_focus) lv_slider_set_value(obj, lv_slider_get_value(obj) - 1, LV_ANIM_ON);
            else lv_slider_set_start_value(obj, lv_slider_get_left_value(obj) - 1, LV_ANIM_ON);
        }
        else {
            return;
        }

        res = lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
        if(res != LV_RESULT_OK) return;
    }
    else if(code == LV_EVENT_ROTARY) {
        int32_t r = lv_event_get_rotary_diff(e);

        if(!slider->left_knob_focus) lv_slider_set_value(obj, lv_slider_get_value(obj) + r, LV_ANIM_ON);
        else lv_slider_set_start_value(obj, lv_slider_get_left_value(obj) + 1, LV_ANIM_ON);

        res = lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
        if(res != LV_RESULT_OK) return;
    }
    else if(code == LV_EVENT_DRAW_MAIN) {
        draw_knob(e);
    }
}

static void draw_knob(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    lv_obj_t * obj = lv_event_get_current_target(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    LV_ASSERT(obj != NULL);
    LV_ASSERT(layer != NULL);
    lv_slider_t * slider = (lv_slider_t *)obj;

    const bool is_rtl = LV_BASE_DIR_RTL == lv_obj_get_style_base_dir_internal(obj, LV_PART_MAIN);
    const bool is_horizontal = is_slider_horizontal(obj);
    const bool is_reversed = slider->bar.val_reversed ^ (is_rtl && is_horizontal);

    lv_area_t knob_area;
    int32_t knob_size;
    bool is_symmetrical = lv_slider_is_symmetrical(obj);

    /*In symmetrical mode with a negative value the value's end is the indicator's other edge,
     *which the bar sets to the last pixel of the value: move it 1 pixel to the boundary*/
    if(is_horizontal) {
        knob_size = lv_obj_get_height(obj);
        if(is_symmetrical &&
           slider->bar.cur_value < 0) knob_area.x1 = LV_SLIDER_KNOB_COORD(!is_reversed, slider->bar.indic_area) + (is_reversed ? -1 : 1);
        else knob_area.x1 = LV_SLIDER_KNOB_COORD(is_reversed, slider->bar.indic_area);
    }
    else {
        knob_size = lv_obj_get_width(obj);
        if(is_symmetrical &&
           slider->bar.cur_value < 0) knob_area.y1 =  LV_SLIDER_KNOB_COORD_VERTICAL(!is_reversed, slider->bar.indic_area) + (is_reversed ? 1 : -1);
        else knob_area.y1 = LV_SLIDER_KNOB_COORD_VERTICAL(is_reversed, slider->bar.indic_area);
    }
    lv_draw_rect_dsc_t knob_rect_dsc;
    lv_draw_rect_dsc_init(&knob_rect_dsc);
    knob_rect_dsc.base.layer = layer;
    lv_obj_init_draw_rect_dsc(obj, LV_PART_KNOB, &knob_rect_dsc);
    /* Update knob area with knob style */
    position_knob(obj, &knob_area, knob_size, is_horizontal);
    /* Update right knob area with calculated knob area */
    slider->right_knob_area = knob_area;

    if(lv_slider_get_mode(obj) != LV_SLIDER_MODE_RANGE) {
        lv_draw_rect(layer, &knob_rect_dsc, &slider->right_knob_area);
    }
    else {
        /*Save the draw part_draw_dsc. because it can be modified in the event*/
        lv_draw_rect_dsc_t knob_rect_dsc_tmp;
        lv_memcpy(&knob_rect_dsc_tmp, &knob_rect_dsc, sizeof(lv_draw_rect_dsc_t));
        /* Draw the right knob */
        lv_draw_rect(layer, &knob_rect_dsc, &slider->right_knob_area);

        /*Calculate the second knob area*/
        if(is_horizontal) {
            /*use !is_reversed to get the other knob*/
            knob_area.x1 = LV_SLIDER_KNOB_COORD(!is_reversed, slider->bar.indic_area);
        }
        else {
            knob_area.y1 = LV_SLIDER_KNOB_COORD_VERTICAL(!is_reversed, slider->bar.indic_area);
        }
        position_knob(obj, &knob_area, knob_size, is_horizontal);
        slider->left_knob_area = knob_area;

        lv_memcpy(&knob_rect_dsc, &knob_rect_dsc_tmp, sizeof(lv_draw_rect_dsc_t));

        lv_draw_rect(layer, &knob_rect_dsc, &slider->left_knob_area);
    }
}

static void position_knob(lv_obj_t * obj, lv_area_t * knob_area, const int32_t knob_size, const bool hor)
{
    LV_ASSERT(obj != NULL);
    LV_ASSERT(knob_area != NULL);

    int32_t knob_left = lv_obj_get_style_pad_left_internal(obj, LV_PART_KNOB);
    int32_t knob_right = lv_obj_get_style_pad_right_internal(obj, LV_PART_KNOB);
    int32_t knob_top = lv_obj_get_style_pad_top_internal(obj, LV_PART_KNOB);
    int32_t knob_bottom = lv_obj_get_style_pad_bottom_internal(obj, LV_PART_KNOB);

    /*Keep the whole knob inside the track: it moves along the track's length minus its own, in
     *proportion to the position of the indicator's edge (the given coordinate) on the track.
     *So it touches the start of the track with an empty indicator, its end with a full one,
     *and always covers the indicator's edge.*/
    int32_t track_start;
    int32_t track_len;
    get_track(obj, hor, &track_start, &track_len);
    int32_t knob_len = knob_size + (hor ? knob_left + knob_right : knob_top + knob_bottom);
    int32_t travel = track_len - knob_len;
    int32_t edge = hor ? knob_area->x1 : knob_area->y1;
    int32_t ofs;
    if(travel <= 0) {
        ofs = travel / 2; /*Not shorter than the track: center it*/
    }
    else {
        edge = LV_CLAMP(track_start, edge, track_start + track_len);
        ofs = (int32_t)(((int64_t)(edge - track_start) * travel + track_len / 2) / track_len);
    }

    if(hor) {
        knob_area->x1 = track_start + ofs;
        knob_area->x2 = knob_area->x1 + knob_len - 1;
        knob_area->y1 = obj->coords.y1 - knob_top;
        knob_area->y2 = obj->coords.y2 + knob_bottom;
    }
    else {
        knob_area->y1 = track_start + ofs;
        knob_area->y2 = knob_area->y1 + knob_len - 1;
        knob_area->x1 = obj->coords.x1 - knob_left;
        knob_area->x2 = obj->coords.x2 + knob_right;
    }

    int32_t transf_w = lv_obj_get_style_transform_width_internal(obj, LV_PART_KNOB);
    int32_t transf_h = lv_obj_get_style_transform_height_internal(obj, LV_PART_KNOB);

    /*Apply the transformation on the knob area*/
    knob_area->x1 -= transf_w;
    knob_area->x2 += transf_w;
    knob_area->y1 -= transf_h;
    knob_area->y2 += transf_h;
}

/**
 * Get the track of the indicator along the slider: the slider's area, transformed and without its
 * padding, as the bar computes it to draw the indicator.
 * @param obj       pointer to a slider
 * @param hor       true if the slider is horizontal
 * @param start     store the first coordinate of the track here
 * @param len       store the length of the track here
 */
static void get_track(lv_obj_t * obj, bool hor, int32_t * start, int32_t * len)
{
    if(hor) {
        int32_t transf_w = lv_obj_get_style_transform_width_internal(obj, LV_PART_MAIN);
        *start = obj->coords.x1 - transf_w + lv_obj_get_style_pad_left_internal(obj, LV_PART_MAIN);
        *len = lv_obj_get_width(obj) + 2 * transf_w - lv_obj_get_style_pad_left_internal(obj, LV_PART_MAIN) -
               lv_obj_get_style_pad_right_internal(obj, LV_PART_MAIN);
    }
    else {
        int32_t transf_h = lv_obj_get_style_transform_height_internal(obj, LV_PART_MAIN);
        *start = obj->coords.y1 - transf_h + lv_obj_get_style_pad_top_internal(obj, LV_PART_MAIN);
        *len = lv_obj_get_height(obj) + 2 * transf_h - lv_obj_get_style_pad_top_internal(obj, LV_PART_MAIN) -
               lv_obj_get_style_pad_bottom_internal(obj, LV_PART_MAIN);
    }
}

/**
 * Get the length of the knob along the slider, with its padding but not its transformation,
 * as `position_knob()` places it.
 * @param obj       pointer to a slider
 * @param hor       true if the slider is horizontal
 * @return          the length of the knob
 */
static int32_t get_knob_len(lv_obj_t * obj, bool hor)
{
    if(hor) {
        return lv_obj_get_height(obj) + lv_obj_get_style_pad_left_internal(obj, LV_PART_KNOB) +
               lv_obj_get_style_pad_right_internal(obj, LV_PART_KNOB);
    }
    else {
        return lv_obj_get_width(obj) + lv_obj_get_style_pad_top_internal(obj, LV_PART_KNOB) +
               lv_obj_get_style_pad_bottom_internal(obj, LV_PART_KNOB);
    }
}

/**
 * Convert the position of the knob's center to the position of the indicator's edge that puts
 * the knob there: the inverse of `position_knob()`.
 * @param obj       pointer to a slider
 * @param hor       true if the slider is horizontal
 * @param center    the coordinate of the knob's center along the slider
 * @return          the coordinate of the indicator's edge
 */
static int32_t knob_center_to_indic_edge(lv_obj_t * obj, bool hor, int32_t center)
{
    int32_t track_start;
    int32_t track_len;
    get_track(obj, hor, &track_start, &track_len);
    int32_t knob_len = get_knob_len(obj, hor);
    int32_t travel = track_len - knob_len;
    if(travel <= 0) return center; /*The knob can't move: keep the plain mapping*/

    int32_t ofs = LV_CLAMP(0, center - track_start - knob_len / 2, travel);
    return track_start + (int32_t)(((int64_t)ofs * track_len + travel / 2) / travel);
}

static bool is_slider_horizontal(lv_obj_t * obj)
{
    LV_ASSERT(obj != NULL);
    lv_slider_t * slider = (lv_slider_t *)obj;
    if(slider->bar.orientation == LV_BAR_ORIENTATION_AUTO) {
        return lv_obj_get_width(obj) >= lv_obj_get_height(obj);
    }
    return slider->bar.orientation == LV_BAR_ORIENTATION_HORIZONTAL;
}

static void drag_start(lv_obj_t * obj)
{
    LV_ASSERT(obj != NULL);
    lv_slider_t * slider = (lv_slider_t *)obj;
    lv_slider_mode_t mode = lv_slider_get_mode(obj);
    lv_point_t p = { 0, 0 };
    slider->dragging = true;
    if(mode == LV_SLIDER_MODE_NORMAL || mode == LV_SLIDER_MODE_SYMMETRICAL) {
        slider->value_to_set = &slider->bar.cur_value;
    }
    else if(mode == LV_SLIDER_MODE_RANGE) {
        lv_indev_t * indev = lv_indev_active();
        if(indev != NULL) lv_indev_get_point(indev, &p);
        lv_obj_transform_point(obj, &p, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE_RECURSIVE);
        const bool is_rtl = LV_BASE_DIR_RTL == lv_obj_get_style_base_dir_internal(obj, LV_PART_MAIN);
        const bool is_horizontal = is_slider_horizontal(obj);
        const bool is_reversed = slider->bar.val_reversed ^ (is_rtl && is_horizontal);
        int32_t dist_left, dist_right;
        if(is_horizontal) {
            if((!is_reversed && p.x > slider->right_knob_area.x2) || (is_reversed && p.x < slider->right_knob_area.x1)) {
                slider->value_to_set = &slider->bar.cur_value;
            }
            else if((!is_reversed && p.x < slider->left_knob_area.x1) || (is_reversed && p.x > slider->left_knob_area.x2)) {
                slider->value_to_set = &slider->bar.start_value;
            }
            else {
                /*Calculate the distance from each knob*/
                dist_left = LV_ABS((slider->left_knob_area.x1 + (slider->left_knob_area.x2 - slider->left_knob_area.x1) / 2) - p.x);
                dist_right = LV_ABS((slider->right_knob_area.x1 + (slider->right_knob_area.x2 - slider->right_knob_area.x1) / 2) - p.x);
                /*Use whichever one is closer*/
                if(dist_right < dist_left) {
                    slider->value_to_set = &slider->bar.cur_value;
                    slider->left_knob_focus = 0;
                }
                else {
                    slider->value_to_set = &slider->bar.start_value;
                    slider->left_knob_focus = 1;
                }
            }
        }
        else {
            if((!is_reversed && p.y < slider->right_knob_area.y1) || (is_reversed && p.y > slider->right_knob_area.y2)) {
                slider->value_to_set = &slider->bar.cur_value;
            }
            else if((!is_reversed && p.y > slider->left_knob_area.y2) || (is_reversed && p.y < slider->left_knob_area.y1)) {
                slider->value_to_set = &slider->bar.start_value;
            }
            else {
                /*Calculate the distance from each knob*/
                dist_left = LV_ABS((slider->left_knob_area.y1 + (slider->left_knob_area.y2 - slider->left_knob_area.y1) / 2) - p.y);
                dist_right = LV_ABS((slider->right_knob_area.y1 + (slider->right_knob_area.y2 - slider->right_knob_area.y1) / 2) - p.y);

                /*Use whichever one is closer*/
                if(dist_right < dist_left) {
                    slider->value_to_set = &slider->bar.cur_value;
                    slider->left_knob_focus = 0;
                }
                else {
                    slider->value_to_set = &slider->bar.start_value;
                    slider->left_knob_focus = 1;
                }
            }
        }
    }
}

static void update_knob_pos(lv_obj_t * obj, bool check_drag)
{
    LV_ASSERT(obj != NULL);
    lv_slider_t * slider = (lv_slider_t *)obj;
    lv_indev_t * indev = lv_indev_active();
    if(indev == NULL) return;
    if(lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER)
        return;
    if(lv_indev_get_scroll_obj(indev) != NULL && !slider->dragging)
        return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_transform_point(obj, &p, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE_RECURSIVE);

    bool is_hor = is_slider_horizontal(obj);

    if(check_drag && !slider->dragging) {
        int32_t ofs = is_hor ? (p.x - slider->pressed_point.x) : (p.y - slider->pressed_point.y);

        /*Stop processing when offset is below scroll_limit*/
        if(LV_ABS(ofs) < indev->scroll_limit) {
            return;
        }
    }

    if(!slider->value_to_set) {
        /*Ready to start drag*/
        drag_start(obj);
    }

    int32_t new_value = 0;
    const int32_t range = slider->bar.max_value - slider->bar.min_value;
    const bool is_rtl = LV_BASE_DIR_RTL == lv_obj_get_style_base_dir_internal(obj, LV_PART_MAIN);
    const bool is_horizontal = is_slider_horizontal(obj);
    const bool is_reversed = slider->bar.val_reversed ^ (is_rtl && is_horizontal);

    /*The knob is kept inside the track (see `position_knob()`): the point is where the knob's
     *center goes, convert it to where the indicator's edge has to be. Where the value grows
     *towards the smaller coordinates, it is counted from the last pixel of the indicator, 1 pixel
     *after its edge.*/
    const int32_t back_ofs = (is_hor ? is_reversed : !is_reversed) ? 1 : 0;
    if(is_hor) p.x = knob_center_to_indic_edge(obj, true, p.x) - back_ofs;
    else p.y = knob_center_to_indic_edge(obj, false, p.y) - back_ofs;

    if(is_hor) {
        const int32_t bg_left = lv_obj_get_style_pad_left_internal(obj, LV_PART_MAIN);
        const int32_t bg_right = lv_obj_get_style_pad_right_internal(obj, LV_PART_MAIN);
        const int32_t w = lv_obj_get_width(obj);
        const int32_t indic_w = w - bg_left - bg_right;

        if(is_reversed) {
            /*Make the point relative to the indicator*/
            new_value = (obj->coords.x2 - bg_right) - p.x;
        }
        else {
            /*Make the point relative to the indicator*/
            new_value = p.x - (obj->coords.x1 + bg_left);
        }
        if(indic_w) {
            new_value = (int32_t)(((int64_t)new_value * range + indic_w / 2) / indic_w);
            new_value += slider->bar.min_value;
        }
    }
    else {
        const int32_t bg_top = lv_obj_get_style_pad_top_internal(obj, LV_PART_MAIN);
        const int32_t bg_bottom = lv_obj_get_style_pad_bottom_internal(obj, LV_PART_MAIN);
        const int32_t h = lv_obj_get_height(obj);
        const int32_t indic_h = h - bg_bottom - bg_top;

        if(is_reversed) {
            /*Make the point relative to the indicator*/
            new_value = p.y - (obj->coords.y1 + bg_top);
        }
        else {
            /*Make the point relative to the indicator*/
            new_value = p.y - (obj->coords.y2 + bg_bottom);
            new_value = -new_value;
        }
        new_value = (int32_t)(((int64_t)new_value * range + indic_h / 2) / indic_h);
        new_value += slider->bar.min_value;
    }

    int32_t real_max_value = slider->bar.max_value;
    int32_t real_min_value = slider->bar.min_value;
    /*Figure out the min. and max. for this mode*/
    if(slider->value_to_set == &slider->bar.start_value) {
        real_max_value = slider->bar.cur_value;
    }
    else {
        real_min_value = slider->bar.start_value;
    }

    new_value = LV_CLAMP(real_min_value, new_value, real_max_value);
    if(*slider->value_to_set != new_value) {
        *slider->value_to_set = new_value;
        if(is_hor)
            lv_obj_set_scroll_chain_ver(obj, false);
        else
            lv_obj_set_scroll_chain_hor(obj, false);

        lv_obj_invalidate(obj);
        lv_result_t res = lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
        if(res != LV_RESULT_OK)
            return;
    }
}


#if LV_USE_OBSERVER

static void slider_value_changed_event_cb(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    lv_obj_t * slider = lv_event_get_current_target(e);
    lv_subject_t * subject = lv_event_get_user_data(e);
    LV_ASSERT(slider != NULL);
    LV_ASSERT(subject != NULL);
    LV_ASSERT(subject->type == LV_SUBJECT_TYPE_INT || subject->type == LV_SUBJECT_TYPE_FLOAT);

    if(subject->type == LV_SUBJECT_TYPE_INT) {
        lv_subject_set_int(subject, lv_slider_get_value(slider));
    }
#if LV_USE_FLOAT
    else {
        lv_subject_set_float(subject, (float)lv_slider_get_value(slider));
    }
#endif
}

static void slider_value_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_ASSERT(observer != NULL);
    LV_ASSERT(observer->target != NULL);
    LV_ASSERT(subject != NULL);
    LV_ASSERT(subject->type == LV_SUBJECT_TYPE_INT || subject->type == LV_SUBJECT_TYPE_FLOAT);

    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    /*If the slider is not rendered yet show the new state immediately*/
    lv_anim_enable_t anim_on = obj->rendered ? LV_ANIM_ON : LV_ANIM_OFF;
    if(subject->type == LV_SUBJECT_TYPE_INT) {
        lv_slider_set_value(observer->target, subject->value.num, anim_on);
    }
#if LV_USE_FLOAT
    else {
        lv_slider_set_value(observer->target, (int32_t)subject->value.float_v, anim_on);
    }
#endif
}

#endif /*LV_USE_OBSERVER*/


#endif
