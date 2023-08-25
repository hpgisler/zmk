/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_key_release

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);


#define ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS_KR 40
const zmk_event_t *captured_events_kr[ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS_KR] = {};


static int capture_event(const zmk_event_t *event) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS_KR; i++) {
        if (captured_events_kr[i] == NULL) {
            captured_events_kr[i] = event;
            return 0;
        }
    }
    return -ENOMEM;
}


static void remove_captured_event(const zmk_event_t *event) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS_KR; i++) {
        if (captured_events_kr[i] == event) {
          captured_events_kr[i] = NULL;
          return;
        }
    }
}



static const zmk_event_t *find_captured_keydown_event(uint32_t position) {
    const  zmk_event_t *last_match = NULL;
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS_KR; i++) {
        const zmk_event_t *eh = captured_events_kr[i];
        if (eh == NULL) {
            return last_match;
        }
        struct zmk_position_state_changed *position_event = as_zmk_position_state_changed(eh);
        if (position_event == NULL) {
            continue;
        }

        if (position_event->position == position && position_event->state) {
            last_match = eh;
        }
    }
    return last_match;
}



const struct zmk_listener zmk_listener_behavior_key_release;

static int behavior_key_release_init(const struct device *dev) { return 0; };

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
  return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);
    ZMK_EVENT_RAISE(
        zmk_keycode_state_changed_from_encoded(binding->param1, true, event.timestamp));
    return ZMK_EVENT_RAISE(
        zmk_keycode_state_changed_from_encoded(binding->param1, false, event.timestamp));
}

static const struct behavior_driver_api behavior_key_release_driver_api = {
    .binding_pressed = on_keymap_binding_pressed, .binding_released = on_keymap_binding_released};


int behavior_key_release_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->state) {
      capture_event(eh);
      return ZMK_EV_EVENT_CAPTURED;
    }

    const zmk_event_t *ek =  find_captured_keydown_event(ev->position);
    remove_captured_event(ek);
    ZMK_EVENT_RAISE_AT(ek, behavior_key_release);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_key_release, behavior_key_release_listener);
ZMK_SUBSCRIPTION(behavior_key_release, zmk_position_state_changed);
ZMK_SUBSCRIPTION(behavior_key_release, zmk_keycode_state_changed);


#define KP_INST(n)                                                                                 \
    DEVICE_DT_INST_DEFINE(n, behavior_key_release_init, NULL, NULL, NULL, APPLICATION,               \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_key_release_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)
