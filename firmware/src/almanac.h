/*
 * NocSif sun/moon almanac — powers the watch-peek countdown line (PLAN §4.14).
 *
 * Given a latitude/longitude and a local calendar day, computes the sun's rise/set time, the moon's
 * rise/set time (both as minutes after local midnight), and how far the moon is through its cycle.
 * The peek watchface uses this to render lines like "sets in 2h05".
 *
 * Accuracy:
 *   - Sun: NOAA's sunrise equation (equation of time + solar declination, corrected for atmospheric
 *     refraction and the disc's size). Accurate to about a minute almost everywhere; polar day/night
 *     is reported as "no event" rather than a bogus time.
 *   - Moon: a low-precision Meeus series for ecliptic position, converted to altitude against local
 *     sidereal time and scanned across the day in 10-minute steps with linear interpolation at each
 *     rise/set crossing. Good to a few minutes at mid-latitudes, worse near the poles; also reports
 *     "no event" on the roughly one day per month the moon doesn't rise or set.
 *   - Not cheap (a few hundred trig calls) — call once per day/location change, not every tick. Uses
 *     no static state, so it's safe to call from more than one context.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOCSIF_ALM_NONE (-1)   /* sentinel: this event does not occur on this local day */

typedef struct {
    bool  sun_ok;         /* false only under polar day/night, where the sun never rises or sets */
    bool  sun_up_all_day; /* when !sun_ok: true = polar day (always up), false = polar night      */
    int   sunrise_min;    /* minutes after local midnight, or NOCSIF_ALM_NONE                     */
    int   sunset_min;
    int   moonrise_min;   /* minutes after local midnight, or NOCSIF_ALM_NONE                     */
    int   moonset_min;
    float moon_age;       /* fraction through the synodic month: 0 new, 0.25 first quarter, 0.5 full */
} nocsif_almanac_day_t;

/* Fill *out with the almanac for local date (y, m, d) at (lat_deg, lon_deg; north/east positive),
 * where the local clock is utc_off_min minutes ahead of UTC (negative for west of UTC). */
void nocsif_almanac_day(double lat_deg, double lon_deg, int y, int m, int d, int utc_off_min,
                        nocsif_almanac_day_t *out);

#ifdef __cplusplus
}
#endif
