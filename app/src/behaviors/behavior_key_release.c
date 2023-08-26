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
#include <zmk/keymap.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);


#define ZMK_BHV_KEY_RELEASE_MAX_CAPTURED_EVENTS 12
const zmk_event_t *captured_events_kr[ZMK_BHV_KEY_RELEASE_MAX_CAPTURED_EVENTS] = {};


static int capture_event(const zmk_event_t *ev) {
    struct zmk_position_state_changed *ep = as_zmk_position_state_changed(ev);
    if (ep == NULL) {
      return 0;
    }
    
    for (int i = 0; i < ZMK_BHV_KEY_RELEASE_MAX_CAPTURED_EVENTS; i++) {
        if (captured_events_kr[i] == NULL) {
            captured_events_kr[i] = ev;
            return 0;
        }
    }
    LOG_ERR("no space left in event capture list");
    return -ENOMEM;
}


static void remove_captured_event(const zmk_event_t *ev) {
    for (int i = 0; i < ZMK_BHV_KEY_RELEASE_MAX_CAPTURED_EVENTS; i++) {
        if (captured_events_kr[i] == ev) {
          captured_events_kr[i] = NULL;
          return;
        }
    }
    LOG_ERR("unable to remove non-existing event");
}

static const zmk_event_t *find_captured_keydown_event(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_KEY_RELEASE_MAX_CAPTURED_EVENTS; i++) {
        const zmk_event_t *ev = captured_events_kr[i];
        if (ev == NULL) {
            continue;
        }
        // here we have an event and we can be sure that it is a position event (as we only capture those)
        struct zmk_position_state_changed *ep = as_zmk_position_state_changed(ev);
        if (ep->position == position && ep->state) {
            return ev;
        }
    }
    return NULL;
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


int behavior_key_release_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *ep = as_zmk_position_state_changed(ev);
    if (ep == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    static uint8_t hal_last = 0;
  
    uint32_t pos = ep->position;
    uint8_t hal = zmk_keymap_highest_layer_active();
    LOG_DBG("***** %d position on layer %d changed", pos, hal);

    if (ep->state) {
      // key presses only
      if (!(pos == 6 || pos > 12 || hal == 0 || hal > 4) && hal == hal_last) {
        /*
          we are in range where we want to act on key releases rather then presses
          - in the set of the 2 x 6 relevant keys
            0       1       2                   3       4       5
            7       8       9                   10      11      12
          - and on the relevant layers
            ALA1     1
            ALA2     2
            ALA1_CPY 3
            ALA2_CPY 4
          - and only if this not the first key press on a newly activated layer 
         */
        // 
        LOG_DBG("***** %d position on layer %d pressed", pos, hal);
        capture_event(ev);
        hal_last = hal; // update on all key presses
        return ZMK_EV_EVENT_CAPTURED;
      }
      hal_last = hal; // update on all key presses
    }

    // for every release:  
    const zmk_event_t *ek =  find_captured_keydown_event(ep->position);
    if(ek != NULL) {
      LOG_DBG("***** %d position on layer %d released ", pos, hal);
      remove_captured_event(ek);
      ZMK_EVENT_RAISE_AFTER(ek, behavior_key_release);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_key_release, behavior_key_release_listener);
ZMK_SUBSCRIPTION(behavior_key_release, zmk_position_state_changed);


#define KP_INST(n)                                                                                 \
    DEVICE_DT_INST_DEFINE(n, behavior_key_release_init, NULL, NULL, NULL, APPLICATION,               \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_key_release_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)
