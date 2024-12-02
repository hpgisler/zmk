/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_delay

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE 12

const struct zmk_listener zmk_listener_delay;

struct delay_cfg {
  int32_t timeout_ms;
  int32_t layer_key_positions_len;
  int32_t layer_key_positions[];
};

struct delay_cfg *delay_config;

struct {
    // CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE
    // theoretical limit, can be reached only
    // if typing is very fast or delay is very large 
    // array: sort order ascending according key_positions_pressed timestamp (occurrence)
    struct zmk_position_state_changed_event ev[CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE];
    struct zmk_position_state_changed_event *oldest, *next;
} past_presses;

struct k_work_delayable timeout_task;

static void advance(struct zmk_position_state_changed_event** it);
static void recede(struct zmk_position_state_changed_event** it);


static int initialize_delay(struct delay_cfg *delay_cfg) {
  // I assume here, that only one delay config exists on keymap file
  delay_config = delay_cfg;
    return 0;
}

static int cleanup();

static int capture_pressed_key(const struct zmk_position_state_changed *evdata) {
  if (past_presses.next == past_presses.oldest) {
    // no space left in ring buffer
    LOG_ERR("Unable to delay position down event; already %d delayed. Increase "
            "CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE",
            CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE);
    return ZMK_EV_EVENT_BUBBLE;
  }
  *past_presses.next = copy_raised_zmk_position_state_changed(evdata);
  if (past_presses.oldest == NULL) {
    past_presses.oldest = past_presses.next;
  }
  advance(&past_presses.next);
  return ZMK_EV_EVENT_CAPTURED;
}

static bool release_oldest_of_past_presses() {
  if (!past_presses.oldest) {
    return false;
  }
  ZMK_EVENT_RAISE_AFTER(*past_presses.oldest, delay);
  advance(&past_presses.oldest);
  if (past_presses.oldest == past_presses.next)
    past_presses.oldest = NULL;
  return true;
}

static void release_captured_key_presses(struct zmk_position_state_changed *evdata) {
  if (past_presses.oldest == NULL) {
    // no space left in ring buffer
    LOG_ERR("No captured key position event to release; ring buffer is empty");
    return;
  }
  struct zmk_position_state_changed_event *it_released = past_presses.next;
  do {
    recede(&it_released);
  } while (it_released->data.position != evdata->position);
  while (true) {
    if (!release_oldest_of_past_presses() || past_presses.oldest == it_released)
      break;
  };
}

static void advance(struct zmk_position_state_changed_event** it) {
  (*it)++;
  if (*it == &past_presses.ev[CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE]) {
    *it = &past_presses.ev[0];
  }
}

static void recede(struct zmk_position_state_changed_event** it) {
  (*it)--;
  if (*it < &past_presses.ev[0]) {
    *it = &past_presses.ev[CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE - 1];
  }
}


static int cleanup() {
    return k_work_cancel_delayable(&timeout_task);
}

static void update_timeout_task() {
  if (past_presses.oldest == NULL) {
    cleanup();
    return;
  }
  int64_t time_since_oldest_ms = k_uptime_get() - past_presses.oldest->data.timestamp;
  int64_t due_in_ms = delay_config->timeout_ms - time_since_oldest_ms;
  if (due_in_ms > 1) {
    k_work_schedule(&timeout_task, K_MSEC(due_in_ms));
    return;
  }
  k_work_schedule(&timeout_task, K_MSEC(1)); // fallback
}

bool is_delayed_keypos(struct zmk_position_state_changed *evdata) {
  for (int p = 0; p < delay_config->layer_key_positions_len ; p++) {
    if (evdata->position == delay_config->layer_key_positions[p]) {
      return false;
    }
  }
  return true;
}

static int position_state_down(struct zmk_position_state_changed *evdata) {
  if (!is_delayed_keypos(evdata)) {
    return ZMK_EV_EVENT_BUBBLE;
  }
  int evt = capture_pressed_key(evdata);
  update_timeout_task();
  return evt;
}

static int position_state_up(struct zmk_position_state_changed *evdata) {
  if (is_delayed_keypos(evdata)) {
    release_captured_key_presses(evdata);
  }
  return ZMK_EV_EVENT_BUBBLE;
}

static void delay_timeout_handler(struct k_work *item) {
  if (!release_oldest_of_past_presses()) {
    cleanup();
    return;
  }
  update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *evdata = as_zmk_position_state_changed(ev);
    if (evdata == NULL) {
        return 0;
    }
    if (evdata->state) { // key down
        return position_state_down(evdata);
    } else { // key up
        return position_state_up(evdata);
    }
}

ZMK_LISTENER(delay, position_state_changed_listener);
ZMK_SUBSCRIPTION(delay, zmk_position_state_changed);

#define DELAY_INST(n)                                                                              \
    static struct delay_cfg delay_cfg_##n = {                                                   \
        .timeout_ms = DT_PROP(n, timeout_ms),                                                      \
        .layer_key_positions = DT_PROP(n, layer_key_positions),                                    \
        .layer_key_positions_len = DT_PROP_LEN(n, layer_key_positions),                            \
    };

#define INITIALIZE_DELAY(n) initialize_delay(&delay_cfg_##n);

DT_INST_FOREACH_CHILD(0, DELAY_INST)

static int delay_init() {
    k_work_init_delayable(&timeout_task, delay_timeout_handler);
    past_presses.next = &past_presses.ev[0]; 
    past_presses.oldest = NULL; 
    DT_INST_FOREACH_CHILD(0, INITIALIZE_DELAY);
    return 0;
}

SYS_INIT(delay_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif
