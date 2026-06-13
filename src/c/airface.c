#include <pebble.h>
#include <math.h>

// Persist keys for last-known values + settings (survive restarts and
// phone-out-of-range periods).
#define PERSIST_STEPS          1
#define PERSIST_HEART_RATE     2
#define PERSIST_SLEEP_SCORE    3
#define PERSIST_CALORIES       4
#define PERSIST_DISTANCE       5   // meters
#define PERSIST_ZONE_MINUTES   6
#define PERSIST_HEART_RATE_MAX 7
#define PERSIST_STEPS_GOAL     8
#define PERSIST_ZONE_GOAL      9
#define PERSIST_CAL_GOAL       10
#define PERSIST_UNITS          11  // 0 = miles, 1 = km
#define PERSIST_HR_MODE        12  // 0 = resting, 1 = avg, 2 = min/max
#define PERSIST_UPDATE_MIN     13
#define PERSIST_BG_STYLE       14  // 0 = black, 1 = navy
#define PERSIST_TIME_FMT       15  // 0 = auto, 1 = 12h, 2 = 24h
#define PERSIST_COLOR_THEME    16  // 0 = color, 1 = mono (teal)
#define PERSIST_RING_STYLE     17  // 0 = classic concentric, 1 = SHD segment
#define PERSIST_RING_LABELS    18  // 0 = off, 1 = on

// Defaults until the config page sends real settings.
#define DEFAULT_STEPS_GOAL  10000
#define DEFAULT_ZONE_GOAL   22
#define DEFAULT_CAL_GOAL    600
#define DEFAULT_UPDATE_MIN  15
#define DEFAULT_RING_STYLE  1     // SHD during development; flip to 0 for release default
#define DEFAULT_RING_LABELS 0

// Classic concentric ring geometry.
#define RING_PROG_THICKNESS  6
#define RING_TRACK_THICKNESS 2
#define RING_GAP_DEG         8
#define RING_INSET_2   13
#define RING_INSET_3   26

// SHD single segment ring geometry.
#define SHD_RING_THICKNESS  12
#define SHD_GAP_DEG         4
#define SHD_SEG_DEG         ((360 - SHD_GAP_DEG * 3) / 3)  // 116°
#define SHD_START_DEG       0   // S=steps starts at 12-o'clock, clean natural position
#define SHD_LABEL_RADIUS    78  // px from center, sits on inner ring edge

static Window *s_window;
static Layer *s_arc_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static TextLayer *s_battery_layer;
static TextLayer *s_steps_layer;
static TextLayer *s_heart_layer;
static TextLayer *s_heart_label;
static TextLayer *s_sleep_layer;
static TextLayer *s_sleep_label;

static char s_time_buf[8];
static char s_date_buf[16];
static char s_battery_buf[12];
static char s_steps_buf[28];
static char s_heart_buf[12];
static char s_sleep_buf[8];

// Live metric state (mirrored to persist).
static int s_steps;
static int s_heart;        // resting / avg / min
static int s_heart_max;    // max (min/max mode only)
static int s_sleep;
static int s_calories;
static int s_distance;     // meters
static int s_zone_minutes;

// Settings state (mirrored to persist).
static int s_steps_goal;
static int s_zone_goal;
static int s_cal_goal;
static int s_units;        // 0 = miles, 1 = km
static int s_hr_mode;      // 0 = resting, 1 = avg, 2 = min/max
static int s_update_min;
static int s_bg_style;     // 0 = black, 1 = navy
static int s_time_fmt;     // 0 = auto, 1 = 12h, 2 = 24h
static int s_color_theme;  // 0 = color, 1 = mono (teal)
static int s_ring_style;   // 0 = classic, 1 = SHD
static int s_ring_labels;  // 0 = off, 1 = on

static int prv_read_int_def(int key, int def) {
  return persist_exists(key) ? persist_read_int(key) : def;
}

static void prv_update_time(struct tm *tick_time);
static void prv_apply_layout(void);

// ── Rendering ───────────────────────────────────────────────────────────────

static GColor prv_ring_color(int ring) {
  if (s_color_theme == 1) {
    return ring == 3 ? GColorCadetBlue : GColorTiffanyBlue;
  }
  return ring == 1 ? GColorGreen : ring == 2 ? GColorYellow : GColorOrange;
}

static GColor prv_track_color(int ring) {
  if (s_color_theme == 1) return GColorDarkGray;
  return ring == 1 ? GColorDarkGreen :
         ring == 2 ? GColorArmyGreen : GColorBulgarianRose;
}

// SHD segment colors: seg 0=steps, 1=zone, 2=cals
static GColor prv_shd_fill_color(int seg) {
  if (s_color_theme == 1) return GColorTiffanyBlue;
  return seg == 0 ? GColorGreen : seg == 1 ? GColorYellow : GColorOrange;
}

static GColor prv_shd_track_color(int seg) {
  if (s_color_theme == 1) return GColorDarkGray;
  return seg == 0 ? GColorDarkGreen :
         seg == 1 ? GColorArmyGreen : GColorBulgarianRose;
}

static void prv_render_steps(void) {
  int tenths;
  const char *unit;
  if (s_units == 1) {
    tenths = (s_distance + 50) / 100;          // tenths of a km
    unit = "km";
  } else {
    tenths = (s_distance * 10 + 805) / 1609;   // tenths of a mile
    unit = "mi";
  }
  snprintf(s_steps_buf, sizeof(s_steps_buf), "%d steps · %d.%d %s",
           s_steps, tenths / 10, tenths % 10, unit);
  text_layer_set_text(s_steps_layer, s_steps_buf);
}

static void prv_render_heart(void) {
  if (s_hr_mode == 2 && s_heart_max > 0) {
    snprintf(s_heart_buf, sizeof(s_heart_buf), "%d-%d", s_heart, s_heart_max);
    text_layer_set_font(s_heart_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text(s_heart_label, "RANGE");
  } else {
    snprintf(s_heart_buf, sizeof(s_heart_buf), "%d", s_heart);
    text_layer_set_font(s_heart_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
    text_layer_set_text(s_heart_label, s_hr_mode == 0 ? "RESTING" : "AVG HR");
  }
  text_layer_set_text(s_heart_layer, s_heart_buf);
}

static void prv_render_sleep(void) {
  snprintf(s_sleep_buf, sizeof(s_sleep_buf), "%d", s_sleep);
  text_layer_set_text(s_sleep_layer, s_sleep_buf);
}

static void prv_render_battery(BatteryChargeState state) {
  if (state.is_charging) {
    snprintf(s_battery_buf, sizeof(s_battery_buf), "charging");
  } else {
    snprintf(s_battery_buf, sizeof(s_battery_buf), "%d%%", state.charge_percent);
  }
  text_layer_set_text(s_battery_layer, s_battery_buf);
}

// ── Classic ring draw ─────────────────────────────────────────────────────

static void prv_draw_ring(GContext *ctx, GRect rect, int value, int goal,
                          GColor track, GColor fill) {
  graphics_context_set_fill_color(ctx, track);
  graphics_fill_radial(ctx, rect, GOvalScaleModeFitCircle, RING_TRACK_THICKNESS,
                       0, DEG_TO_TRIGANGLE(360));
  int v        = value > goal ? goal : value;
  int arc_span = 360 - 2 * RING_GAP_DEG;
  int fill_deg = goal > 0 ? v * arc_span / goal : 0;
  if (fill_deg > 0) {
    graphics_context_set_fill_color(ctx, fill);
    graphics_fill_radial(ctx, rect, GOvalScaleModeFitCircle, RING_PROG_THICKNESS,
                         DEG_TO_TRIGANGLE(RING_GAP_DEG),
                         DEG_TO_TRIGANGLE(RING_GAP_DEG + fill_deg));
  }
}

// ── SHD segment ring draw ─────────────────────────────────────────────────

// Draws a radial arc, splitting into two calls if the arc crosses 360°.
static void prv_fill_arc(GContext *ctx, GRect bounds, int start, int end, int thickness) {
  if (end <= 360) {
    graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, thickness,
                         DEG_TO_TRIGANGLE(start), DEG_TO_TRIGANGLE(end));
  } else {
    graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, thickness,
                         DEG_TO_TRIGANGLE(start), DEG_TO_TRIGANGLE(360));
    graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, thickness,
                         DEG_TO_TRIGANGLE(0), DEG_TO_TRIGANGLE(end - 360));
  }
}

static void prv_draw_shd_ring(GContext *ctx, GRect bounds) {
  GPoint center = grect_center_point(&bounds);
  GFont lbl_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  const int values[3] = { s_steps,        s_zone_minutes, s_calories };
  const int goals[3]  = { s_steps_goal,   s_zone_goal,    s_cal_goal };
  const char *labels[3] = { "S", "Z", "C" };

  for (int i = 0; i < 3; i++) {
    int start_deg = (SHD_START_DEG + i * (SHD_SEG_DEG + SHD_GAP_DEG)) % 360;
    int end_deg   = start_deg + SHD_SEG_DEG;

    // Muted track for the full segment
    graphics_context_set_fill_color(ctx, prv_shd_track_color(i));
    prv_fill_arc(ctx, bounds, start_deg, end_deg, SHD_RING_THICKNESS);

    // Bright fill for progress (clamped to goal)
    int v        = values[i] > goals[i] ? goals[i] : values[i];
    int fill_deg = goals[i] > 0 ? v * SHD_SEG_DEG / goals[i] : 0;
    if (fill_deg > 0) {
      graphics_context_set_fill_color(ctx, prv_shd_fill_color(i));
      prv_fill_arc(ctx, bounds, start_deg, start_deg + fill_deg, SHD_RING_THICKNESS);
    }

  }

  // Small centered legend: S · Z · C in segment colors, at very bottom mirroring date gap
  GFont leg = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  const int ly = 132;
  // positions: S at cx-18, · at cx-8, Z at cx-3, · at cx+8, C at cx+14
  const int offsets[3] = { -18, -3, 12 };
  for (int i = 0; i < 3; i++) {
    graphics_context_set_text_color(ctx, prv_shd_fill_color(i));
    graphics_draw_text(ctx, labels[i], leg,
                       GRect(center.x + offsets[i], ly, 10, 12),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
  graphics_context_set_text_color(ctx, GColorDarkGray);
  graphics_draw_text(ctx, "·", leg, GRect(center.x - 10, ly, 10, 12),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, "·", leg, GRect(center.x + 5, ly, 10, 12),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

// ── Arc layer update proc ─────────────────────────────────────────────────

static void prv_arc_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  GColor base = (s_bg_style == 1) ? GColorOxfordBlue : GColorBlack;
  graphics_context_set_fill_color(ctx, base);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_ring_style == 1) {
    prv_draw_shd_ring(ctx, bounds);
  } else {
    prv_draw_ring(ctx, bounds, s_steps, s_steps_goal,
                  prv_track_color(1), prv_ring_color(1));
    prv_draw_ring(ctx, grect_inset(bounds, GEdgeInsets(RING_INSET_2)),
                  s_zone_minutes, s_zone_goal, prv_track_color(2), prv_ring_color(2));
    prv_draw_ring(ctx, grect_inset(bounds, GEdgeInsets(RING_INSET_3)),
                  s_calories, s_cal_goal, prv_track_color(3), prv_ring_color(3));

    // Classic S/Z/C labels in the 12-o'clock gap
    GFont lbl = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    const char *class_labels[3] = {"S", "Z", "C"};
    const int   label_y[3] = {0, RING_INSET_2, RING_INSET_3};
    GPoint center = grect_center_point(&bounds);
    for (int i = 0; i < 3; i++) {
      graphics_context_set_text_color(ctx, prv_ring_color(i + 1));
      graphics_draw_text(ctx, class_labels[i], lbl,
                         GRect(center.x - 14, label_y[i], 28, 14),
                         GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }
  }
}

// ── Theme + layout ────────────────────────────────────────────────────────

static void prv_apply_theme(void) {
  bool mono = (s_color_theme == 1);
  text_layer_set_text_color(s_time_layer,    mono ? GColorTiffanyBlue : GColorWhite);
  text_layer_set_text_color(s_heart_layer,   mono ? GColorTiffanyBlue : GColorRed);
  text_layer_set_text_color(s_sleep_layer,   mono ? GColorTiffanyBlue : GColorVividCerulean);
  text_layer_set_text_color(s_battery_layer, mono ? GColorTiffanyBlue : GColorLightGray);
  layer_mark_dirty(s_arc_layer);
}

static void prv_apply_layout(void) {
  Layer *root = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(root);
  const int w = bounds.size.w;
  const int margin = 42;
  const int col = (w - 2 * margin) / 2;
  bool shd = (s_ring_style == 1);

  if (shd) {
    // SHD: single outer ring opens up the full interior (y=16–164, 148px usable).
    // 6 items totalling ~130px; ~18px slack split into even 3px gaps; block
    // vertically centered so content sits at y=25–170.
    text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    // GOTHIC_28_BOLD values at y=151 fill to the ring bottom (y=168).
    // Steps/legend/labels packed tightly above to make room.
    text_layer_set_font(s_heart_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
    text_layer_set_font(s_sleep_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
    layer_set_frame(text_layer_get_layer(s_date_layer),    GRect(0, 30, w, 14));
    layer_set_frame(text_layer_get_layer(s_battery_layer), GRect(0, 46, w, 14));
    layer_set_frame(text_layer_get_layer(s_time_layer),    GRect(0, 62, w, 50));
    layer_set_frame(text_layer_get_layer(s_steps_layer),   GRect(0, 113, w, 14));
    // legend drawn at ly=132 (arc layer), +4px gap under steps
    layer_set_frame(text_layer_get_layer(s_heart_label),   GRect(margin, 148, col, 12));
    layer_set_frame(text_layer_get_layer(s_sleep_label),   GRect(margin + col, 148, col, 12));
    layer_set_frame(text_layer_get_layer(s_heart_layer),   GRect(margin, 160, col, 32));
    layer_set_frame(text_layer_get_layer(s_sleep_layer),   GRect(margin + col, 160, col, 32));
    layer_set_hidden(text_layer_get_layer(s_battery_layer), false);
  } else {
    // Classic: three concentric rings eat the interior; keep original positions
    text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
    layer_set_frame(text_layer_get_layer(s_date_layer),  GRect(0, 54, w, 22));
    layer_set_frame(text_layer_get_layer(s_time_layer),  GRect(0, 80, w, 50));
    layer_set_frame(text_layer_get_layer(s_steps_layer), GRect(0, 132, w, 18));
    layer_set_frame(text_layer_get_layer(s_heart_label), GRect(margin, 156, col, 18));
    layer_set_frame(text_layer_get_layer(s_sleep_label), GRect(margin + col, 156, col, 18));
    layer_set_frame(text_layer_get_layer(s_heart_layer), GRect(margin, 173, col, 32));
    layer_set_frame(text_layer_get_layer(s_sleep_layer), GRect(margin + col, 173, col, 32));
    layer_set_hidden(text_layer_get_layer(s_battery_layer), true);
  }
}

// ── Communication ─────────────────────────────────────────────────────────

static void prv_request_update(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_REQUEST_UPDATE, 1);
  app_message_outbox_send();
}

static void prv_store(int key, int *field, int value) {
  if (*field == value) return;
  *field = value;
  persist_write_int(key, value);
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_STEPS)))
    prv_store(PERSIST_STEPS, &s_steps, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_HEART_RATE)))
    prv_store(PERSIST_HEART_RATE, &s_heart, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_HEART_RATE_MAX)))
    prv_store(PERSIST_HEART_RATE_MAX, &s_heart_max, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_SLEEP_SCORE)))
    prv_store(PERSIST_SLEEP_SCORE, &s_sleep, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_CALORIES)))
    prv_store(PERSIST_CALORIES, &s_calories, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_DISTANCE)))
    prv_store(PERSIST_DISTANCE, &s_distance, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_ZONE_MINUTES)))
    prv_store(PERSIST_ZONE_MINUTES, &s_zone_minutes, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_STEPS_GOAL)))
    prv_store(PERSIST_STEPS_GOAL, &s_steps_goal, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_ZONE_GOAL)))
    prv_store(PERSIST_ZONE_GOAL, &s_zone_goal, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_CAL_GOAL)))
    prv_store(PERSIST_CAL_GOAL, &s_cal_goal, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_UNITS)))
    prv_store(PERSIST_UNITS, &s_units, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_HR_MODE)))
    prv_store(PERSIST_HR_MODE, &s_hr_mode, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_UPDATE_MIN)))
    prv_store(PERSIST_UPDATE_MIN, &s_update_min, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_BG_STYLE)))
    prv_store(PERSIST_BG_STYLE, &s_bg_style, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_TIME_FMT)))
    prv_store(PERSIST_TIME_FMT, &s_time_fmt, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_COLOR_THEME)))
    prv_store(PERSIST_COLOR_THEME, &s_color_theme, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_RING_STYLE)))
    prv_store(PERSIST_RING_STYLE, &s_ring_style, t->value->int32);
  if ((t = dict_find(iter, MESSAGE_KEY_RING_LABELS)))
    prv_store(PERSIST_RING_LABELS, &s_ring_labels, t->value->int32);

  prv_render_steps();
  prv_render_heart();
  prv_render_sleep();
  prv_apply_theme();
  prv_apply_layout();

  time_t now = time(NULL);
  prv_update_time(localtime(&now));
}

// ── Time ──────────────────────────────────────────────────────────────────

static void prv_update_time(struct tm *tick_time) {
  bool use_24h = (s_time_fmt == 2) || (s_time_fmt == 0 && clock_is_24h_style());
  strftime(s_time_buf, sizeof(s_time_buf),
           use_24h ? "%H:%M" : "%l:%M", tick_time);
  text_layer_set_text(s_time_layer, s_time_buf);

  strftime(s_date_buf, sizeof(s_date_buf), "%a %e %b", tick_time);
  for (char *p = s_date_buf; *p; p++) {
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  prv_update_time(tick_time);
  int interval = s_update_min > 0 ? s_update_min : DEFAULT_UPDATE_MIN;
  if (tick_time->tm_min % interval == 0) {
    prv_request_update();
  }
}

// ── Battery ───────────────────────────────────────────────────────────────

static void prv_battery_handler(BatteryChargeState state) {
  prv_render_battery(state);
}

// ── Window ────────────────────────────────────────────────────────────────

static TextLayer *prv_make_label(Layer *parent, GRect frame, const char *font_key,
                                 GColor color) {
  TextLayer *layer = text_layer_create(frame);
  text_layer_set_background_color(layer, GColorClear);
  text_layer_set_text_color(layer, color);
  text_layer_set_font(layer, fonts_get_system_font(font_key));
  text_layer_set_text_alignment(layer, GTextAlignmentCenter);
  layer_add_child(parent, text_layer_get_layer(layer));
  return layer;
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  const int w = bounds.size.w;
  const int margin = 42;
  const int col = (w - 2 * margin) / 2;

  s_arc_layer = layer_create(bounds);
  layer_set_update_proc(s_arc_layer, prv_arc_update);
  layer_add_child(window_layer, s_arc_layer);

  // Create all text layers at placeholder positions; prv_apply_layout() sets
  // final frames after settings are loaded.
  s_date_layer    = prv_make_label(window_layer, GRect(0, 0, w, 16),
                                   FONT_KEY_GOTHIC_14, GColorLightGray);
  s_battery_layer = prv_make_label(window_layer, GRect(0, 0, w, 16),
                                   FONT_KEY_GOTHIC_14, GColorLightGray);
  s_time_layer    = prv_make_label(window_layer, GRect(0, 0, w, 50),
                                   FONT_KEY_BITHAM_42_BOLD, GColorWhite);
  s_steps_layer   = prv_make_label(window_layer, GRect(0, 0, w, 16),
                                   FONT_KEY_GOTHIC_14, GColorLightGray);

  s_heart_label = prv_make_label(window_layer, GRect(margin, 0, col, 18),
                                 FONT_KEY_GOTHIC_14, GColorLightGray);
  text_layer_set_text(s_heart_label, "RESTING");
  s_sleep_label = prv_make_label(window_layer, GRect(margin + col, 0, col, 18),
                                 FONT_KEY_GOTHIC_14, GColorLightGray);
  text_layer_set_text(s_sleep_label, "SLEEP");
  s_heart_layer = prv_make_label(window_layer, GRect(margin, 0, col, 32),
                                 FONT_KEY_GOTHIC_28_BOLD, GColorRed);
  s_sleep_layer = prv_make_label(window_layer, GRect(margin + col, 0, col, 32),
                                 FONT_KEY_GOTHIC_28_BOLD, GColorVividCerulean);

  // Seed settings and last-known metric values from persist.
  s_steps_goal  = prv_read_int_def(PERSIST_STEPS_GOAL,  DEFAULT_STEPS_GOAL);
  s_zone_goal   = prv_read_int_def(PERSIST_ZONE_GOAL,   DEFAULT_ZONE_GOAL);
  s_cal_goal    = prv_read_int_def(PERSIST_CAL_GOAL,    DEFAULT_CAL_GOAL);
  s_units       = persist_read_int(PERSIST_UNITS);
  s_hr_mode     = persist_read_int(PERSIST_HR_MODE);
  s_update_min  = prv_read_int_def(PERSIST_UPDATE_MIN,  DEFAULT_UPDATE_MIN);
  s_bg_style    = persist_read_int(PERSIST_BG_STYLE);
  s_time_fmt    = persist_read_int(PERSIST_TIME_FMT);
  s_color_theme = persist_read_int(PERSIST_COLOR_THEME);
  s_ring_style  = prv_read_int_def(PERSIST_RING_STYLE,  DEFAULT_RING_STYLE);
  s_ring_labels = prv_read_int_def(PERSIST_RING_LABELS, DEFAULT_RING_LABELS);

  s_steps        = persist_read_int(PERSIST_STEPS);
  s_heart        = persist_read_int(PERSIST_HEART_RATE);
  s_heart_max    = persist_read_int(PERSIST_HEART_RATE_MAX);
  s_sleep        = persist_read_int(PERSIST_SLEEP_SCORE);
  s_calories     = persist_read_int(PERSIST_CALORIES);
  s_distance     = persist_read_int(PERSIST_DISTANCE);
  s_zone_minutes = persist_read_int(PERSIST_ZONE_MINUTES);

  prv_render_steps();
  prv_render_heart();
  prv_render_sleep();
  prv_render_battery(battery_state_service_peek());
  prv_apply_theme();
  prv_apply_layout();

  time_t now = time(NULL);
  prv_update_time(localtime(&now));
}

static void prv_window_unload(Window *window) {
  layer_destroy(s_arc_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
  text_layer_destroy(s_battery_layer);
  text_layer_destroy(s_steps_layer);
  text_layer_destroy(s_heart_layer);
  text_layer_destroy(s_heart_label);
  text_layer_destroy(s_sleep_layer);
  text_layer_destroy(s_sleep_label);
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

static void prv_init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  battery_state_service_subscribe(prv_battery_handler);

  app_message_register_inbox_received(prv_inbox_received);
  app_message_open(512, 64);

  prv_request_update();
}

static void prv_deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
