/* Battery level: X-Powers AXP2101 power chip on the shared I2C bus (address 0x34). Its built-in fuel
 * gauge gives the charge in percent (register 0xA4). Register numbers from XPowersLib (Waveshare example). */
#include "battery_axp2101.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"

#define AXP_ADDR        0x34
#define REG_STATUS1     0x00   /* bit 3: battery present */
#define REG_STATUS2     0x01   /* bits 7..5: 1 = charging, 2 = discharging, 0 = standby */
#define REG_GAUGE_CTRL  0x18   /* bit 3: fuel gauge on */
#define REG_ADC_CTRL    0x30   /* bit 0: battery voltage measurement on */
#define REG_BAT_DET     0x68   /* bit 0: battery detection on */
#define REG_BAT_PERCENT 0xA4

static i2c_master_dev_handle_t s_dev;
static bool s_setup;

static bool rd(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, v, 1, 100) == ESP_OK;
}

static void set_bit(uint8_t reg, uint8_t bit)
{
    uint8_t v;
    if (!rd(reg, &v) || (v & (1u << bit))) return;
    uint8_t b[2] = { reg, (uint8_t)(v | (1u << bit)) };
    i2c_master_transmit(s_dev, b, 2, 100);
}

bool battery_read(int *pct, bool *charging)
{
    if (!s_dev) {
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        if (!bus) return false;
        i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = AXP_ADDR, .scl_speed_hz = 100000 };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) { s_dev = NULL; return false; }
    }
    if (!s_setup) {                      /* make sure detection and the gauge are running (harmless if already on) */
        set_bit(REG_BAT_DET, 0);
        set_bit(REG_ADC_CTRL, 0);
        set_bit(REG_GAUGE_CTRL, 3);
        s_setup = true;
    }
    uint8_t s1, s2, p;
    if (!rd(REG_STATUS1, &s1) || !rd(REG_STATUS2, &s2) || !rd(REG_BAT_PERCENT, &p)) return false;
    *charging = (s2 >> 5) == 1;
    *pct = (s1 & 0x08) ? (p > 100 ? 100 : p) : -1;
    return true;
}

int battery_mv(void)
{
    uint8_t h, l, s1;
    if (!s_dev || !rd(REG_STATUS1, &s1) || !(s1 & 0x08)) return 0;
    if (!rd(0x34, &h) || !rd(0x35, &l)) return 0;   /* 14-bit battery voltage, 1 mV per step */
    return ((h & 0x3F) << 8) | l;
}

/* USB power present (AXP2101 status 1, bit 5 "VBUS good"), 2026-11-22: while plugged in the board must not
 * light-sleep, or its USB connection to the computer drops (and with it the log). */
bool battery_usb_power(void)
{
    if (!s_dev) { int p; bool c; battery_read(&p, &c); }
    uint8_t v;
    return s_dev && rd(REG_STATUS1, &v) && (v & 0x20);
}

/* Switch the whole board off, like holding the PWR key (AXP2101 register 0x10 "common configuration", bit 0:
 * software power off). The PWR key switches it on again. 2026-11-22. */
void battery_power_off(void)
{
    if (!s_dev) { int p; bool c; battery_read(&p, &c); }
    if (s_dev) set_bit(0x10, 0);
}
