#pragma once
#include <stdbool.h>

/* Battery level from the AXP2101 power chip. Returns false if the chip does not answer.
 * *pct: 0-100, or -1 when no battery is connected. *charging: battery is being charged. */
bool battery_read(int *pct, bool *charging);
/* Battery voltage in mV (0 if unknown). */
int battery_mv(void);
/* USB (VBUS) power present. */
bool battery_usb_power(void);
/* Switch the board off (PWR key switches it on again). */
void battery_power_off(void);
