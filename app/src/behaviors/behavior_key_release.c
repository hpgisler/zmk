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

/* Layers */
#define ALA0        0
#define ALA1        1
#define ALA2        2
#define ALA1_CPY    3
#define ALA2_CPY    4
#define NAS0        5
#define FUN0        6
#define NAS1        7
#define FUN1        8
#define NAS2        9
#define FUN2       10
#define ALA3       11
#define ALA4       12
#define MODL       13
#define MODR       14

#define ALA2_KEY    14
#define NAS0_KEY    15
#define FUN0_KEY    16
#define ALA1_KEY    17

#define IS_LHS_KEY(KeyPos) ((KeyPos >= 0 && KeyPos <= 2) || (KeyPos >= 7  && KeyPos <=  9))
#define IS_RHS_KEY(KeyPos) ((KeyPos >= 3 && KeyPos <= 5) || (KeyPos >= 10 && KeyPos <= 12))
#define IS_LAYER_KEY(KeyPos) (KeyPos >= ALA2_KEY && KeyPos <= ALA1_KEY)

int behavior_key_release_listener(const zmk_event_t *ev) {
  struct zmk_position_state_changed *ep = as_zmk_position_state_changed(ev);
  if (ep == NULL) {
    return ZMK_EV_EVENT_BUBBLE;
  }

  const bool KeyPress = true;
  const bool KeyRelease = false;
  const bool KeyAction = ep->state;

  const uint32_t KeyPos = ep->position;
  const uint8_t FocusLayer = zmk_keymap_highest_layer_active();
  LOG_DBG("***** %d position on layer %d changed to %s", KeyPos, FocusLayer, KeyAction ? "pressed" : "released");

  static uint8_t state1 = 1; // for ALA1 handling
  static uint8_t state2 = 1; // for ALA2 handling

  // capture memory
  static const zmk_event_t *pCapturedEvent;
  
  /*
    the key positions are defined as follows

              0       1       2                   3       4       5
    (6)       7       8       9                   10      11      12       (13)
                      14      15                  16      17

   */

  // ------------------
  // state machine ALA1
  // ------------------

  switch (state1) {
  case 1:
    LOG_DBG("===== Current state: ALA1.1 =====");
    if ((KeyAction == KeyPress && KeyPos == ALA1_KEY &&
         (FocusLayer == ALA0 || FocusLayer == ALA2_CPY)) ||
        (KeyAction == KeyRelease && KeyPos == ALA2_KEY && FocusLayer == ALA4) ||
        (KeyAction == KeyRelease && KeyPos == NAS0_KEY && FocusLayer == NAS1)) {
      state1 = 2;
      LOG_DBG("      (ALA1 activated)");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
    }
    break;

  case 2:
    LOG_DBG("===== Current state: ALA1.2 =====");
    if IS_LAYER_KEY(KeyPos) {
      state1 = 1;
      LOG_DBG("      (Layer key changed)");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
    }
    if (KeyAction == KeyPress && IS_RHS_KEY(KeyPos)) {
      state1 = 3;
      LOG_DBG("      (RHS key pressed)");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
    }
    break;

  case 3:
    LOG_DBG("===== Current state: ALA1.3 =====");
    if IS_LAYER_KEY(KeyPos) {
      state1 = 1;
      LOG_DBG("      (Layer key changed)");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
    }
    if (KeyAction == KeyPress && IS_LHS_KEY(KeyPos)) {
      state1 = 4;
      LOG_DBG("      (LHS key pressed)");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
      pCapturedEvent = ev;
      return ZMK_EV_EVENT_CAPTURED;
    }
    break;

  case 4:
    LOG_DBG("===== Current state: ALA1.4 =====");
    if (pCapturedEvent == NULL) {
      state1 = 1;
      LOG_ERR("!!!!! Error: On state 4, missing captured event");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
      break;
    }

    if ((KeyAction == KeyPress && (IS_LHS_KEY(KeyPos) || IS_RHS_KEY(KeyPos) || IS_LAYER_KEY(KeyPos))) ||
        (KeyAction == KeyRelease && KeyPos == as_zmk_position_state_changed(pCapturedEvent)->position)) {
      state1 = 1;
      // raise captured event first, then do the  rest
      LOG_DBG("      (Any key pressed or Captured key released: Raise Captured Event, then do the rest");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
      ZMK_EVENT_RAISE_AFTER(pCapturedEvent, behavior_key_release);
      pCapturedEvent = NULL;

    } else if (KeyAction == KeyRelease && KeyPos == ALA1_KEY) {
      state1 = 1;
      // leave the layer (and the state), then - later - wait for captured key release: then raise it
      LOG_DBG("***** (ALA1 layer key released: Exit layer; then - later - wait for captured key release: then raise it");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
    }
    break;

  default:
    LOG_ERR("invalid state '%d' of key release behavior", state1);
    break;
  }

  // ------------------
  // state machine ALA2
  // ------------------

  switch (state2) {
  case 1:
    LOG_DBG("===== Current state: ALA2.1 =====");
    if ((KeyAction == KeyPress && KeyPos == ALA2_KEY &&
         (FocusLayer == ALA0 || FocusLayer == ALA1_CPY)) ||
        (KeyAction == KeyRelease && KeyPos == ALA1_KEY && FocusLayer == ALA4) ||
        (KeyAction == KeyRelease && KeyPos == FUN0_KEY && FocusLayer == FUN1)) {
      state2 = 2;
      LOG_DBG("      (ALA2 activated)");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
    }
    break;

  case 2:
    LOG_DBG("===== Current state: ALA2.2 =====");
    if IS_LAYER_KEY(KeyPos) {
      state2 = 1;
      LOG_DBG("      (Layer key changed)");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
    }
    if (KeyAction == KeyPress && IS_LHS_KEY(KeyPos)) {
      state2 = 3;
      LOG_DBG("      (LHS key pressed)");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
    }
    break;

  case 3:
    LOG_DBG("===== Current state: ALA2.3 =====");
    if IS_LAYER_KEY(KeyPos) {
      state2 = 1;
      LOG_DBG("      (Layer key changed)");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
    }
    if (KeyAction == KeyPress && IS_RHS_KEY(KeyPos)) {
      state2 = 4;
      LOG_DBG("      (RHS key pressed)");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
      pCapturedEvent = ev;
      return ZMK_EV_EVENT_CAPTURED;
    }
    break;

  case 4:
    LOG_DBG("===== Current state: ALA2.4 =====");
    if (pCapturedEvent == NULL) {
      state2 = 1;
      LOG_ERR("!!!!! Error: On state 4, missing captured event");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
      break;
    }

    if ((KeyAction == KeyPress && (IS_LHS_KEY(KeyPos) || IS_RHS_KEY(KeyPos) || IS_LAYER_KEY(KeyPos))) ||
        (KeyAction == KeyRelease && KeyPos == as_zmk_position_state_changed(pCapturedEvent)->position)) {
      state2 = 1;
      // raise captured event first, then do the  rest
      LOG_DBG("      (Any key pressed or Captured key released: Raise Captured Event, then do the rest");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
      ZMK_EVENT_RAISE_AFTER(pCapturedEvent, behavior_key_release);
      pCapturedEvent = NULL;

    } else if (KeyAction == KeyRelease && KeyPos == ALA2_KEY) {
      state2 = 1;
      // leave the layer (and the state), then - later - wait for captured key release: then raise it
      LOG_DBG("***** (ALA2 layer key released: Exit layer; then - later - wait for captured key release: then raise it");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
    }
    break;

  default:
    LOG_ERR("invalid state '%d' of key release behavior", state2);
    break;
  }

  // Outside of state machine (Stateless) key release check:
  // is our captured key released? if so: raise key press before release
  if (pCapturedEvent != NULL) {
    struct zmk_position_state_changed *pCapturedPos = as_zmk_position_state_changed(pCapturedEvent);
    if ( pCapturedPos != NULL && pCapturedPos->position == KeyPos && KeyAction == KeyRelease) {
      LOG_DBG("      (Captured key released: Raise Captured Event now");
      ZMK_EVENT_RAISE_AFTER(pCapturedEvent, behavior_key_release);
      pCapturedEvent = NULL;
    }
  }    
  
  return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_key_release, behavior_key_release_listener);
ZMK_SUBSCRIPTION(behavior_key_release, zmk_position_state_changed);
