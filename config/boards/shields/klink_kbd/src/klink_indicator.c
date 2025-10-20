#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>

#include <math.h>

#define NUMLOCK_BIT BIT(0)
#define CAPSLOCK_BIT BIT(1)
#define SCROLLLOCK_BIT BIT(2)

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_r)),
             "An alias for a red LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_g)),
             "An alias for a green LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_b)),
             "An alias for a blue LED is not found for RGBLED_WIDGET");

// GPIO-based LED device and indices of red/green/blue LEDs inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t led_idx[] = {DT_NODE_CHILD_IDX(DT_ALIAS(indicator_r)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(indicator_g)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(indicator_b))};

struct indicator_state_t {
    uint8_t keylock;
    uint8_t connection;
    uint8_t active_device;
    uint8_t battery;
    uint8_t flash_times;
} indicator_state;

static void set_indicator_color(uint8_t bits) {
    static uint8_t last_bits = 0;
    if (bits != last_bits) {
        for (uint8_t pos = 0; pos < 3; pos++) {
            if (bits & (1<<pos)) {
                led_on(led_dev, led_idx[pos]);
            } else {
                led_off(led_dev, led_idx[pos]);
            }
        }
        last_bits = bits;
    }
}

static void get_lock_indicators(void) {
    uint8_t state = zmk_hid_indicators_get_current_profile();
    LOG_DBG("LOCK LEDS: %d", state);
    indicator_state.keylock = state;
}

static void hid_indicators_status_update_cb(const zmk_event_t *eh) {
    get_lock_indicators();
}

ZMK_LISTENER(widget_hid_indicators_status, hid_indicators_status_update_cb);
ZMK_SUBSCRIPTION(widget_hid_indicators_status, zmk_hid_indicators_changed);


struct blink_item {
    uint16_t duration_ms;
    uint16_t sleep_ms;
    uint8_t count;
};

K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);


static void ble_active_profile_update(void) {
    uint8_t profile_index = zmk_ble_active_profile_index();
    if (profile_index > 3) return;
    indicator_state.active_device = profile_index;
    if (zmk_ble_active_profile_is_connected()) {
        indicator_state.connection = 2;
        indicator_state.flash_times = 3*4;
    //} else if (zmk_ble_active_profile_is_open()) {
    } else {
        indicator_state.connection = 1;
        indicator_state.flash_times = 15*4;
    }
    LOG_DBG("Device_BT%d, Connection State: %d", indicator_state.active_device+1, indicator_state.connection);
    return;
}

static void ble_active_profile_update_cb(const zmk_event_t *eh) {
    ble_active_profile_update();
}

ZMK_LISTENER(ble_active_profile_listener, ble_active_profile_update_cb);
ZMK_SUBSCRIPTION(ble_active_profile_listener, zmk_ble_active_profile_changed);

#include <zmk/events/keycode_state_changed.h>
static int zmk_handle_keycode_user(struct zmk_keycode_state_changed *event) {
    zmk_key_t key = event->keycode;
    LOG_DBG("key 0x%X", key);
    if (key == 0xAB) {
        ble_active_profile_update();
    }
    return ZMK_EV_EVENT_HANDLED;
}

static int keycode_user_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *kc_state;

    kc_state = as_zmk_keycode_state_changed(eh);

    if (kc_state != NULL) {
        return zmk_handle_keycode_user(kc_state);
    }

    return 0;
}

ZMK_LISTENER(keycode_user, keycode_user_listener);
ZMK_SUBSCRIPTION(keycode_user, zmk_keycode_state_changed);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int led_battery_listener_cb(const zmk_event_t *eh) {
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;
    indicator_state.battery = battery_level;
    return 0;
}

ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

void led_process_thread(void) {
    while (true) {
        k_sleep(K_MSEC(20));
        static uint16_t led_timer_steps = 0;
        led_timer_steps++;

        if (indicator_state.connection > 0) {
                static uint8_t profile_color_bits[3]= {0b011, 0b110, 0b101};
            if (indicator_state.active_device >= 3) {
                return;
            }

            if ((led_timer_steps & 0xf) == 0xf) {
                indicator_state.flash_times--;
                uint8_t color_bits = profile_color_bits[indicator_state.active_device];
                switch ((led_timer_steps >> 4) & 0x3) {
                    case 0:
                        set_indicator_color(0);
                        break;
                    case 1:
                        set_indicator_color(color_bits);
                        break;
                    case 2:
                        if (indicator_state.connection != 2) set_indicator_color(0);
                        break;
                    case 3:
                        if (indicator_state.connection != 2) {
                            bt_addr_le_t *addr = zmk_ble_active_profile_addr();
                            if ( bt_addr_le_eq(addr, BT_ADDR_LE_ANY) ) set_indicator_color(0b001); //red color
                            else set_indicator_color(0b100); //blue color
                        }
                        break;
                }
                if (indicator_state.flash_times == 0) indicator_state.connection = 0;
            }
        } else if (indicator_state.battery < 10) {
            if ((led_timer_steps & 0x1f) == 0xf) set_indicator_color(0b001);
            else if ((led_timer_steps & 0x1f) == 0x1f) set_indicator_color(0);
        } else {
            if (indicator_state.keylock & CAPSLOCK_BIT) {
                set_indicator_color(0b101);
            } else {
                set_indicator_color(0);
            }
        }
    }
}

// define led_process_thread with stack size 1024, start running it 100 ms after boot
K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 100);


void klink_indicator_init_thread(void) {
    indicator_state.connection = 1;
    // zmk_ble_set_device_name("Tofu60 v3.0z BLE");
    indicator_state.battery = 111;
}

K_THREAD_DEFINE(klink_indicator_init_tid, 1024, klink_indicator_init_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 200);