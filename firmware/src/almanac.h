/*
 * NocSif — sun / moon almanac (PLAN §4.14 "watch-peek sun/moon almanac countdowns").
 *
 * Pure arithmetic, no hardware, no allocation: for a latitude / longitude and a LOCAL calendar day it
 * returns the sun's rise and set, the moon's rise and set (each as minutes after local midnight) and the
 * moon's age in its cycle. The peek watchface turns these into the "sets in 2h05 / rises in 4h10" line.
 *
 * Method + honesty:
 *   - Sun: the NOAA sunrise equation (equation of time + declination at local noon, zenith 90.833° for
 *     refraction + the solar disc). Within ~1 minute anywhere the sun rises and sets; polar day / night
 *     report "no event".
 *   - Moon: a low-precision Meeus ecliptic position (the leading dozen longitude terms, eight latitude
 *     terms, the leading distance terms for parallax) → equatorial → altitude against local sidereal
 *     time, scanned across the local day in 10-minute steps and linearly interpolated at each crossing of
 *     h0 = 0.7275·parallax − 34′ (Meeus' standard moonrise altitude). Typically within a few minutes at
 *     mid latitudes; can be off more at high latitude and reports "no event" on the days the moon does
 *     not rise or set (roughly one day a month for each).
 *   - Costs a few hundred double-precision trig calls — run it once per day / location change, not per
 *     tick. Not reentrant-sensitive (no statics).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOCSIF_ALM_NONE (-1)   /* no such event on this local day (polar day/night; the moon's skip day) */

typedef struct {
    bool  sun_ok;         /* the sun both rises and sets today (false = polar day / polar night)      */
    bool  sun_up_all_day; /* when !sun_ok: true = polar day, false = polar night                       */
    int   sunrise_min;    /* minutes after local midnight, or NOCSIF_ALM_NONE                          */
    int   sunset_min;
    int   moonrise_min;   /* minutes after local midnight, or NOCSIF_ALM_NONE                          */
    int   moonset_min;
    float moon_age;       /* 0..1 through the synodic cycle: 0 new, 0.25 first quarter, 0.5 full, …   */
} nocsif_almanac_day_t;

/* Compute the almanac for the local calendar day (y, m, d) at (lat_deg, lon_deg; +N / +E) whose local
 * clock runs utc_off_min minutes ahead of UTC (negative west). */
void nocsif_almanac_day(double lat_deg, double lon_deg, int y, int m, int d, int utc_off_min,
                        nocsif_almanac_day_t *out);

#ifdef __cplusplus
}
#endif
