#include <pebble.h>
#include "watch.h"

static Window *s_main_window;
static Layer *s_ticks_layer;
static Layer *s_wall_time_layer;
static Layer *s_stopwatch_layer;

static TextLayer *s_batt_text_layer;
static TextLayer *s_date_text_layer;

static GFont s_font;

static int minute_when_last_updated;

static int tick_radius;
static int second_hand_length;
static int minute_hand_length;
static int hour_hand_length;

static AppTimer *timer_handle = NULL;

static GRect  window_bounds;
static GRect  watch_bounds;
static GRect  inner_bounds;     /* relative to inner layers */
static GPoint center;           /* relative to inner layers */

/* stopwatch complications */
/* 1 = stopwatch 1/10s of a second */
/* 2 = time of day seconds (big hand seconds is for the stopwatch) */
/* 3 = stopwatch minute/hour */
static GPoint center1, center2, center3;
static int    radius1, radius2, radius3;

static WatchSettings settings;

GPoint tick_angle_point(GPoint center, int radius, int angle) {
    int x = center.x + ROUND(radius * 1.0 * sin_lookup(angle) / TRIG_MAX_RATIO);
    int y = center.y - ROUND(radius * 1.0 * cos_lookup(angle) / TRIG_MAX_RATIO);
    return GPoint(x, y);
}

GPoint tick_point(GPoint center, int radius, int degrees) {
    int angle = DEG_TO_TRIGANGLE(degrees);
    return tick_angle_point(center, radius, angle);
}

void draw_ticks (GContext *ctx, GPoint center, int radius, int num_ticks, int ticks_modulo, int thick) {
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_fill_color(ctx, GColorWhite);
    for (int i = 0; i < num_ticks; i += 1) {
        GPoint p = tick_point(center, radius, i * 360 / num_ticks);
        if (i % ticks_modulo == 0) {
            if (thick) {
                graphics_fill_rect(ctx, GRect(p.x - 1, p.y - 1, 3, 3), 0, GCornerNone);
            } else {
                GPoint p1 = tick_point(center, radius + 1, i * 360 / num_ticks);
                GPoint p2 = tick_point(center, radius - 1, i * 360 / num_ticks);
                graphics_context_set_stroke_width(ctx, 1);
                graphics_draw_line(ctx, p1, p2);
            }
        } else {
            graphics_draw_pixel(ctx, p);
        }
    }
}

static void ticks_update_proc(Layer *layer, GContext *ctx) {
    draw_ticks(ctx, center, tick_radius, 60, 5, 1);
    draw_ticks(ctx, center1, radius1, 20, 2, 1);
    draw_ticks(ctx, center2, radius2, 60, 5, 0);
    draw_ticks(ctx, center3, radius3, 60, 5, 0);
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    int second_angle = ROUND(TRIG_MAX_ANGLE / 60.0    *                                            t->tm_sec);
    int minute_angle = ROUND(TRIG_MAX_ANGLE / 3600.0  * (                         t->tm_min * 60 + t->tm_sec));
    int hour_angle   = ROUND(TRIG_MAX_ANGLE / 43200.0 * (t->tm_hour % 12 * 3600 + t->tm_min * 60 + t->tm_sec));

    GPoint second, minute, hour;

    if (settings.stopwatch_uses_big_second_hand) {
        second = tick_angle_point(center2, radius2 - 4, second_angle);
    } else {
        second = tick_angle_point(center, second_hand_length, second_angle);
    }
    minute = tick_angle_point(center, minute_hand_length, minute_angle);
    hour   = tick_angle_point(center, hour_hand_length,   hour_angle);

    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_fill_color(ctx, GColorWhite);
  
    graphics_context_set_stroke_width(ctx, 1);
    if (settings.stopwatch_uses_big_second_hand) {
        graphics_draw_line(ctx, center2, second);
    } else {
        graphics_draw_line(ctx, center, second);
    }

    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_line(ctx, center, minute);
    graphics_draw_line(ctx, center, hour);
}

static void update_date(struct tm *tick_time) {
    static char date_buffer[] = "WED 12/31";
    if (minute_when_last_updated != tick_time->tm_min) {
        strftime(date_buffer, sizeof(date_buffer), "%a %m/%d", tick_time);
        text_layer_set_text(s_date_text_layer, date_buffer);
    }
    minute_when_last_updated = tick_time->tm_min;
}

void stopwatch_update_proc(Layer *layer, GContext *ctx) {
    TimeWithMsec t = stopwatch_time();

    GPoint pt_msec, pt_second, pt_minute, pt_hour;
  
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_fill_color(ctx, GColorWhite);

    pt_msec   = tick_point(center1, radius1 - 4, 360.0 * t.msec / 1000);
    if (settings.stopwatch_uses_big_second_hand) {
        pt_second = tick_point(center, second_hand_length - 4, t.sec % 60 * 6);
    } else {
        pt_second = tick_point(center2, radius2 - 4, t.sec % 60 * 6);
    }
    pt_minute = tick_point(center3, radius3 - 4, ROUND((t.sec / 60) % 60 * 6.0));
    pt_hour = tick_point(center3, ROUND((radius3 - 4.0) * 2.0 / 3.0), ROUND((t.sec / 60) % 720 * 0.5));

    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, center1, pt_msec);
    if (settings.stopwatch_uses_big_second_hand) {
        graphics_draw_line(ctx, center, pt_second);
    } else {
        graphics_draw_line(ctx, center2, pt_second);
    }
    graphics_draw_line(ctx, center3, pt_minute);
    graphics_draw_line(ctx, center3, pt_hour);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    layer_mark_dirty(s_wall_time_layer);
    if (settings.show_date) {
        update_date(tick_time);
    }
}

void update_stopwatch() {
    timer_handle = app_timer_register(50, (AppTimerCallback) update_stopwatch, NULL);
    layer_mark_dirty(s_stopwatch_layer);
}

static void on_battery_state_change(BatteryChargeState charge) {
    static char buffer[] = "100%C";
    int l;
  
    snprintf(buffer, sizeof(buffer), "%d%%", charge.charge_percent);
    if (charge.is_charging) {
        l = strlen(buffer);
        strncpy(buffer + l, "C", sizeof(buffer) - l);
    }
    text_layer_set_text(s_batt_text_layer, buffer);
}

static void message_handler(DictionaryIterator *received, void *context) {
    bool refresh_window = 0;
    Tuple *tuple_show_date                      = dict_find(received, MESSAGE_KEY_ShowDate);
    Tuple *tuple_show_battery                   = dict_find(received, MESSAGE_KEY_ShowBattery);
    Tuple *tuple_use_bold_font                  = dict_find(received, MESSAGE_KEY_UseBoldFont);
    Tuple *tuple_use_larger_font                = dict_find(received, MESSAGE_KEY_UseLargerFont);
    Tuple *tuple_stopwatch_uses_big_second_hand = dict_find(received, MESSAGE_KEY_StopwatchUsesBigSecondHand);

    if (tuple_show_date) {
        refresh_window = 1;
        settings.show_date = (bool)tuple_show_date->value->int32;
    }
    if (tuple_show_battery) {
        refresh_window = 1;
        settings.show_battery = (bool)tuple_show_battery->value->int32;
    }
    if (tuple_use_bold_font) {
        refresh_window = 1;
        settings.use_bold_font = (bool)tuple_use_bold_font->value->int32;
    }
    if (tuple_use_larger_font) {
        refresh_window = 1;
        settings.use_larger_font = (bool)tuple_use_larger_font->value->int32;
    }
    if (tuple_stopwatch_uses_big_second_hand) {
        refresh_window = 1;
        settings.stopwatch_uses_big_second_hand = (bool)tuple_stopwatch_uses_big_second_hand->value->int32;
    }

    persist_write_data(SETTINGS_KEY, &settings, sizeof(settings));

    if (refresh_window) {
        main_window_destroy();
        main_window_create();
    }
}

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    static BatteryChargeState battery_state;

    /* defaults */
    settings.show_date = 0;
    settings.show_battery = 0;
    settings.use_bold_font = 0;
    settings.use_larger_font = 0;
    settings.stopwatch_uses_big_second_hand = 1;

    persist_read_data(SETTINGS_KEY, &settings, sizeof(settings));

    window_bounds = layer_get_bounds(window_layer);

    watch_bounds = window_bounds;
    if (settings.show_date || settings.show_battery) {
        if (settings.use_larger_font) {
            watch_bounds.origin.y += 18;
            watch_bounds.size.h   -= 18;
        } else {
            watch_bounds.origin.y += 14;
            watch_bounds.size.h   -= 14;
        }
    }

    tick_radius = MIN(watch_bounds.size.h, watch_bounds.size.w) / 2 - 2;
    second_hand_length = tick_radius - 3;
    minute_hand_length = ROUND(tick_radius * 0.8);
    hour_hand_length   = ROUND(tick_radius * 0.5);

    window_set_background_color(window, GColorBlack);

    s_ticks_layer = layer_create(watch_bounds);
    layer_set_update_proc(s_ticks_layer, ticks_update_proc);
    layer_add_child(window_layer, s_ticks_layer);

    inner_bounds = layer_get_bounds(s_ticks_layer);

    center = grect_center_point(&inner_bounds);
    center1 = tick_point(center, ROUND(tick_radius * 0.6),   0);
    center2 = tick_point(center, ROUND(tick_radius * 0.5), 285);
    center3 = tick_point(center, ROUND(tick_radius * 0.5), 180);
    radius1 = 20;
    radius2 = 20;
    radius3 = 30;

    s_wall_time_layer = layer_create(watch_bounds);
    layer_set_update_proc(s_wall_time_layer, canvas_update_proc);
    layer_add_child(window_layer, s_wall_time_layer);

    s_date_text_layer = NULL;
    s_batt_text_layer = NULL;

    if (settings.use_bold_font) {
        if (settings.use_larger_font) {
            s_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
        } else {
            s_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
        }
    } else {
        if (settings.use_larger_font) {
            s_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
        } else {
            s_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
        }
    }

    if (settings.show_date) {
        s_date_text_layer = text_layer_create(GRect(0, 0, 90, settings.use_larger_font ? 18 : 14));
        text_layer_set_background_color(s_date_text_layer, GColorBlack);
        text_layer_set_text_color(s_date_text_layer, GColorWhite);
        text_layer_set_font(s_date_text_layer, s_font);
        text_layer_set_text_alignment(s_date_text_layer, GTextAlignmentLeft);
        layer_add_child(window_layer, text_layer_get_layer(s_date_text_layer));
    }

    if (settings.show_battery) {
        s_batt_text_layer = text_layer_create(GRect(90, 0, 54, settings.use_larger_font ? 18 : 14));
        text_layer_set_background_color(s_batt_text_layer, GColorBlack);
        text_layer_set_text_color(s_batt_text_layer, GColorWhite);
        text_layer_set_font(s_batt_text_layer, s_font);
        text_layer_set_text_alignment(s_batt_text_layer, GTextAlignmentRight);
        layer_add_child(window_layer, text_layer_get_layer(s_batt_text_layer));
    }

    minute_when_last_updated = -1;

    s_stopwatch_layer = layer_create(watch_bounds);
    layer_set_update_proc(s_stopwatch_layer, stopwatch_update_proc);
    layer_add_child(window_layer, s_stopwatch_layer);

    layer_mark_dirty(s_wall_time_layer);
    layer_mark_dirty(s_stopwatch_layer);

    tick_timer_service_subscribe(SECOND_UNIT, tick_handler);

    if (stopwatch_load_persist()) {
        timer_handle = app_timer_register(50, (AppTimerCallback) update_stopwatch, NULL);
    }

    if (settings.show_battery) {
        battery_state = battery_state_service_peek();
        on_battery_state_change(battery_state);
        battery_state_service_subscribe(on_battery_state_change);
    }
}

static void main_window_unload(Window *window) {
    battery_state_service_unsubscribe();
    tick_timer_service_unsubscribe();
    minute_when_last_updated = -1;
  
    if (s_date_text_layer) {
        text_layer_destroy(s_date_text_layer);
        s_date_text_layer = NULL;
    }
    if (s_batt_text_layer) {
        text_layer_destroy(s_batt_text_layer);
        s_batt_text_layer = NULL;
    }

    layer_destroy(s_wall_time_layer);
    layer_destroy(s_stopwatch_layer);
}

void up_single_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (stopwatch_start_stop()) {	/* start */
        if (timer_handle) {
            app_timer_cancel(timer_handle);
        }
        timer_handle = app_timer_register(50, (AppTimerCallback) update_stopwatch, NULL);
    } else {			/* stop */
        if (timer_handle) {
            app_timer_cancel(timer_handle);
        }
        timer_handle = NULL;
        layer_mark_dirty(s_stopwatch_layer);
    }
}

void down_single_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (stopwatch_lap_reset()) {	/* lap */
    } else {			/* reset */
    }
    layer_mark_dirty(s_stopwatch_layer);
}

void click_config_provider(Window *window) {
    window_single_click_subscribe(BUTTON_ID_UP,     up_single_click_handler);
    window_single_click_subscribe(BUTTON_ID_DOWN,   down_single_click_handler);
}

static void main_window_create() {
    s_main_window = window_create();
    WindowHandlers wh = {
	.load   = main_window_load,
        .unload = main_window_unload
    };
    window_set_background_color(s_main_window, GColorBlack);
    window_set_window_handlers(s_main_window, wh);
    window_stack_push(s_main_window, true);
}

static void main_window_destroy() {
    window_stack_pop(true);
    window_destroy(s_main_window);
}

static void init(void) {
    main_window_create();
    app_message_open(app_message_inbox_size_maximum(),
                     app_message_outbox_size_maximum());
    app_message_register_inbox_received(message_handler);
    window_set_click_config_provider(s_main_window, (ClickConfigProvider) click_config_provider);
}

static void deinit(void) {
    app_message_deregister_callbacks();
    main_window_destroy();
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}

