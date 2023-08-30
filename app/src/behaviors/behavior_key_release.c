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

int behavior_key_release_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *ep = as_zmk_position_state_changed(ev);
    if (ep == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    static bool in_range_presses_exist = false;
  
    uint32_t pos = ep->position;
    uint8_t hal = zmk_keymap_highest_layer_active();
    LOG_DBG("***** %d position on layer %d changed", pos, hal);

    /*
      we define THE RANGE as being:
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

    if (ep->state) { /* we regard key presses only */
      if (pos == 6 || pos > 12 || hal == 0 || hal > 4) {
        /* we are out of range */
        in_range_presses_exist = false;
      } else {
        /* we are in range */
        if (in_range_presses_exist) {
          LOG_DBG("***** %d position on layer %d pressed", pos, hal);
          capture_event(ev);
          return ZMK_EV_EVENT_CAPTURED;
        }
        in_range_presses_exist = true; 
      }
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
