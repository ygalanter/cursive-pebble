#include <pebble.h>

// ── Persist keys ────────────────────────────────────────────────────────────
#define PERSIST_DATA1       0
#define PERSIST_DATA2       1
#define PERSIST_DATA3       2
#define PERSIST_DATA4       3
#define PERSIST_DATA5       4
#define PERSIST_PAPER_COLOR 5
#define PERSIST_LINES_COLOR 6
#define PERSIST_TEXT_COLOR  7
#define PERSIST_DATA_TIMEOUT 8

// ── Defaults ─────────────────────────────────────────────────────────────────
#define DEFAULT_DATA1       "Dear diary, today"
#define DEFAULT_DATA2       "I walked %steps% steps"
#define DEFAULT_DATA3       "And burned %calories% cals"
#define DEFAULT_DATA4       "My heartbeat is %heartrate%"
#define DEFAULT_DATA5       "Battery: %battery%"
#define DEFAULT_PAPER_COLOR 0xFFFF55u
#define DEFAULT_LINES_COLOR 0xAAAA55u
#define DEFAULT_TEXT_COLOR  0x0000AAu

// Auto-return-to-time-screen timeout (seconds). 0 disables.
#define MAX_DATA_TIMEOUT_SECONDS 300

// ── Layout constants ─────────────────────────────────────────────────────────
#define LINE_STEP  44
#define SCREEN_W   200
#define SCREEN_H   228
#define DATA_LINE_H 44

// ── App state ────────────────────────────────────────────────────────────────
static Window   *s_window;

// Background layer (shared between both screens)
static Layer    *s_bg_layer;

// Time screen layers
static TextLayer *s_time_layer;
static TextLayer *s_dow_layer;
static TextLayer *s_date_layer;

// Data screen layers
static TextLayer *s_data_layers[5];

// Fonts
static GFont s_font_time;
static GFont s_font_text;

// Colors (nearest Pebble 64-color)
static GColor s_paper_color;
static GColor s_lines_color;
static GColor s_text_color;

// Screen state
static bool s_show_data = false;
static time_t s_last_tap = 0;
static uint16_t s_data_timeout = 0;       // seconds; 0 = disabled
static AppTimer *s_data_view_timer = NULL;

// Buffers
static char s_time_buf[8];
static char s_dow_buf[32];
static char s_date_buf[32];
static char s_data_buf[5][64];
static char s_data_tmpl[5][64];

// ── Month / day name tables ───────────────────────────────────────────────────
static const char *MONTHS[] = {
  "January","February","March","April","May","June",
  "July","August","September","October","November","December"
};
static const char *DAYS[] = {
  "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};

// ── GColor from 0xRRGGBB hex ─────────────────────────────────────────────────
static GColor color_from_hex(uint32_t hex) {
  uint8_t r = (hex >> 16) & 0xFF;
  uint8_t g = (hex >>  8) & 0xFF;
  uint8_t b =  hex        & 0xFF;
  // Snap to 0/85/170/255 per channel
  r = (r + 42) / 85 * 85;
  g = (g + 42) / 85 * 85;
  b = (b + 42) / 85 * 85;
  return GColorFromRGB(r, g, b);
}

// ── Persist helpers ───────────────────────────────────────────────────────────
static void load_colors(void) {
  uint32_t p = persist_exists(PERSIST_PAPER_COLOR)
               ? (uint32_t)persist_read_int(PERSIST_PAPER_COLOR) : DEFAULT_PAPER_COLOR;
  uint32_t l = persist_exists(PERSIST_LINES_COLOR)
               ? (uint32_t)persist_read_int(PERSIST_LINES_COLOR) : DEFAULT_LINES_COLOR;
  uint32_t t = persist_exists(PERSIST_TEXT_COLOR)
               ? (uint32_t)persist_read_int(PERSIST_TEXT_COLOR)  : DEFAULT_TEXT_COLOR;
  s_paper_color = color_from_hex(p);
  s_lines_color = color_from_hex(l);
  s_text_color  = color_from_hex(t);
}

static void load_templates(void) {
  const char *defaults[5] = {
    DEFAULT_DATA1, DEFAULT_DATA2, DEFAULT_DATA3, DEFAULT_DATA4, DEFAULT_DATA5
  };
  for (int i = 0; i < 5; i++) {
    if (persist_exists(PERSIST_DATA1 + i)) {
      persist_read_string(PERSIST_DATA1 + i, s_data_tmpl[i], sizeof(s_data_tmpl[i]));
    } else {
      strncpy(s_data_tmpl[i], defaults[i], sizeof(s_data_tmpl[i]) - 1);
      s_data_tmpl[i][sizeof(s_data_tmpl[i]) - 1] = '\0';
    }
  }
}

static uint16_t clamp_timeout(int v) {
  if (v < 0) return 0;
  if (v > MAX_DATA_TIMEOUT_SECONDS) return MAX_DATA_TIMEOUT_SECONDS;
  return (uint16_t)v;
}

static void load_timeout(void) {
  if (persist_exists(PERSIST_DATA_TIMEOUT)) {
    s_data_timeout = clamp_timeout(persist_read_int(PERSIST_DATA_TIMEOUT));
  } else {
    s_data_timeout = 0;
  }
}

// In-place placeholder replacement within buf — replaces every occurrence.
static void replace_in_buf(char *buf, size_t buf_sz, const char *placeholder, const char *value) {
  size_t ph_len  = strlen(placeholder);
  size_t val_len = strlen(value);
  if (ph_len == 0) return;
  char *p = buf;
  while ((p = strstr(p, placeholder)) != NULL) {
    size_t buf_len = strlen(buf);
    size_t prefix  = (size_t)(p - buf);
    size_t suffix  = buf_len - prefix - ph_len;
    size_t this_val = val_len;
    if (prefix + this_val + suffix + 1 > buf_sz) {
      this_val = buf_sz - prefix - suffix - 1;
    }
    memmove(p + this_val, p + ph_len, suffix + 1);
    memcpy(p, value, this_val);
    p += this_val;  // skip past the inserted value (avoids re-matching its content)
  }
}

// Format a distance in meters into "X.X km" or "X.X mi".
static void format_distance(char *out, size_t out_sz, int meters) {
  bool metric = health_service_get_measurement_system_for_display(
      HealthMetricWalkedDistanceMeters) == MeasurementSystemMetric;
  if (metric) {
    snprintf(out, out_sz, "%d.%01d km", meters / 1000, (meters % 1000) / 100);
  } else {
    int tenths = (meters * 10) / 1609;
    snprintf(out, out_sz, "%d.%d mi", tenths / 10, tenths % 10);
  }
}

// ── Health/data formatting ────────────────────────────────────────────────────
// IMPORTANT: process longer placeholders BEFORE shorter ones so e.g. %stepsgoal%
// is handled before %steps% (which is a substring of it).
static void format_data_line(int idx) {
  // Work in s_data_buf[idx] directly — start with the template
  strncpy(s_data_buf[idx], s_data_tmpl[idx], sizeof(s_data_buf[idx]) - 1);
  s_data_buf[idx][sizeof(s_data_buf[idx]) - 1] = '\0';

  char val[16];

  // ── Goal placeholders first (longer strings; must run before base ones to
  //    avoid %steps% matching inside %stepsgoal%). The Pebble SDK doesn't
  //    expose user goals — we render 0 as a safe fallback.
  snprintf(val, sizeof(val), "0");
  replace_in_buf(s_data_buf[idx], sizeof(s_data_buf[idx]), "%stepsgoal%",    val);
  replace_in_buf(s_data_buf[idx], sizeof(s_data_buf[idx]), "%caloriesgoal%", val);
  format_distance(val, sizeof(val), 0);
  replace_in_buf(s_data_buf[idx], sizeof(s_data_buf[idx]), "%distancegoal%", val);

  // ── Base placeholders ──────────────────────────────────────────────────────
  // %steps%
  HealthServiceAccessibilityMask mask =
      health_service_metric_accessible(HealthMetricStepCount, time_start_of_today(), time(NULL));
  snprintf(val, sizeof(val), "%d",
           (mask & HealthServiceAccessibilityMaskAvailable)
           ? (int)health_service_sum_today(HealthMetricStepCount) : 0);
  replace_in_buf(s_data_buf[idx], sizeof(s_data_buf[idx]), "%steps%", val);

  // %calories%
  mask = health_service_metric_accessible(HealthMetricActiveKCalories, time_start_of_today(), time(NULL));
  snprintf(val, sizeof(val), "%d",
           (mask & HealthServiceAccessibilityMaskAvailable)
           ? (int)health_service_sum_today(HealthMetricActiveKCalories) : 0);
  replace_in_buf(s_data_buf[idx], sizeof(s_data_buf[idx]), "%calories%", val);

  // %distance%
  mask = health_service_metric_accessible(HealthMetricWalkedDistanceMeters, time_start_of_today(), time(NULL));
  int meters = (mask & HealthServiceAccessibilityMaskAvailable)
               ? (int)health_service_sum_today(HealthMetricWalkedDistanceMeters) : 0;
  format_distance(val, sizeof(val), meters);
  replace_in_buf(s_data_buf[idx], sizeof(s_data_buf[idx]), "%distance%", val);

  // %heartrate%
  HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
  snprintf(val, sizeof(val), "%d", hr > 0 ? (int)hr : 0);
  replace_in_buf(s_data_buf[idx], sizeof(s_data_buf[idx]), "%heartrate%", val);

  // %battery%
  BatteryChargeState batt = battery_state_service_peek();
  snprintf(val, sizeof(val), "%d pct", batt.charge_percent);
  replace_in_buf(s_data_buf[idx], sizeof(s_data_buf[idx]), "%battery%", val);

  text_layer_set_text(s_data_layers[idx], s_data_buf[idx]);
}

static void update_data_lines(void) {
  for (int i = 0; i < 5; i++) {
    format_data_line(i);
  }
}

// ── Background draw proc ──────────────────────────────────────────────────────
static void background_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, s_paper_color);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, s_lines_color);
  graphics_context_set_stroke_width(ctx, 2);
  for (int y = LINE_STEP; y < SCREEN_H; y += LINE_STEP) {
    graphics_draw_line(ctx, GPoint(0, y), GPoint(SCREEN_W, y));
  }
}

// ── Show / hide screens ───────────────────────────────────────────────────────
static void apply_text_color(void) {
  text_layer_set_text_color(s_time_layer, s_text_color);
  text_layer_set_text_color(s_dow_layer,  s_text_color);
  text_layer_set_text_color(s_date_layer, s_text_color);
  for (int i = 0; i < 5; i++) {
    text_layer_set_text_color(s_data_layers[i], s_text_color);
  }
}

static void cancel_data_timer(void) {
  if (s_data_view_timer) {
    app_timer_cancel(s_data_view_timer);
    s_data_view_timer = NULL;
  }
}

static void return_to_time_view(void *ctx);

static void schedule_data_timer(void) {
  cancel_data_timer();
  if (s_data_timeout > 0) {
    s_data_view_timer = app_timer_register(
        (uint32_t)s_data_timeout * 1000, return_to_time_view, NULL);
  }
}

static void show_screen(bool data_screen) {
  s_show_data = data_screen;
  bool time_vis = !data_screen;
  layer_set_hidden(text_layer_get_layer(s_time_layer), !time_vis);
  layer_set_hidden(text_layer_get_layer(s_dow_layer),  !time_vis);
  layer_set_hidden(text_layer_get_layer(s_date_layer), !time_vis);
  for (int i = 0; i < 5; i++) {
    layer_set_hidden(text_layer_get_layer(s_data_layers[i]), time_vis);
  }
  if (data_screen) {
    update_data_lines();
    schedule_data_timer();
  } else {
    cancel_data_timer();
  }
}

static void return_to_time_view(void *ctx) {
  (void)ctx;
  s_data_view_timer = NULL;
  if (s_show_data) {
    show_screen(false);
  }
}

// ── Clock update ──────────────────────────────────────────────────────────────
static void update_clock(struct tm *tick_time) {
  // Time
  if (clock_is_24h_style()) {
    snprintf(s_time_buf, sizeof(s_time_buf), "%02d:%02d",
             tick_time->tm_hour, tick_time->tm_min);
  } else {
    int h = tick_time->tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(s_time_buf, sizeof(s_time_buf), "%d:%02d", h, tick_time->tm_min);
  }
  text_layer_set_text(s_time_layer, s_time_buf);

  // Day of week
  snprintf(s_dow_buf, sizeof(s_dow_buf), "Today is %s",
           DAYS[tick_time->tm_wday]);
  text_layer_set_text(s_dow_layer, s_dow_buf);

  // Date
  snprintf(s_date_buf, sizeof(s_date_buf), "%s %d, %d",
           MONTHS[tick_time->tm_mon],
           tick_time->tm_mday,
           1900 + tick_time->tm_year);
  text_layer_set_text(s_date_layer, s_date_buf);
}

// ── Tick handler ──────────────────────────────────────────────────────────────
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_clock(tick_time);
  if (s_show_data) {
    update_data_lines();
  }
}

// ── Tap handler ───────────────────────────────────────────────────────────────
static void tap_handler(AccelAxisType axis, int32_t direction) {
  (void)axis; (void)direction;
  time_t now = time(NULL);
  if (now - s_last_tap < 1) return;
  s_last_tap = now;

  show_screen(!s_show_data);
}

// ── AppMessage ────────────────────────────────────────────────────────────────
static void inbox_received(DictionaryIterator *iter, void *context) {
  bool color_changed = false;

  for (int i = 0; i < 5; i++) {
    Tuple *t = dict_find(iter, MESSAGE_KEY_DATA1 + i);
    if (t && t->type == TUPLE_CSTRING) {
      persist_write_string(PERSIST_DATA1 + i, t->value->cstring);
      strncpy(s_data_tmpl[i], t->value->cstring, sizeof(s_data_tmpl[i]) - 1);
      s_data_tmpl[i][sizeof(s_data_tmpl[i]) - 1] = '\0';
    }
  }

  Tuple *tp = dict_find(iter, MESSAGE_KEY_PAPER_COLOR);
  if (tp) {
    uint32_t hex = (uint32_t)tp->value->int32;
    persist_write_int(PERSIST_PAPER_COLOR, (int32_t)hex);
    s_paper_color = color_from_hex(hex);
    color_changed = true;
  }

  Tuple *tl = dict_find(iter, MESSAGE_KEY_LINES_COLOR);
  if (tl) {
    uint32_t hex = (uint32_t)tl->value->int32;
    persist_write_int(PERSIST_LINES_COLOR, (int32_t)hex);
    s_lines_color = color_from_hex(hex);
    color_changed = true;
  }

  Tuple *tt = dict_find(iter, MESSAGE_KEY_TEXT_COLOR);
  if (tt) {
    uint32_t hex = (uint32_t)tt->value->int32;
    persist_write_int(PERSIST_TEXT_COLOR, (int32_t)hex);
    s_text_color = color_from_hex(hex);
    color_changed = true;
  }

  Tuple *tto = dict_find(iter, MESSAGE_KEY_TIMEOUT);
  if (tto) {
    int v = (tto->type == TUPLE_CSTRING) ? atoi(tto->value->cstring)
                                          : (int)tto->value->int32;
    s_data_timeout = clamp_timeout(v);
    persist_write_int(PERSIST_DATA_TIMEOUT, s_data_timeout);
    if (s_show_data) {
      schedule_data_timer();
    } else {
      cancel_data_timer();
    }
  }

  if (color_changed) {
    apply_text_color();
    layer_mark_dirty(s_bg_layer);
  }

  if (s_show_data) update_data_lines();
}

// ── Window load / unload ──────────────────────────────────────────────────────
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  // Background layer
  s_bg_layer = layer_create(bounds);
  layer_set_update_proc(s_bg_layer, background_update_proc);
  layer_add_child(root, s_bg_layer);

  // Fonts
  s_font_time = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_BLACKPEN_128));
  s_font_text = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_BLACKPEN_42));

  // ── Time screen ────────────────────────────────────────────────────────────
  // Time: large, centered, top portion — oversized rect so font isn't clipped
  s_time_layer = text_layer_create(GRect(0, -10, bounds.size.w, 158));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, s_text_color);
  text_layer_set_font(s_time_layer, s_font_time);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_time_layer, GTextOverflowModeWordWrap);
  layer_add_child(s_bg_layer, text_layer_get_layer(s_time_layer));

  // Day of week — sits in section 4 (132–176)
  s_dow_layer = text_layer_create(GRect(0, 133, bounds.size.w, 46));
  text_layer_set_background_color(s_dow_layer, GColorClear);
  text_layer_set_text_color(s_dow_layer, s_text_color);
  text_layer_set_font(s_dow_layer, s_font_text);
  text_layer_set_text_alignment(s_dow_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_dow_layer, GTextOverflowModeWordWrap);
  layer_add_child(s_bg_layer, text_layer_get_layer(s_dow_layer));

  // Date — sits in section 5 (176–220), bottom at 222
  s_date_layer = text_layer_create(GRect(0, 176, bounds.size.w, 46));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, s_text_color);
  text_layer_set_font(s_date_layer, s_font_text);
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_date_layer, GTextOverflowModeWordWrap);
  layer_add_child(s_bg_layer, text_layer_get_layer(s_date_layer));

  // ── Data screen — each slot is LINE_STEP tall, 5 slots fit in 220px ─────────
  for (int i = 0; i < 5; i++) {
    int y = i * LINE_STEP + 2;
    s_data_layers[i] = text_layer_create(GRect(5, y, bounds.size.w - 10, DATA_LINE_H));
    text_layer_set_background_color(s_data_layers[i], GColorClear);
    text_layer_set_text_color(s_data_layers[i], s_text_color);
    text_layer_set_font(s_data_layers[i], s_font_text);
    text_layer_set_text_alignment(s_data_layers[i], GTextAlignmentLeft);
    text_layer_set_overflow_mode(s_data_layers[i], GTextOverflowModeWordWrap);
    layer_add_child(s_bg_layer, text_layer_get_layer(s_data_layers[i]));
  }

  // Start showing time screen
  show_screen(false);

  // Initial clock update
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  update_clock(t);
}

static void window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_dow_layer);
  text_layer_destroy(s_date_layer);
  for (int i = 0; i < 5; i++) {
    text_layer_destroy(s_data_layers[i]);
  }
  layer_destroy(s_bg_layer);
  fonts_unload_custom_font(s_font_time);
  fonts_unload_custom_font(s_font_text);
}

// ── Init / deinit ─────────────────────────────────────────────────────────────
static void init(void) {
  load_colors();
  load_templates();
  load_timeout();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);

  app_message_register_inbox_received(inbox_received);
  app_message_open(512, 64);
}

static void deinit(void) {
  cancel_data_timer();
  tick_timer_service_unsubscribe();
  accel_tap_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
