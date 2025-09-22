#define DT_DRV_COMPAT zmk_pair_inhibitor

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h> // For sending key events

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// DTS node definition for the main pair_inhibitors node.
#define ZMK_DT_PAIR_INHIBITORS_NODE DT_PATH(pair_inhibitors)

#if !DT_NODE_EXISTS(ZMK_DT_PAIR_INHIBITORS_NODE)
#error "No 'pair_inhibitors' node found in devicetree. Please define it in your .dts/.overlay file."
#endif

// Helper macro to count child nodes using DT_FOREACH_CHILD_STATUS_OK_VARGS
#define CHILD_COUNTER(node_id, i) 1 +

// Count the number of individual pair-inhibitor instances defined in DTS
// For ZMK v3.5, use DT_FOREACH_CHILD_STATUS_OK_VARGS with a counter macro.
#define PAIR_INHIBITOR_COUNT DT_FOREACH_CHILD_STATUS_OKAY_VARGS(ZMK_DT_PAIR_INHIBITORS_NODE, CHILD_COUNTER) 0

#if PAIR_INHIBITOR_COUNT == 0
#error "No child nodes found under 'pair_inhibitors'. Define at least one pair_inhibitor_L_H node."
#endif

// State for each pair inhibitor
enum pair_inhibitor_state {
    PAIR_INHIBITOR_STATE_IDLE,              // Neither L nor H is actively involved.
    PAIR_INHIBITOR_STATE_L_PENDING_TIMEOUT, // L-key pressed, timer started, waiting for H or timeout.
    PAIR_INHIBITOR_STATE_L_ACTIVE,          // L-key press bubbled, waiting for L release or H press.
    PAIR_INHIBITOR_STATE_L_INHIBITED,       // L-key press inhibited by H-key, physical L-release is consumed.
};

struct pair_inhibitor_instance {
    uint32_t l_pos;
    uint32_t h_pos;
    int32_t timeout_ms;
    struct k_work_delayable work_item;
    enum pair_inhibitor_state state;
    // Tracks if the physical L key is currently down, regardless of its logical (bubbled) state.
    // Used to consume the actual physical L-release event if its press was discarded or synthesized.
    bool l_is_physically_pressed; 
};

// Array of all pair inhibitor instances
static struct pair_inhibitor_instance pair_inhibitors[PAIR_INHIBITOR_COUNT];

const struct zmk_listener zmk_listener_pair_inhibitor;

// Forward declarations
static void l_key_timeout_handler(struct k_work *work);

static int pair_inhibitor_listener(const zmk_event_t *eh);

// ZMK Event Manager listener and subscription setup
ZMK_LISTENER(pair_inhibitor, pair_inhibitor_listener);
ZMK_SUBSCRIPTION(pair_inhibitor, zmk_position_state_changed);

// Helper to find the instance by L or H position
static struct pair_inhibitor_instance *get_instance_by_l_pos(uint32_t pos) {
    for (size_t i = 0; i < PAIR_INHIBITOR_COUNT; ++i) {
        if (pair_inhibitors[i].l_pos == pos) {
            return &pair_inhibitors[i];
        }
    }
    return NULL;
}

static struct pair_inhibitor_instance *get_instance_by_h_pos(uint32_t pos) {
    for (size_t i = 0; i < PAIR_INHIBITOR_COUNT; ++i) {
        if (pair_inhibitors[i].h_pos == pos) {
            return &pair_inhibitors[i];
        }
    }
    return NULL;
}

// Function to generate and bubble a position event
static int bubble_position_event(uint32_t position, bool pressed) {
    struct zmk_position_state_changed event = {
      .position = position,
      .state = pressed,
      .timestamp = k_uptime_get(),
    };

    struct zmk_position_state_changed_event dupe_ev =
      copy_raised_zmk_position_state_changed(&event);
  
    dupe_ev.header.event =  &zmk_event_zmk_position_state_changed;
    return ZMK_EVENT_RAISE_AFTER(dupe_ev, pair_inhibitor);
    // return ZMK_EVENT_RAISE(dupe_ev);
}

// Timeout handler for L-keys
static void l_key_timeout_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct pair_inhibitor_instance *instance = CONTAINER_OF(dwork, struct pair_inhibitor_instance, work_item);

    if (instance->state == PAIR_INHIBITOR_STATE_L_PENDING_TIMEOUT) {
        LOG_DBG("L-key %d timeout, activating press", instance->l_pos);
        instance->state = PAIR_INHIBITOR_STATE_L_ACTIVE;
        // Bubble the L-key press event
        bubble_position_event(instance->l_pos, true);
    } else {
        LOG_WRN("L-key %d timeout handler called in unexpected state %d", instance->l_pos, instance->state);
    }
}


// Event listener function for ZMK position_state_changed events
static int pair_inhibitor_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint32_t position = ev->position;
    bool is_pressed = ev->state;

    struct pair_inhibitor_instance *instance = NULL;
    bool is_l_key = false;
    bool is_h_key = false;

    // Check if the event is for an L-key
    instance = get_instance_by_l_pos(position);
    if (instance != NULL) {
        is_l_key = true;
    } else {
        // Check if the event is for an H-key
        instance = get_instance_by_h_pos(position);
        if (instance != NULL) {
            is_h_key = true;
        }
    }

    if (instance == NULL) {
        // Not a monitored key for any pair, bubble it up normally.
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    LOG_DBG("Pair Inhibitor: Pos %d %s (L_pos %d, H_pos %d), State %d, last L_phys_pressed state: %d", 
            position, is_pressed ? "pressed" : "released", instance->l_pos, instance->h_pos, 
            instance->state, instance->l_is_physically_pressed);

    if (is_l_key) {
        if (is_pressed) { // L-key pressed
            if (instance->state == PAIR_INHIBITOR_STATE_IDLE) {
                LOG_DBG("L-key %d pressed: Starting timeout", position);
                instance->state = PAIR_INHIBITOR_STATE_L_PENDING_TIMEOUT;
                instance->l_is_physically_pressed = true; // Mark physical L as pressed
                k_work_reschedule(&instance->work_item, K_MSEC(instance->timeout_ms));
                return ZMK_EV_EVENT_HANDLED; // Consume L-key press (don't bubble yet)
            } else { 
                // L is already in a state (PENDING_TIMEOUT, ACTIVE, or INHIBITED).
                // Any additional physical press should be consumed, but update physical state.
                instance->l_is_physically_pressed = true;
                LOG_DBG("L-key %d pressed: Already in state %d, consuming", position, instance->state);
                return ZMK_EV_EVENT_HANDLED;
            }
        } else { // L-key released
            instance->l_is_physically_pressed = false; // Physical release occurred
            
            if (instance->state == PAIR_INHIBITOR_STATE_L_PENDING_TIMEOUT) {
                // TODO, hgi: will probably not happen, because user is to slow for press/release <
                // timeout, but should probably create press event in-between here and then do ZMK_EV_EVENT_BUBBLE 
                LOG_DBG("L-key %d released: Cancelling timeout, activating L-press", position);
                k_work_cancel_delayable(&instance->work_item);
                instance->state = PAIR_INHIBITOR_STATE_IDLE;
                bubble_position_event(instance->l_pos, true); // hgi: Generate and bubble a synthetic L-press event
                return ZMK_EV_EVENT_BUBBLE; // hgi: Bubble L-key release normally
            } else if (instance->state == PAIR_INHIBITOR_STATE_L_ACTIVE) {
                LOG_DBG("L-key %d released: Bubbling L-release", position);
                instance->state = PAIR_INHIBITOR_STATE_IDLE;
                return ZMK_EV_EVENT_BUBBLE; // Bubble L-key release normally
            } else if (instance->state == PAIR_INHIBITOR_STATE_L_INHIBITED) {
                // hgi: this will never happen with my H/L switch arrangement
                LOG_DBG("L-key %d released: Inhibited, consuming L-release", position);
                // State remains L_INHIBITED until H is released. Physical L-release is consumed.
                return ZMK_EV_EVENT_HANDLED; 
            } else if (instance->state == PAIR_INHIBITOR_STATE_IDLE) {
                // This scenario means L became IDLE while still physically pressed (e.g., H released).
                // This physical L-release needs to be consumed.
                LOG_DBG("L-key %d released: Currently IDLE, consuming (physical L-key release)", position);
                return ZMK_EV_EVENT_HANDLED;
            }
        }
    } else if (is_h_key) {
        if (is_pressed) { // H-key pressed
            if (instance->state == PAIR_INHIBITOR_STATE_L_PENDING_TIMEOUT) {
                LOG_DBG("H-key %d pressed: Stopping L-key %d timeout, inhibiting L-press", position, instance->l_pos);
                k_work_cancel_delayable(&instance->work_item);
                instance->state = PAIR_INHIBITOR_STATE_L_INHIBITED;
            } else if (instance->state == PAIR_INHIBITOR_STATE_L_ACTIVE) {
                LOG_DBG("H-key %d pressed: L-key %d active, generating L-release", position, instance->l_pos);
                instance->state = PAIR_INHIBITOR_STATE_L_INHIBITED;
                bubble_position_event(instance->l_pos, false); // Generate and bubble a synthetic L-release event
            }
            // In other states (IDLE, L_INHIBITED), H-press is just bubbled normally.
            return ZMK_EV_EVENT_BUBBLE; // Always bubble H-key press
        } else { // H-key released
            if (instance->state == PAIR_INHIBITOR_STATE_L_INHIBITED) {
                LOG_DBG("H-key %d released: L-key %d was inhibited, now IDLE", position, instance->l_pos);
                instance->state = PAIR_INHIBITOR_STATE_IDLE;
            }
            // In other states (IDLE, L_PENDING_TIMEOUT, L_ACTIVE), H-release is just bubbled normally.
            return ZMK_EV_EVENT_BUBBLE; // Always bubble H-key release
        }
    }
    
    // Should not reach here if logic is exhaustive for relevant keys.
    return ZMK_EV_EVENT_BUBBLE;
}

// Macro to initialize individual pair inhibitor instance structs from DTS
#define PI_INIT_INSTANCE_STRUCT(node_id)                                                  \
    {                                                                                     \
        .l_pos = DT_PROP_BY_IDX(node_id, key_positions_pair, 0),                          \
        .h_pos = DT_PROP_BY_IDX(node_id, key_positions_pair, 1),                          \
        .timeout_ms = DT_PROP_OR(node_id, timeout_ms, 10),                                \
        .state = PAIR_INHIBITOR_STATE_IDLE,                                               \
        .l_is_physically_pressed = false,                                                 \
    },

// Initialize the static array of pair inhibitor instances using DTS data
static struct pair_inhibitor_instance pair_inhibitors[PAIR_INHIBITOR_COUNT] = {
    DT_FOREACH_CHILD(ZMK_DT_PAIR_INHIBITORS_NODE, PI_INIT_INSTANCE_STRUCT)
};

// Module initialization function
static int pair_inhibitor_module_init(void) {
    if (PAIR_INHIBITOR_COUNT == 0) {
        LOG_WRN("No pair inhibitor instances defined. Module is inactive.");
        return 0; // Return success but inactive
    }

    LOG_DBG("Initializing %d pair inhibitor instances", PAIR_INHIBITOR_COUNT);
    for (size_t i = 0; i < PAIR_INHIBITOR_COUNT; ++i) {
        k_work_init_delayable(&pair_inhibitors[i].work_item, l_key_timeout_handler);
        LOG_DBG("Instance %d: L_pos=%d, H_pos=%d, Timeout=%dms", i,
                pair_inhibitors[i].l_pos, pair_inhibitors[i].h_pos,
                pair_inhibitors[i].timeout_ms);
    }
    return 0;
}

// Register the module initialization function with Zephyr's system initialization
SYS_INIT(pair_inhibitor_module_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif
