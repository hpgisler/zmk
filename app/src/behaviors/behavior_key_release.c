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
  static uint32_t last_key_pos = 0;

  // capture memory
  static struct zmk_position_state_changed_event captured_event_;

  // current memory
  struct zmk_position_state_changed_event current_event = copy_raised_zmk_position_state_changed(ep);

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
    if (KeyAction == KeyPress && (IS_LHS_KEY(KeyPos)
                                  || (last_key_pos == 12 /*y*/ && KeyPos == 11)
                                  || (last_key_pos == 11 /*,*/ && KeyPos == 10))) {
      state1 = 4;
      LOG_DBG("      (LHS key pressed)");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
      captured_event_ = copy_raised_zmk_position_state_changed(ep);
      last_key_pos = KeyPos;
      return ZMK_EV_EVENT_CAPTURED;
    }
    break;

  case 4:
    LOG_DBG("===== Current state: ALA1.4 =====");
    if ((KeyAction == KeyPress && (IS_LHS_KEY(KeyPos) || IS_RHS_KEY(KeyPos) || IS_LAYER_KEY(KeyPos))) ||
        (KeyAction == KeyRelease && KeyPos == captured_event_.data.position)) {
      state1 = 1;
      // raise captured event first, then do the  rest
      LOG_DBG("      (Any key pressed or Captured key released: Raise Captured Event, then do the rest");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
      ZMK_EVENT_RAISE_AFTER(captured_event_, behavior_key_release);
      // note: the actual key release will be bubbled (i.e. do not return here)

    } else if (KeyAction == KeyRelease && KeyPos == ALA1_KEY) {
      state1 = 1;
      // leave the layer (and the state), then - later - wait for captured key release: then raise it
      LOG_DBG("***** (ALA1 layer key released: Exit layer, then raise captured key press");
      LOG_DBG("----- Next state: ALA1.%d -----", state1);
      ZMK_EVENT_RAISE_AFTER((current_event), behavior_key_release);
      ZMK_EVENT_RAISE_AFTER(captured_event_, behavior_key_release);
      LOG_DBG("      (Released captured key");
      last_key_pos = KeyPos;
      return ZMK_EV_EVENT_CAPTURED;
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
    if (KeyAction == KeyPress && (IS_RHS_KEY(KeyPos)
                                  || (last_key_pos == 7 /*l*/ && KeyPos == 8)
                                  || (last_key_pos == 8 /*f*/ && KeyPos == 9))) {
      state2 = 4;
      LOG_DBG("      (RHS key pressed)");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
      captured_event_ = copy_raised_zmk_position_state_changed(ep);
      last_key_pos = KeyPos;
      return ZMK_EV_EVENT_CAPTURED;
    }
    break;

  case 4:
    LOG_DBG("===== Current state: ALA2.4 =====");
    if ((KeyAction == KeyPress && (IS_LHS_KEY(KeyPos) || IS_RHS_KEY(KeyPos) || IS_LAYER_KEY(KeyPos))) ||
        (KeyAction == KeyRelease && KeyPos == captured_event_.data.position)) {
      state2 = 1;
      // raise captured event first, then do the  rest
      LOG_DBG("      (Any key pressed or Captured key released: Raise Captured Event, then do the rest");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
      ZMK_EVENT_RAISE_AFTER(captured_event_, behavior_key_release);
      // note: the actual key release will be bubbled (i.e. do not return here)

    } else if (KeyAction == KeyRelease && KeyPos == ALA2_KEY) {
      state2 = 1;
      // leave the layer (and the state), then - later - wait for captured key release: then raise it
      LOG_DBG("***** (ALA2 layer key released: Exit layer, then raise captured key press");
      LOG_DBG("----- Next state: ALA2.%d -----", state2);
      ZMK_EVENT_RAISE_AFTER(current_event, behavior_key_release);
      ZMK_EVENT_RAISE_AFTER(captured_event_, behavior_key_release);
      LOG_DBG("      (Released captured key");
      last_key_pos = KeyPos;
      return ZMK_EV_EVENT_CAPTURED;
    }
    break;

  default:
    LOG_ERR("invalid state '%d' of key release behavior", state2);
    break;
  }

  last_key_pos = KeyPos;
  return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_key_release, behavior_key_release_listener);
ZMK_SUBSCRIPTION(behavior_key_release, zmk_position_state_changed);
