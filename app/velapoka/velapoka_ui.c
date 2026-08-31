/****************************************************************************
 * apps/app/velapoka/velapoka_ui.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include "velapoka_camera.h"
#include "velapoka_model.h"
#include "velapoka_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VELAPOKA_SCREEN_WIDTH  1024
#define VELAPOKA_SCREEN_HEIGHT 600
#define VELAPOKA_PANEL_Y       12
#define VELAPOKA_PANEL_HEIGHT  576
#define VELAPOKA_LEFT_X        12
#define VELAPOKA_LEFT_WIDTH    492
#define VELAPOKA_RIGHT_X       516
#define VELAPOKA_RIGHT_WIDTH   496
#define VELAPOKA_PREVIEW_WIDTH 460
#define VELAPOKA_PREVIEW_HEIGHT 259
#define VELAPOKA_PREVIEW_SIZE \
  (VELAPOKA_PREVIEW_WIDTH * VELAPOKA_PREVIEW_HEIGHT)

#define COLOR_SCREEN           lv_color_hex(0x07111f)
#define COLOR_PANEL            lv_color_hex(0x0d1b2a)
#define COLOR_CARD             lv_color_hex(0x13263a)
#define COLOR_CARD_ALT         lv_color_hex(0x0a1625)
#define COLOR_BORDER           lv_color_hex(0x294158)
#define COLOR_TEXT             lv_color_hex(0xe8f1f7)
#define COLOR_MUTED            lv_color_hex(0x8fa6b8)
#define COLOR_PRIMARY          lv_color_hex(0x18b6a4)
#define COLOR_PRIMARY_DARK     lv_color_hex(0x0c756d)
#define COLOR_SUCCESS          lv_color_hex(0x37d67a)
#define COLOR_WARNING          lv_color_hex(0xffb340)
#define COLOR_DANGER           lv_color_hex(0xff5d67)

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum velapoka_threshold_step_e
{
  VELAPOKA_THRESHOLD_DECREASE = 0,
  VELAPOKA_THRESHOLD_INCREASE
};

struct velapoka_ui_s
{
  lv_image_dsc_t preview_dsc;
  lv_obj_t *status_label;
  lv_obj_t *result_label;
  lv_obj_t *result_detail;
  lv_obj_t *metrics_result;
  lv_obj_t *metrics_similarity;
  lv_obj_t *metrics_difference;
  lv_obj_t *metrics_time;
  lv_obj_t *threshold_label;
  lv_obj_t *slider;
  lv_obj_t *camera_dot;
  lv_obj_t *storage_dot;
  lv_obj_t *history_label;
  lv_obj_t *preview_image;
  lv_obj_t *preview_placeholder;
  lv_obj_t *preview_card;
  lv_obj_t *difference_boxes[VELAPOKA_INSPECT_MAX_BOXES];
  lv_obj_t *fps_label;
  FAR uint16_t *preview_pixels;
  uint16_t gray_rgb565[256];
  uint16_t preview_xmap[VELAPOKA_PREVIEW_WIDTH];
  uint16_t preview_ymap[VELAPOKA_PREVIEW_HEIGHT];
  struct timespec fps_started;
  uint32_t last_sequence;
  unsigned int fps_frames;
  unsigned int action_count;
  enum velapoka_ui_action_e pending_action;
  bool preview_seen;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct velapoka_ui_s g_ui;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static lv_obj_t *velapoka_panel_create(lv_obj_t *parent, int32_t x,
                                       int32_t width)
{
  lv_obj_t *panel = lv_obj_create(parent);

  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(panel, x, VELAPOKA_PANEL_Y);
  lv_obj_set_size(panel, width, VELAPOKA_PANEL_HEIGHT);
  lv_obj_set_style_radius(panel, 18, 0);
  lv_obj_set_style_bg_color(panel, COLOR_PANEL, 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, COLOR_BORDER, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  return panel;
}

static lv_obj_t *velapoka_card_create(lv_obj_t *parent, int32_t x,
                                      int32_t y, int32_t width,
                                      int32_t height)
{
  lv_obj_t *card = lv_obj_create(parent);

  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, COLOR_BORDER, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  return card;
}

static lv_obj_t *velapoka_label_create(lv_obj_t *parent, const char *text,
                                       int32_t x, int32_t y,
                                       lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

static void velapoka_button_style(lv_obj_t *button, lv_color_t color)
{
  lv_obj_set_style_radius(button, 10, 0);
  lv_obj_set_style_bg_color(button, color, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_bg_color(button, lv_color_lighten(color, LV_OPA_20),
                            LV_STATE_PRESSED);
}

static void velapoka_status_set(const char *status, lv_color_t color)
{
  lv_label_set_text_fmt(g_ui.status_label, "SYSTEM  %s", status);
  lv_obj_set_style_text_color(g_ui.status_label, color, 0);
}

static void velapoka_action_event(lv_event_t *event)
{
  enum velapoka_ui_action_e action =
    (enum velapoka_ui_action_e)(uintptr_t)lv_event_get_user_data(event);
  lv_event_code_t code = lv_event_get_code(event);

  if (code == LV_EVENT_LONG_PRESSED && action == VELAPOKA_UI_ACTION_STOP)
    {
      action = VELAPOKA_UI_ACTION_EXIT;
    }
  else if (code != LV_EVENT_CLICKED)
    {
      return;
    }

  g_ui.action_count++;
  g_ui.pending_action = action;
  syslog(LOG_INFO, "VelaPoka UI: action=%u click=%u\n",
         (unsigned int)action, g_ui.action_count);
}

static lv_obj_t *velapoka_button_create(lv_obj_t *parent, const char *text,
                                        int32_t x, int32_t width,
                                        lv_color_t color,
                                        enum velapoka_ui_action_e action)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  lv_obj_set_pos(button, x, 292);
  lv_obj_set_size(button, width, 54);
  velapoka_button_style(button, color);
  lv_obj_add_event_cb(button, velapoka_action_event, LV_EVENT_ALL,
                      (FAR void *)(uintptr_t)action);

  label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
  lv_obj_center(label);
  return button;
}

static void velapoka_slider_event(lv_event_t *event)
{
  int32_t value = lv_slider_get_value(g_ui.slider);

  lv_label_set_text_fmt(g_ui.threshold_label, "Threshold  %" PRId32 "%%",
                        value);

  if (lv_event_get_code(event) == LV_EVENT_RELEASED)
    {
      syslog(LOG_INFO, "VelaPoka UI: threshold=%" PRId32 "%%\n", value);
    }
}

static void velapoka_threshold_step_event(lv_event_t *event)
{
  enum velapoka_threshold_step_e action =
    (enum velapoka_threshold_step_e)(uintptr_t)
      lv_event_get_user_data(event);
  int32_t value;

  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  value = lv_slider_get_value(g_ui.slider);
  value += action == VELAPOKA_THRESHOLD_INCREASE ? 1 : -1;
  lv_slider_set_value(g_ui.slider, value, LV_ANIM_OFF);
  value = lv_slider_get_value(g_ui.slider);
  lv_label_set_text_fmt(g_ui.threshold_label, "Threshold  %" PRId32 "%%",
                        value);
  syslog(LOG_INFO, "VelaPoka UI: threshold=%" PRId32 "%%\n", value);
}

static lv_obj_t *velapoka_threshold_button_create(
  lv_obj_t *parent, const char *text, int32_t x,
  enum velapoka_threshold_step_e action)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  lv_obj_set_pos(button, x, 235);
  lv_obj_set_size(button, 34, 34);
  velapoka_button_style(button, COLOR_PRIMARY_DARK);
  lv_obj_add_event_cb(button, velapoka_threshold_step_event,
                      LV_EVENT_CLICKED, (FAR void *)(uintptr_t)action);

  label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_obj_center(label);
  return button;
}

static lv_obj_t *velapoka_device_badge(lv_obj_t *parent, const char *name,
                                       int32_t x, bool online)
{
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_t *label;

  lv_obj_remove_style_all(dot);
  lv_obj_set_pos(dot, x, 62);
  lv_obj_set_size(dot, 9, 9);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, online ? COLOR_SUCCESS : COLOR_DANGER, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

  label = velapoka_label_create(parent, name, x + 14, 57, COLOR_MUTED);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  return dot;
}

static void velapoka_left_create(lv_obj_t *screen, bool touch_online,
                                 bool camera_online, bool storage_online)
{
  lv_obj_t *panel = velapoka_panel_create(screen, VELAPOKA_LEFT_X,
                                          VELAPOKA_LEFT_WIDTH);
  lv_obj_t *label;
  lv_obj_t *card;

  label = velapoka_label_create(panel, "VelaPoka", 24, 18, COLOR_TEXT);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
  label = velapoka_label_create(panel, "VISUAL ASSEMBLY INSPECTOR",
                                25, 52, COLOR_MUTED);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);

  g_ui.status_label = velapoka_label_create(panel, "SYSTEM  READY",
                                             270, 29, COLOR_SUCCESS);
  lv_obj_set_width(g_ui.status_label, 204);
  lv_label_set_long_mode(g_ui.status_label, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(g_ui.status_label, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(g_ui.status_label, &lv_font_montserrat_16, 0);

  card = velapoka_card_create(panel, 18, 86, 456, 102);
  label = velapoka_label_create(card, "DEVICE STATUS", 16, 13, COLOR_TEXT);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  g_ui.camera_dot = velapoka_device_badge(card, "CAMERA", 16,
                                           camera_online);
  velapoka_device_badge(card, "DISPLAY", 126, true);
  velapoka_device_badge(card, "TOUCH", 244, touch_online);
  g_ui.storage_dot = velapoka_device_badge(card, "SD", 344,
                                            storage_online);

  label = velapoka_label_create(panel, "Current sample", 22, 208,
                                COLOR_MUTED);
  label = velapoka_label_create(panel, "Connector Standard A-01", 142, 205,
                                COLOR_TEXT);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);

  g_ui.threshold_label = velapoka_label_create(panel, "Threshold  18%",
                                                22, 247, COLOR_TEXT);
  velapoka_threshold_button_create(panel, "-", 142,
                                   VELAPOKA_THRESHOLD_DECREASE);
  g_ui.slider = lv_slider_create(panel);
  lv_obj_set_pos(g_ui.slider, 190, 244);
  lv_obj_set_size(g_ui.slider, 214, 16);
  lv_slider_set_range(g_ui.slider, 5, 40);
  lv_slider_set_value(g_ui.slider, 18, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(g_ui.slider, COLOR_BORDER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_ui.slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(g_ui.slider, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_ui.slider, COLOR_MUTED, LV_PART_MAIN);
  lv_obj_set_style_radius(g_ui.slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_ui.slider, COLOR_PRIMARY,
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(g_ui.slider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(g_ui.slider, LV_RADIUS_CIRCLE,
                          LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(g_ui.slider, COLOR_TEXT, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(g_ui.slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_border_width(g_ui.slider, 3, LV_PART_KNOB);
  lv_obj_set_style_border_color(g_ui.slider, COLOR_PRIMARY, LV_PART_KNOB);
  lv_obj_set_style_pad_all(g_ui.slider, 5, LV_PART_KNOB);
  lv_obj_add_event_cb(g_ui.slider, velapoka_slider_event,
                      LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(g_ui.slider, velapoka_slider_event,
                      LV_EVENT_RELEASED, NULL);
  velapoka_threshold_button_create(panel, "+", 418,
                                   VELAPOKA_THRESHOLD_INCREASE);
  label = velapoka_label_create(panel, "Drag slider or tap - / +",
                                190, 271, COLOR_MUTED);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);

  velapoka_button_create(panel, "Enroll", 18, 104, COLOR_PRIMARY_DARK,
                         VELAPOKA_UI_ACTION_ENROLL);
  velapoka_button_create(panel, "Inspect", 132, 104, COLOR_PRIMARY,
                         VELAPOKA_UI_ACTION_INSPECT);
  velapoka_button_create(panel, "Stop", 246, 104, COLOR_WARNING,
                         VELAPOKA_UI_ACTION_STOP);
  velapoka_button_create(panel, "Exit", 360, 104, COLOR_DANGER,
                         VELAPOKA_UI_ACTION_EXIT);

  card = velapoka_card_create(panel, 18, 365, 456, 126);
  label = velapoka_label_create(card, "LATEST RESULT", 16, 13,
                                COLOR_MUTED);
  g_ui.result_label = velapoka_label_create(card, "READY", 16, 39,
                                             COLOR_SUCCESS);
  lv_obj_set_style_text_font(g_ui.result_label, &lv_font_montserrat_28, 0);
  g_ui.result_detail = velapoka_label_create(card,
                                              "Score --  |  Time -- ms",
                                              16, 83, COLOR_TEXT);

  label = velapoka_label_create(panel, "RECENT   No saved results",
                                22, 524, COLOR_MUTED);
  g_ui.history_label = label;
  lv_obj_set_width(label, 448);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
}

static void velapoka_right_create(lv_obj_t *screen, bool camera_online)
{
  lv_obj_t *panel = velapoka_panel_create(screen, VELAPOKA_RIGHT_X,
                                          VELAPOKA_RIGHT_WIDTH);
  lv_obj_t *preview;
  lv_obj_t *roi;
  lv_obj_t *card;
  lv_obj_t *label;

  label = velapoka_label_create(panel, "LIVE", 20, 20, COLOR_SUCCESS);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  g_ui.fps_label = velapoka_label_create(panel,
                                          camera_online ? "0 FPS" :
                                                          "OFFLINE",
                                          82, 25, COLOR_MUTED);
  label = velapoka_label_create(panel, "PRODUCT  A-01", 345, 25,
                                COLOR_TEXT);

  preview = velapoka_card_create(panel, 18, 62, 460, 259);
  g_ui.preview_card = preview;
  lv_obj_set_style_bg_color(preview, COLOR_CARD_ALT, 0);
  g_ui.preview_image = lv_image_create(preview);
  lv_obj_set_pos(g_ui.preview_image, 0, 0);
  lv_obj_set_size(g_ui.preview_image, VELAPOKA_PREVIEW_WIDTH,
                  VELAPOKA_PREVIEW_HEIGHT);
  lv_image_set_src(g_ui.preview_image, &g_ui.preview_dsc);

  g_ui.preview_placeholder =
    velapoka_label_create(preview,
                          camera_online ? "WAITING FOR CAMERA" :
                                          "CAMERA OFFLINE",
                          139, 113, COLOR_MUTED);

  roi = lv_obj_create(preview);
  lv_obj_remove_flag(roi, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(roi,
                 VELAPOKA_ROI_X * VELAPOKA_PREVIEW_WIDTH /
                 VELAPOKA_INSPECT_WIDTH,
                 VELAPOKA_ROI_Y * VELAPOKA_PREVIEW_HEIGHT /
                 VELAPOKA_INSPECT_HEIGHT);
  lv_obj_set_size(roi,
                  VELAPOKA_ROI_WIDTH * VELAPOKA_PREVIEW_WIDTH /
                  VELAPOKA_INSPECT_WIDTH,
                  VELAPOKA_ROI_HEIGHT * VELAPOKA_PREVIEW_HEIGHT /
                  VELAPOKA_INSPECT_HEIGHT);
  lv_obj_set_style_bg_opa(roi, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(roi, 2, 0);
  lv_obj_set_style_border_color(roi, COLOR_PRIMARY, 0);
  lv_obj_set_style_radius(roi, 2, 0);
  lv_obj_set_style_pad_all(roi, 0, 0);
  label = velapoka_label_create(roi, "ROI", 8, 6, COLOR_PRIMARY);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);

  card = velapoka_card_create(panel, 18, 337, 284, 154);
  velapoka_label_create(card, "INSPECTION METRICS", 14, 12, COLOR_MUTED);
  label = velapoka_label_create(card, "--", 14, 40, COLOR_MUTED);
  g_ui.metrics_result = label;
  lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
  velapoka_label_create(card, "Similarity", 14, 89, COLOR_MUTED);
  g_ui.metrics_similarity =
    velapoka_label_create(card, "--", 118, 87, COLOR_TEXT);
  velapoka_label_create(card, "Difference", 14, 117, COLOR_MUTED);
  g_ui.metrics_difference =
    velapoka_label_create(card, "--", 118, 115, COLOR_TEXT);
  velapoka_label_create(card, "Time", 192, 89, COLOR_MUTED);
  g_ui.metrics_time =
    velapoka_label_create(card, "--", 192, 115, COLOR_TEXT);

  card = velapoka_card_create(panel, 316, 337, 162, 72);
  velapoka_label_create(card, "REFERENCE", 12, 10, COLOR_MUTED);
  velapoka_label_create(card, "thumbnail", 47, 39, COLOR_TEXT);
  card = velapoka_card_create(panel, 316, 419, 162, 72);
  velapoka_label_create(card, "DIFFERENCE", 12, 10, COLOR_MUTED);
  velapoka_label_create(card, "thumbnail", 47, 39, COLOR_TEXT);

  label = velapoka_label_create(panel,
                                "Red boxes mark missing or misplaced parts",
                                20, 524, COLOR_WARNING);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velapoka_ui_create(bool touch_online, bool camera_online,
                       bool storage_online)
{
  lv_obj_t *screen = lv_screen_active();

  if (lv_display_get_horizontal_resolution(NULL) != VELAPOKA_SCREEN_WIDTH ||
      lv_display_get_vertical_resolution(NULL) != VELAPOKA_SCREEN_HEIGHT)
    {
      syslog(LOG_ERR, "VelaPoka UI: expected %dx%d display\n",
             VELAPOKA_SCREEN_WIDTH, VELAPOKA_SCREEN_HEIGHT);
      return -ERANGE;
    }

  if (g_ui.preview_pixels == NULL)
    {
      g_ui.preview_pixels =
        memalign(64, VELAPOKA_PREVIEW_SIZE * sizeof(uint16_t));
      if (g_ui.preview_pixels == NULL)
        {
          return -ENOMEM;
        }
    }

  memset(g_ui.preview_pixels, 0,
         VELAPOKA_PREVIEW_SIZE * sizeof(uint16_t));
  memset(&g_ui.preview_dsc, 0, sizeof(g_ui.preview_dsc));
  g_ui.preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  g_ui.preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  g_ui.preview_dsc.header.w = VELAPOKA_PREVIEW_WIDTH;
  g_ui.preview_dsc.header.h = VELAPOKA_PREVIEW_HEIGHT;
  g_ui.preview_dsc.header.stride =
    VELAPOKA_PREVIEW_WIDTH * sizeof(uint16_t);
  g_ui.preview_dsc.data_size =
    VELAPOKA_PREVIEW_SIZE * sizeof(uint16_t);
  g_ui.preview_dsc.data = (FAR const uint8_t *)g_ui.preview_pixels;
  g_ui.preview_seen = false;
  g_ui.fps_frames = 0;
  g_ui.pending_action = VELAPOKA_UI_ACTION_NONE;

  for (unsigned int i = 0; i <= UINT8_MAX; i++)
    {
      g_ui.gray_rgb565[i] = ((i & 0xf8) << 8) |
                            ((i & 0xfc) << 3) | (i >> 3);
    }

  for (unsigned int i = 0; i < VELAPOKA_PREVIEW_WIDTH; i++)
    {
      g_ui.preview_xmap[i] = i * VELAPOKA_CAMERA_PREVIEW_WIDTH /
                             VELAPOKA_PREVIEW_WIDTH;
    }

  for (unsigned int i = 0; i < VELAPOKA_PREVIEW_HEIGHT; i++)
    {
      g_ui.preview_ymap[i] = i * VELAPOKA_CAMERA_PREVIEW_HEIGHT /
                             VELAPOKA_PREVIEW_HEIGHT;
    }

  lv_obj_clean(screen);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(screen, COLOR_SCREEN, 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);

  velapoka_left_create(screen, touch_online, camera_online,
                       storage_online);
  velapoka_right_create(screen, camera_online);
  for (unsigned int i = 0; i < VELAPOKA_INSPECT_MAX_BOXES; i++)
    {
      g_ui.difference_boxes[i] = lv_obj_create(g_ui.preview_card);
      lv_obj_remove_flag(g_ui.difference_boxes[i], LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_style_bg_opa(g_ui.difference_boxes[i], LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(g_ui.difference_boxes[i], 3, 0);
      lv_obj_set_style_border_color(g_ui.difference_boxes[i],
                                    COLOR_DANGER, 0);
      lv_obj_set_style_radius(g_ui.difference_boxes[i], 2, 0);
      lv_obj_set_style_pad_all(g_ui.difference_boxes[i], 0, 0);
      lv_obj_add_flag(g_ui.difference_boxes[i], LV_OBJ_FLAG_HIDDEN);
    }

  lv_obj_invalidate(screen);
  return OK;
}

int velapoka_ui_update_preview(FAR const uint8_t *gray, size_t size,
                               uint32_t sequence)
{
  struct timespec now;
  uint64_t elapsed_ms;
  unsigned int x;
  unsigned int y;
  unsigned int fps_tenths;

  if (gray == NULL || size != VELAPOKA_CAMERA_PREVIEW_SIZE ||
      g_ui.preview_pixels == NULL || g_ui.preview_image == NULL)
    {
      return -EINVAL;
    }

  if (g_ui.preview_seen && sequence == g_ui.last_sequence)
    {
      return OK;
    }

  for (y = 0; y < VELAPOKA_PREVIEW_HEIGHT; y++)
    {
      FAR const uint8_t *src =
        gray + g_ui.preview_ymap[y] * VELAPOKA_CAMERA_PREVIEW_WIDTH;
      FAR uint16_t *dst =
        g_ui.preview_pixels + y * VELAPOKA_PREVIEW_WIDTH;

      for (x = 0; x < VELAPOKA_PREVIEW_WIDTH; x++)
        {
          dst[x] = g_ui.gray_rgb565[src[g_ui.preview_xmap[x]]];
        }
    }

  g_ui.last_sequence = sequence;
  lv_obj_add_flag(g_ui.preview_placeholder, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(g_ui.preview_image);

  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!g_ui.preview_seen)
    {
      g_ui.preview_seen = true;
      g_ui.fps_started = now;
      g_ui.fps_frames = 1;
      return OK;
    }

  g_ui.fps_frames++;
  elapsed_ms = (uint64_t)(now.tv_sec - g_ui.fps_started.tv_sec) * 1000;
  if (now.tv_nsec >= g_ui.fps_started.tv_nsec)
    {
      elapsed_ms += (now.tv_nsec - g_ui.fps_started.tv_nsec) / 1000000;
    }
  else
    {
      elapsed_ms -= 1000;
      elapsed_ms += (1000000000 + now.tv_nsec -
                     g_ui.fps_started.tv_nsec) / 1000000;
    }

  if (elapsed_ms >= 1000)
    {
      fps_tenths = (unsigned int)
        ((uint64_t)g_ui.fps_frames * 10000 / elapsed_ms);
      lv_label_set_text_fmt(g_ui.fps_label, "%u.%u FPS",
                            fps_tenths / 10, fps_tenths % 10);
      g_ui.fps_started = now;
      g_ui.fps_frames = 0;
    }

  return OK;
}

void velapoka_ui_set_camera_online(bool online)
{
  if (g_ui.camera_dot != NULL)
    {
      lv_obj_set_style_bg_color(g_ui.camera_dot,
                                online ? COLOR_SUCCESS : COLOR_DANGER, 0);
    }

  if (g_ui.fps_label != NULL)
    {
      lv_label_set_text(g_ui.fps_label, online ? "0 FPS" : "OFFLINE");
    }

  if (!online && g_ui.preview_placeholder != NULL)
    {
      lv_label_set_text(g_ui.preview_placeholder, "CAMERA OFFLINE");
      lv_obj_remove_flag(g_ui.preview_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

void velapoka_ui_set_storage_online(bool online)
{
  if (g_ui.storage_dot != NULL)
    {
      lv_obj_set_style_bg_color(g_ui.storage_dot,
                                online ? COLOR_SUCCESS : COLOR_DANGER, 0);
    }
}

enum velapoka_ui_action_e velapoka_ui_take_action(void)
{
  enum velapoka_ui_action_e action = g_ui.pending_action;

  g_ui.pending_action = VELAPOKA_UI_ACTION_NONE;
  return action;
}

unsigned int velapoka_ui_get_threshold(void)
{
  return g_ui.slider == NULL ? 18 : lv_slider_get_value(g_ui.slider);
}

void velapoka_ui_set_threshold(unsigned int threshold_percent)
{
  if (g_ui.slider == NULL)
    {
      return;
    }

  lv_slider_set_value(g_ui.slider, threshold_percent, LV_ANIM_OFF);
  threshold_percent = lv_slider_get_value(g_ui.slider);
  lv_label_set_text_fmt(g_ui.threshold_label, "Threshold  %u%%",
                        threshold_percent);
}

void velapoka_ui_set_state(enum velapoka_state_e state)
{
  lv_color_t color = COLOR_PRIMARY;

  if (state == VELAPOKA_STATE_READY || state == VELAPOKA_STATE_PASS)
    {
      color = COLOR_SUCCESS;
    }
  else if (state == VELAPOKA_STATE_SAMPLE_CAPTURE ||
           state == VELAPOKA_STATE_SAVE_SAMPLE)
    {
      color = COLOR_WARNING;
    }
  else if (state == VELAPOKA_STATE_FAIL || state == VELAPOKA_STATE_ERROR)
    {
      color = COLOR_DANGER;
    }
  else if (state == VELAPOKA_STATE_IDLE)
    {
      color = COLOR_MUTED;
    }

  velapoka_status_set(velapoka_state_name(state), color);
}

void velapoka_ui_set_enroll_progress(unsigned int captured,
                                     unsigned int total)
{
  lv_label_set_text(g_ui.result_label, "ENROLLING");
  lv_obj_set_style_text_color(g_ui.result_label, COLOR_WARNING, 0);
  lv_label_set_text_fmt(g_ui.result_detail, "Reference frame %u / %u",
                        captured, total);
}

void velapoka_ui_set_message(FAR const char *title,
                             FAR const char *detail, bool error)
{
  lv_label_set_text(g_ui.result_label, title);
  lv_obj_set_style_text_color(g_ui.result_label,
                              error ? COLOR_DANGER : COLOR_TEXT, 0);
  lv_label_set_text(g_ui.result_detail, detail);
}

void velapoka_ui_clear_boxes(void)
{
  unsigned int i;

  for (i = 0; i < VELAPOKA_INSPECT_MAX_BOXES; i++)
    {
      if (g_ui.difference_boxes[i] != NULL)
        {
          lv_obj_add_flag(g_ui.difference_boxes[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void velapoka_ui_set_result(FAR const struct velapoka_result_s *result)
{
  lv_color_t color;
  unsigned int i;

  if (result == NULL)
    {
      return;
    }

  color = result->pass ? COLOR_SUCCESS : COLOR_DANGER;
  lv_label_set_text(g_ui.result_label, result->pass ? "PASS" : "FAIL");
  lv_obj_set_style_text_color(g_ui.result_label, color, 0);
  lv_label_set_text_fmt(g_ui.result_detail,
                        "Score %u.%u  |  Diff %u.%u%%  |  %" PRIu32 " ms",
                        result->score_tenths / 10,
                        result->score_tenths % 10,
                        result->difference_tenths / 10,
                        result->difference_tenths % 10,
                        result->elapsed_ms);
  lv_label_set_text(g_ui.metrics_result, result->pass ? "PASS" : "FAIL");
  lv_obj_set_style_text_color(g_ui.metrics_result, color, 0);
  lv_label_set_text_fmt(g_ui.metrics_similarity, "%u.%u%%",
                        result->score_tenths / 10,
                        result->score_tenths % 10);
  lv_label_set_text_fmt(g_ui.metrics_difference, "%u.%u%%",
                        result->difference_tenths / 10,
                        result->difference_tenths % 10);
  lv_label_set_text_fmt(g_ui.metrics_time, "%" PRIu32 " ms",
                        result->elapsed_ms);

  velapoka_ui_clear_boxes();
  for (i = 0; i < result->box_count; i++)
    {
      FAR const struct velapoka_box_s *box = &result->boxes[i];
      lv_obj_t *object = g_ui.difference_boxes[i];

      lv_obj_set_pos(object,
                     box->x * VELAPOKA_PREVIEW_WIDTH /
                     VELAPOKA_INSPECT_WIDTH,
                     box->y * VELAPOKA_PREVIEW_HEIGHT /
                     VELAPOKA_INSPECT_HEIGHT);
      lv_obj_set_size(object,
                      box->width * VELAPOKA_PREVIEW_WIDTH /
                      VELAPOKA_INSPECT_WIDTH,
                      box->height * VELAPOKA_PREVIEW_HEIGHT /
                      VELAPOKA_INSPECT_HEIGHT);
      lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void velapoka_ui_set_history(FAR const struct velapoka_history_s *history,
                             unsigned int count)
{
  char text[160];
  size_t used;
  unsigned int i;

  if (g_ui.history_label == NULL)
    {
      return;
    }

  used = strlcpy(text, "RECENT  ", sizeof(text));
  if (history == NULL || count == 0)
    {
      strlcpy(text + used, " No saved results", sizeof(text) - used);
    }
  else
    {
      for (i = 0; i < count && used < sizeof(text); i++)
        {
          int length = snprintf(text + used, sizeof(text) - used,
                                " #%" PRIu32 " %s",
                                history[i].id,
                                history[i].pass ? "PASS" : "FAIL");

          if (length < 0 || (size_t)length >= sizeof(text) - used)
            {
              break;
            }

          used += length;
        }
    }

  lv_label_set_text(g_ui.history_label, text);
}
