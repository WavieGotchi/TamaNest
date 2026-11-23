#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { int year, month, day, hour, minute, second; } rtc_time_t;

/* Read the board clock. *lost is set when the chip says its time is not reliable (power was lost). */
bool rtc_read(rtc_time_t *t, bool *lost);
/* Set the board clock (also clears the "lost" flag). */
bool rtc_write(const rtc_time_t *t);
/* Local time <-> seconds since 1970 (no time zone). */
uint32_t rtc_to_secs(const rtc_time_t *t);
void rtc_from_secs(uint32_t s, rtc_time_t *t);
int rtc_days_in_month(int year, int month);
