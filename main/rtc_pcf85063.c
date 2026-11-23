/* Board real-time clock: NXP PCF85063 on the shared I2C bus (address 0x51). It runs from the AXP2101's RTC
 * supply, so it keeps time while the board is switched off as long as a battery is connected.
 * Time is kept as local time (no time zones): seconds since 1970 as if the local time were UTC. */
#include "rtc_pcf85063.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#define RTC_ADDR      0x51
#define REG_CONTROL1  0x00
#define REG_SECONDS   0x04   /* bit 7: OS, oscillator stopped (time not reliable) */

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;

static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

static bool rtc_dev(void)
{
    if (s_dev) return true;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return false;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = RTC_ADDR, .scl_speed_hz = 100000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) { s_dev = NULL; return false; }
    return true;
}

/* Days since 1970-01-01 for a civil date (proleptic Gregorian). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint32_t rtc_to_secs(const rtc_time_t *t)
{
    return (uint32_t)(days_from_civil(t->year, t->month, t->day) * 86400 + t->hour * 3600 + t->minute * 60 + t->second);
}

void rtc_from_secs(uint32_t s, rtc_time_t *t)
{
    int64_t z = s / 86400 + 719468;
    int64_t era = z / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    t->day = doy - (153 * mp + 2) / 5 + 1;
    t->month = mp < 10 ? mp + 3 : mp - 9;
    t->year = (int)(yoe + era * 400) + (t->month <= 2);
    uint32_t sod = s % 86400;
    t->hour = (int)(sod / 3600); t->minute = (int)(sod / 60 % 60); t->second = (int)(sod % 60);
}

int rtc_days_in_month(int year, int month)
{
    static const uint8_t d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return d[(month - 1) % 12];
}

bool rtc_read(rtc_time_t *t, bool *lost)
{
    if (!rtc_dev()) return false;
    uint8_t reg = REG_SECONDS, b[7];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof b, 100) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not answering");
        return false;
    }
    if (lost) *lost = (b[0] & 0x80) != 0;
    t->second = from_bcd(b[0] & 0x7F);
    t->minute = from_bcd(b[1] & 0x7F);
    t->hour   = from_bcd(b[2] & 0x3F);
    t->day    = from_bcd(b[3] & 0x3F);
    t->month  = from_bcd(b[5] & 0x1F);
    t->year   = 2000 + from_bcd(b[6]);
    if (t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 || t->hour > 23 || t->minute > 59 || t->second > 59) {
        if (lost) *lost = true;
    }
    return true;
}

bool rtc_write(const rtc_time_t *t)
{
    if (!rtc_dev()) return false;
    int64_t days = days_from_civil(t->year, t->month, t->day);
    uint8_t wd = (uint8_t)((days + 4) % 7);          /* 1970-01-01 was a Thursday; 0 = Sunday */
    uint8_t ctl[2] = { REG_CONTROL1, 0x00 };          /* 24 h mode, clock running */
    uint8_t b[8] = { REG_SECONDS, to_bcd(t->second) /* writing clears OS */, to_bcd(t->minute), to_bcd(t->hour),
                     to_bcd(t->day), wd, to_bcd(t->month), to_bcd(t->year % 100) };
    if (i2c_master_transmit(s_dev, ctl, sizeof ctl, 100) != ESP_OK) return false;
    return i2c_master_transmit(s_dev, b, sizeof b, 100) == ESP_OK;
}
