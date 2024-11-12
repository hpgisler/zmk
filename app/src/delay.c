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

//#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE 12

struct delay_cfg {
  int32_t timeout_ms;
  int32_t layer_key_positions_len;
  int32_t layer_key_positions[];
};

struct delayed_position_presses {
    // CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE
    // theoretical limit, can be reached only
    // if typing is very fast or delay is very large 
    // array: sort order ascending according key_positions_pressed timestamp (occurrence)
    const zmk_event_t *key_positions_pressed[CONFIG_ZMK_DELAY_MAX_KEY_POSITIONS_DELAYABLE];
    zmk_event_t *oldest, *next;
};


struct k_work_delayable timeout_task;
int64_t timeout_task_timeout_at;

// Store the delay key pointer in the delays array, one pointer for each key position
// The delays are sorted shortest-first, then by virtual-key-position.
static int initialize_delay(struct delay_cfg *new_delay) {
    for (int i = 0; i < new_delay->key_position_len; i++) {
        int32_t position = new_delay->key_positions[i];
        if (position >= ZMK_KEYMAP_LEN) {
            LOG_ERR("Unable to initialize delay, key position %d does not exist", position);
            return -EINVAL;
        }

        struct delay_cfg *insert_delay = new_delay;
        bool set = false;
        for (int j = 0; j < CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY; j++) {
            struct delay_cfg *delay_at_j = delay_lookup[position][j];
            if (delay_at_j == NULL) {
                delay_lookup[position][j] = insert_delay;
                set = true;
                break;
            }
            if (delay_at_j->key_position_len < insert_delay->key_position_len ||
                (delay_at_j->key_position_len == insert_delay->key_position_len &&
                 delay_at_j->virtual_key_position < insert_delay->virtual_key_position)) {
                continue;
            }
            // put insert_delay in this spot, move all other delays up.
            delay_lookup[position][j] = insert_delay;
            insert_delay = delay_at_j;
        }
        if (!set) {
            LOG_ERR("Too many delays for key position %d, CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY %d.",
                    position, CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY);
            return -ENOMEM;
        }
    }
    return 0;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int number_of_delay_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();
    for (int i = 0; i < CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY; i++) {
        struct delay_cfg *delay = delay_lookup[position][i];
        if (delay == NULL) {
            return number_of_delay_candidates;
        }
        if (delay_active_on_layer(delay, highest_active_layer)) {
            candidates[number_of_delay_candidates].delay = delay;
            candidates[number_of_delay_candidates].timeout_at = timestamp + delay->timeout_ms;
            number_of_delay_candidates++;
        }
        // LOG_DBG("delay timeout %d %d %d", position, i, candidates[i].timeout_at);
    }
    return number_of_delay_candidates;
}

static int filter_candidates(int32_t position) {
    // this code iterates over candidates and the lookup together to filter in O(n)
    // assuming they are both sorted on key_position_len, virtal_key_position
    int matches = 0, lookup_idx = 0, candidate_idx = 0;
    while (lookup_idx < CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY &&
           candidate_idx < CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY) {
        struct delay_cfg *candidate = candidates[candidate_idx].delay;
        struct delay_cfg *lookup = delay_lookup[position][lookup_idx];
        if (candidate == NULL || lookup == NULL) {
            break;
        }
        if (candidate->virtual_key_position == lookup->virtual_key_position) {
            candidates[matches] = candidates[candidate_idx];
            matches++;
            candidate_idx++;
            lookup_idx++;
        } else if (candidate->key_position_len > lookup->key_position_len) {
            lookup_idx++;
        } else if (candidate->key_position_len < lookup->key_position_len) {
            candidate_idx++;
        } else if (candidate->virtual_key_position > lookup->virtual_key_position) {
            lookup_idx++;
        } else if (candidate->virtual_key_position < lookup->virtual_key_position) {
            candidate_idx++;
        }
    }
    // clear unmatched candidates
    for (int i = matches; i < CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY; i++) {
        candidates[i].delay = NULL;
    }
    // LOG_DBG("delay matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout() {
    int64_t first_timeout = LONG_MAX;
    for (int i = 0; i < CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY; i++) {
        if (candidates[i].delay == NULL) {
            break;
        }
        if (candidates[i].timeout_at < first_timeout) {
            first_timeout = candidates[i].timeout_at;
        }
    }
    return first_timeout;
}

static inline bool candidate_is_completely_pressed(struct delay_cfg *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->key_positions)
    // this invariant is enforced by filter_candidates
    // since events may have been reraised after clearing one or more slots at
    // the start of pressed_keys (see: release_pressed_keys), we have to check
    // that each key needed to trigger the delay was pressed, not just the last.
    for (int i = 0; i < candidate->key_position_len; i++) {
        if (pressed_keys[i] == NULL) {
            return false;
        }
    }
    return true;
}

static int cleanup();

static int filter_timed_out_candidates(int64_t timestamp) {
    int num_candidates = 0;
    for (int i = 0; i < CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY; i++) {
        struct delay_candidate *candidate = &candidates[i];
        if (candidate->delay == NULL) {
            break;
        }
        if (candidate->timeout_at > timestamp) {
            // reorder candidates so they're contiguous
            candidates[num_candidates].delay = candidate->delay;
            candidates[num_candidates].timeout_at = candidate->timeout_at;
            num_candidates++;
        } else {
            candidate->delay = NULL;
        }
    }
    return num_candidates;
}

static int clear_candidates() {
    for (int i = 0; i < CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY; i++) {
        if (candidates[i].delay == NULL) {
            return i;
        }
        candidates[i].delay = NULL;
    }
    return CONFIG_ZMK_DELAY_MAX_DELAYS_PER_KEY;
}

static int capture_pressed_key(const zmk_event_t *ev) {
    for (int i = 0; i < CONFIG_ZMK_DELAY_MAX_KEYS_PER_DELAY; i++) {
        if (pressed_keys[i] != NULL) {
            continue;
        }
        pressed_keys[i] = ev;
        return ZMK_EV_EVENT_CAPTURED;
    }
    return 0;
}

const struct zmk_listener zmk_listener_delay;

static int release_pressed_keys() {
    for (int i = 0; i < CONFIG_ZMK_DELAY_MAX_KEYS_PER_DELAY; i++) {
        const zmk_event_t *captured_event = pressed_keys[i];
        if (pressed_keys[i] == NULL) {
            return i;
        }
        pressed_keys[i] = NULL;
        if (i == 0) {
            LOG_DBG("delay: releasing position event %d",
                    as_zmk_position_state_changed(captured_event)->position);
            ZMK_EVENT_RELEASE(captured_event)
        } else {
            // reprocess events (see tests/delay/fully-overlapping-delays-3 for why this is needed)
            LOG_DBG("delay: reraising position event %d",
                    as_zmk_position_state_changed(captured_event)->position);
            ZMK_EVENT_RAISE(captured_event);
        }
    }
    return CONFIG_ZMK_DELAY_MAX_KEYS_PER_DELAY;
}

static inline int press_delay_behavior(struct delay_cfg *delay, int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = delay->virtual_key_position,
        .timestamp = timestamp,
    };

    return behavior_keymap_binding_pressed(&delay->behavior, event);
}

static inline int release_delay_behavior(struct delay_cfg *delay, int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = delay->virtual_key_position,
        .timestamp = timestamp,
    };

    return behavior_keymap_binding_released(&delay->behavior, event);
}

static void move_pressed_keys_to_delayed(struct delayed *delayed) {
    int delay_length = delayed->delay->key_position_len;
    for (int i = 0; i < delay_length; i++) {
        delayed->key_positions_pressed[i] = pressed_keys[i];
        pressed_keys[i] = NULL;
    }
    // move any other pressed keys up
    for (int i = 0; i + delay_length < CONFIG_ZMK_DELAY_MAX_KEYS_PER_DELAY; i++) {
        if (pressed_keys[i + delay_length] == NULL) {
            return;
        }
        pressed_keys[i] = pressed_keys[i + delay_length];
        pressed_keys[i + delay_length] = NULL;
    }
}

static struct delayed *store_delayed(struct delay_cfg *delay) {
    for (int i = 0; i < CONFIG_ZMK_DELAY_MAX_PRESSED_DELAYS; i++) {
        if (delayeds[i].delay == NULL) {
            delayeds[i].delay = delay;
            delayed_count++;
            return &delayeds[i];
        }
    }
    LOG_ERR("Unable to store delay; already %d active. Increase "
            "CONFIG_ZMK_DELAY_MAX_PRESSED_DELAYS",
            CONFIG_ZMK_DELAY_MAX_PRESSED_DELAYS);
    return NULL;
}

static void activate_delay(struct delay_cfg *delay) {
    struct delayed *delayed = store_delayed(delay);
    if (delayed == NULL) {
        // unable to store delay
        release_pressed_keys();
        return;
    }
    move_pressed_keys_to_delayed(delayed);
    press_delay_behavior(
        delay, as_zmk_position_state_changed(delayed->key_positions_pressed[0])->timestamp);
}

static void deactivate_delay(int delayed_index) {
    delayed_count--;
    if (delayed_index != delayed_count) {
        memcpy(&delayeds[delayed_index], &delayeds[delayed_count],
               sizeof(struct delayed));
    }
    delayeds[delayed_count].delay = NULL;
    delayeds[delayed_count] = (struct delayed){0};
}

/* returns true if a key was released. */
static bool release_delay_key(int32_t position, int64_t timestamp) {
    for (int delay_idx = 0; delay_idx < delayed_count; delay_idx++) {
        struct delayed *delayed = &delayeds[delay_idx];

        bool key_released = false;
        bool all_keys_pressed = true;
        bool all_keys_released = true;
        for (int i = 0; i < delayed->delay->key_position_len; i++) {
            if (delayed->key_positions_pressed[i] == NULL) {
                all_keys_pressed = false;
            } else if (as_zmk_position_state_changed(delayed->key_positions_pressed[i])
                           ->position != position) {
                all_keys_released = false;
            } else { // not null and position matches
                ZMK_EVENT_FREE(delayed->key_positions_pressed[i]);
                delayed->key_positions_pressed[i] = NULL;
                key_released = true;
            }
        }

        if (key_released) {
            if ((delayed->delay->slow_release && all_keys_released) ||
                (!delayed->delay->slow_release && all_keys_pressed)) {
                release_delay_behavior(delayed->delay, timestamp);
            }
            if (all_keys_released) {
                deactivate_delay(delay_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup() {
    k_work_cancel_delayable(&timeout_task);
    clear_candidates();
    if (fully_pressed_delay != NULL) {
        activate_delay(fully_pressed_delay);
        fully_pressed_delay = NULL;
    }
    return release_pressed_keys();
}

static void update_timeout_task() {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(first_timeout - k_uptime_get())) >= 0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (candidates[0].delay == NULL) {
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return 0;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }
    update_timeout_task();

    struct delay_cfg *candidate_delay = candidates[0].delay;
    LOG_DBG("delay: capturing position event %d", data->position);
    int ret = capture_pressed_key(ev);
    switch (num_candidates) {
    case 0:
        cleanup();
        return ret;
    case 1:
        if (candidate_is_completely_pressed(candidate_delay)) {
            fully_pressed_delay = candidate_delay;
            cleanup();
        }
        return ret;
    default:
        if (candidate_is_completely_pressed(candidate_delay)) {
            fully_pressed_delay = candidate_delay;
        }
        return ret;
    }
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup();
    if (release_delay_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        // The second and further key down events are re-raised. To preserve
        // correct order for e.g. hold-taps, reraise the key up event too.
        ZMK_EVENT_RAISE(ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return 0;
}

static void delay_timeout_handler(struct k_work *item) {
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        // timer was cancelled or rescheduled.
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) < 2) {
        cleanup();
    }
    update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return 0;
    }

    if (data->state) { // keydown
        return position_state_down(ev, data);
    } else { // keyup
        return position_state_up(ev, data);
    }
}

ZMK_LISTENER(delay, position_state_changed_listener);
ZMK_SUBSCRIPTION(delay, zmk_position_state_changed);

#define DELAY_INST(n)                                                                              \
    static struct delay_cfg delay_config_##n = {                                                   \
        .timeout_ms = DT_PROP(n, timeout_ms),                                                      \
        .key_positions = DT_PROP(n, key_positions),                                                \
        .key_position_len = DT_PROP_LEN(n, key_positions),                                         \
        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                                              \
        .virtual_key_position = ZMK_VIRTUAL_KEY_POSITION_DELAY(__COUNTER__),                       \
        .slow_release = DT_PROP(n, slow_release),                                                  \
        .layers = DT_PROP(n, layers),                                                              \
        .layers_len = DT_PROP_LEN(n, layers),                                                      \
    };

#define INITIALIZE_DELAY(n) initialize_delay(&delay_config_##n);

DT_INST_FOREACH_CHILD(0, DELAY_INST)

static int delay_init() {
    k_work_init_delayable(&timeout_task, delay_timeout_handler);
    DT_INST_FOREACH_CHILD(0, INITIALIZE_DELAY);
    return 0;
}

SYS_INIT(delay_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

//#endif
