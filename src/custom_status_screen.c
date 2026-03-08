/*
 * custom_status_screen.c
 *
 * Custom ZMK OLED status screen for Corne — VERTICAL orientation (32×128)
 *
 * Layout (top to bottom):
 * ┌────────┐
 * │  QWRT  │  ← Layer name
 * │ ────── │
 * │  BT:0* │  ← BT profile (* = connected)
 * │ ────── │
 * │ ██████ │  ← Battery bar
 * │   85%  │  ← Battery %
 * │ ────── │
 * │WPM:045 │  ← WPM count
 * │        │
 * │ · · · ·│
 * │ · · ·██│  ← Dot matrix WPM graph
 * │ · ·████│    (dim = empty cell, bright = filled)
 * │ ·██████│
 * └────────┘
 *
 * Dot matrix grid: DOT_STEP px per cell (DOT_SIZE dot + DOT_GAP gap)
 * GRID_COLS columns of history, GRID_ROWS levels of WPM resolution.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>
#include <zmk/ble.h>
#include <zmk/battery.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ── Screen dimensions ─────────────────────────────────────── */
#define SCREEN_W 32
#define SCREEN_H 128

/* ── Battery bar dimensions ────────────────────────────────── */
#define BAT_BAR_W 24
#define BAT_BAR_H 6

/* ── Dot matrix graph settings ─────────────────────────────── */
#define DOT_SIZE 1                    /* px — size of each lit dot               */
#define DOT_GAP 1                     /* px — gap between dots                   */
#define DOT_STEP (DOT_SIZE + DOT_GAP) /* 2px per cell            */

#define GRAPH_W 30 /* canvas width  (fits in 32px screen)     */
#define GRAPH_H 44 /* canvas height (bottom portion of screen) */

#define GRID_COLS (GRAPH_W / DOT_STEP) /* 15 columns of history   */
#define GRID_ROWS (GRAPH_H / DOT_STEP) /* 22 rows of resolution   */

#define WPM_MAX 120 /* WPM value that fills the graph to top   */

/* Canvas pixel buffer — 1bpp for monochrome OLED */
static uint8_t canvas_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GRAPH_W, GRAPH_H)];

/* ── WPM history ring buffer ───────────────────────────────── */
static uint8_t wpm_history[GRID_COLS];
static uint8_t wpm_head = 0; /* points to the oldest entry           */

/* ── Layer display names ───────────────────────────────────── */
static const char *const layer_names[] = {
    "QWRT", /* 0 — Default  */
    "LOWR", /* 1 — Lower    */
    "RISE", /* 2 — Raise    */
    "ADJ",  /* 3 — Adjust   */
};
#define LAYER_COUNT ARRAY_SIZE(layer_names)

/* ── Widget handles ────────────────────────────────────────── */
static lv_obj_t *layer_label;
static lv_obj_t *bt_label;
static lv_obj_t *bat_bar;
static lv_obj_t *bat_label;
static lv_obj_t *wpm_label;
static lv_obj_t *wpm_canvas;

/* ── Dot matrix redraw ─────────────────────────────────────── */
/*
 * For each column (oldest→newest, left→right), calculate how many
 * rows should be lit based on the stored WPM value, then draw:
 *   - bright dot  for filled rows (from bottom up)
 *   - dim dot     for empty rows  (above the filled region)
 *
 * This produces the classic dot-matrix LED bargraph look.
 */
static void redraw_dot_matrix(void)
{
    /* Clear to black */
    lv_canvas_fill_bg(wpm_canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.bg_opa = LV_OPA_COVER;

    /* Dim colour for unfilled cells — gives the "off LED" feel */
    lv_color_t dim = lv_color_make(28, 28, 28);
    lv_color_t bright = lv_color_white();

    for (int col = 0; col < GRID_COLS; col++)
    {
        /* Map so oldest value is on the left */
        int idx = (wpm_head + col) % GRID_COLS;
        int val = wpm_history[idx];

        /* How many rows to light up (clamped) */
        int filled = (val * GRID_ROWS) / WPM_MAX;
        if (filled > GRID_ROWS)
            filled = GRID_ROWS;

        for (int row = 0; row < GRID_ROWS; row++)
        {
            /* row 0 = top of canvas; filled rows start from bottom */
            bool lit = (row >= (GRID_ROWS - filled));

            dsc.bg_color = lit ? bright : dim;

            int x = col * DOT_STEP;
            int y = row * DOT_STEP;

            lv_canvas_draw_rect(wpm_canvas, x, y, DOT_SIZE, DOT_SIZE, &dsc);
        }
    }

    lv_obj_invalidate(wpm_canvas);
}

/* ── Divider helper ────────────────────────────────────────── */
static lv_obj_t *make_divider(lv_obj_t *parent, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, SCREEN_W, 1);
    lv_obj_set_pos(line, 0, y);
    lv_obj_set_style_bg_color(line, lv_color_make(60, 60, 60), LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
    return line;
}

/* ── Update helpers ────────────────────────────────────────── */
static void update_layer(void)
{
    uint8_t active = zmk_keymap_highest_layer_active();
    if (active < LAYER_COUNT)
    {
        lv_label_set_text(layer_label, layer_names[active]);
    }
    else
    {
        lv_label_set_text_fmt(layer_label, "L:%02d", active);
    }
}

static void update_bt(void)
{
    int profile = zmk_ble_active_profile_index();
    bool connected = zmk_ble_active_profile_is_connected();
    lv_label_set_text_fmt(bt_label, "BT:%d%s", profile, connected ? "*" : " ");
}

static void update_battery(void)
{
    uint8_t level = zmk_battery_state_of_charge();
    lv_label_set_text_fmt(bat_label, "%d%%", level);
    lv_bar_set_value(bat_bar, (int32_t)level, LV_ANIM_OFF);
}

static void update_wpm(void)
{
    uint8_t wpm = zmk_wpm_get_state();
    lv_label_set_text_fmt(wpm_label, "WPM:%3d", wpm);

    /* Push into ring buffer — overwrite the oldest slot */
    wpm_history[wpm_head] = wpm;
    wpm_head = (wpm_head + 1) % GRID_COLS;

    redraw_dot_matrix();
}

/* ── ZMK event listeners ───────────────────────────────────── */
static int handle_wpm(const zmk_event_t *eh)
{
    update_wpm();
    return ZMK_EV_EVENT_BUBBLE;
}
static int handle_layer(const zmk_event_t *eh)
{
    update_layer();
    return ZMK_EV_EVENT_BUBBLE;
}
static int handle_bt(const zmk_event_t *eh)
{
    update_bt();
    return ZMK_EV_EVENT_BUBBLE;
}
static int handle_battery(const zmk_event_t *eh)
{
    update_battery();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(cs_wpm, handle_wpm);
ZMK_LISTENER(cs_layer, handle_layer);
ZMK_LISTENER(cs_bt, handle_bt);
ZMK_LISTENER(cs_battery, handle_battery);

ZMK_SUBSCRIPTION(cs_wpm, zmk_wpm_state_changed);
ZMK_SUBSCRIPTION(cs_layer, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(cs_bt, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(cs_battery, zmk_battery_state_changed);

/* ── Screen constructor ────────────────────────────────────── */
lv_obj_t *zmk_display_status_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    /* ── Layer label ──────────────────────────────────────── */
    layer_label = lv_label_create(screen);
    lv_obj_set_style_text_color(layer_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(layer_label, "----");
    lv_obj_align(layer_label, LV_ALIGN_TOP_MID, 0, 2);

    make_divider(screen, 14);

    /* ── BT profile label ─────────────────────────────────── */
    bt_label = lv_label_create(screen);
    lv_obj_set_style_text_color(bt_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(bt_label, "BT:- ");
    lv_obj_align(bt_label, LV_ALIGN_TOP_MID, 0, 18);

    make_divider(screen, 30);

    /* ── Battery bar ──────────────────────────────────────── */
    bat_bar = lv_bar_create(screen);
    lv_obj_set_size(bat_bar, BAT_BAR_W, BAT_BAR_H);
    lv_obj_align(bat_bar, LV_ALIGN_TOP_MID, 0, 34);
    lv_bar_set_range(bat_bar, 0, 100);
    lv_bar_set_value(bat_bar, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bat_bar, lv_color_make(60, 60, 60), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bat_bar, lv_color_white(), LV_PART_INDICATOR);

    /* ── Battery % label ──────────────────────────────────── */
    bat_label = lv_label_create(screen);
    lv_obj_set_style_text_color(bat_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(bat_label, "---%");
    lv_obj_align(bat_label, LV_ALIGN_TOP_MID, 0, 44);

    make_divider(screen, 56);

    /* ── WPM label ────────────────────────────────────────── */
    wpm_label = lv_label_create(screen);
    lv_obj_set_style_text_color(wpm_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(wpm_label, "WPM:  0");
    lv_obj_align(wpm_label, LV_ALIGN_TOP_MID, 0, 60);

    /* ── Dot matrix canvas ────────────────────────────────── */
    wpm_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(wpm_canvas, canvas_buf, GRAPH_W, GRAPH_H,
                         LV_IMG_CF_INDEXED_1BIT);
    lv_obj_align(wpm_canvas, LV_ALIGN_TOP_MID, 0, 74);

    /* Set the 1bpp palette: index 0 = black, index 1 = white */
    lv_canvas_set_palette(wpm_canvas, 0, lv_color_black());
    lv_canvas_set_palette(wpm_canvas, 1, lv_color_white());

    /* Initialise history to zero and draw the empty grid */
    memset(wpm_history, 0, sizeof(wpm_history));
    redraw_dot_matrix();

    /* ── Populate all widgets with current state ──────────── */
    update_layer();
    update_bt();
    update_battery();
    update_wpm();

    return screen;
}