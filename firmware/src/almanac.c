/*
 * NocSif sun/moon almanac implementation. See almanac.h for the accuracy notes.
 */
#include "almanac.h"

#include <math.h>

#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)

static double norm360(double a)
{
    a = fmod(a, 360.0);
    return a < 0.0 ? a + 360.0 : a;
}

/* Civil (y,m,d) -> days since the Unix epoch, via Howard Hinnant's proleptic-Gregorian formula. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Julian Day number at 00:00 UTC for the given civil date. */
static double jd_at_utc_midnight(int y, int m, int d)
{
    return (double)days_from_civil(y, m, d) + 2440587.5;
}

/* ---- sun (NOAA) ------------------------------------------------------------------------------ */

/* NOAA equation of time (minutes) and solar declination (degrees) for Julian Day jd. */
static void sun_eqtime_decl(double jd, double *eqtime_min, double *decl_deg)
{
    const double t   = (jd - 2451545.0) / 36525.0;
    const double L0  = norm360(280.46646 + t * (36000.76983 + t * 0.0003032));
    const double M   = norm360(357.52911 + t * (35999.05029 - 0.0001537 * t));
    const double e   = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const double Mr  = M * DEG2RAD;
    const double C   = sin(Mr) * (1.914602 - t * (0.004817 + 0.000014 * t))
                     + sin(2 * Mr) * (0.019993 - 0.000101 * t) + sin(3 * Mr) * 0.000289;
    const double sunlong = L0 + C;
    const double omega   = 125.04 - 1934.136 * t;
    const double lambda  = sunlong - 0.00569 - 0.00478 * sin(omega * DEG2RAD);
    const double eps0    = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const double eps     = (eps0 + 0.00256 * cos(omega * DEG2RAD)) * DEG2RAD;
    *decl_deg = asin(sin(eps) * sin(lambda * DEG2RAD)) * RAD2DEG;
    const double y   = tan(eps / 2.0) * tan(eps / 2.0);
    const double L0r = L0 * DEG2RAD;
    *eqtime_min = 4.0 * RAD2DEG * (y * sin(2 * L0r) - 2.0 * e * sin(Mr) + 4.0 * e * y * sin(Mr) * cos(2 * L0r)
                                   - 0.5 * y * y * sin(4 * L0r) - 1.25 * e * e * sin(2 * Mr));
}

/* Computes sunrise/sunset as minutes after local midnight (NOCSIF_ALM_NONE if the sun stays above or
 * below the horizon all day). Runs a second pass using the declination at the first pass's event
 * times, which sharpens the result over using noon's declination alone. */
static void sun_day(double lat, double lon, double jd0_utc, int utc_off_min,
                    int *rise_min, int *set_min, bool *ok, bool *up_all_day)
{
    const double latr = lat * DEG2RAD;
    *rise_min = *set_min = NOCSIF_ALM_NONE;
    *ok = false; *up_all_day = false;
    /* local noon, expressed as a UTC Julian Day offset */
    double jd_noon = jd0_utc + (720.0 - (double)utc_off_min) / 1440.0;
    for (int pass = 0; pass < 2; pass++) {
        double eq, decl;
        double jd_r = (pass == 0 || *rise_min == NOCSIF_ALM_NONE) ? jd_noon : jd0_utc + (*rise_min - utc_off_min) / 1440.0;
        double jd_s = (pass == 0 || *set_min  == NOCSIF_ALM_NONE) ? jd_noon : jd0_utc + (*set_min  - utc_off_min) / 1440.0;
        /* sunrise hour angle */
        sun_eqtime_decl(jd_r, &eq, &decl);
        double cosha = cos(90.833 * DEG2RAD) / (cos(latr) * cos(decl * DEG2RAD)) - tan(latr) * tan(decl * DEG2RAD);
        if (cosha > 1.0)  { *up_all_day = false; return; }      /* polar night: sun never clears the horizon */
        if (cosha < -1.0) { *up_all_day = true;  return; }      /* polar day: sun never sets                 */
        double ha = acos(cosha) * RAD2DEG;
        double rise_utc = 720.0 - 4.0 * (lon + ha) - eq;        /* minutes after UTC midnight */
        /* sunset hour angle */
        sun_eqtime_decl(jd_s, &eq, &decl);
        cosha = cos(90.833 * DEG2RAD) / (cos(latr) * cos(decl * DEG2RAD)) - tan(latr) * tan(decl * DEG2RAD);
        if (cosha > 1.0)  { *up_all_day = false; return; }
        if (cosha < -1.0) { *up_all_day = true;  return; }
        ha = acos(cosha) * RAD2DEG;
        double set_utc = 720.0 - 4.0 * (lon - ha) - eq;
        *rise_min = (int)lround(rise_utc + utc_off_min);
        *set_min  = (int)lround(set_utc  + utc_off_min);
    }
    *ok = true;
}

/* ---- moon (Meeus, low precision) ------------------------------------------------------------- */

/* Geocentric apparent moon RA/Dec (degrees) and horizontal parallax (degrees) at Julian Day jd, plus
 * the mean elongation used elsewhere to derive the moon's phase. */
static void moon_radec(double jd, double *ra_deg, double *dec_deg, double *par_deg, double *elong_deg)
{
    const double T  = (jd - 2451545.0) / 36525.0;
    const double Lp = norm360(218.3164477 + 481267.88123421 * T);    /* mean longitude        */
    const double D  = norm360(297.8501921 + 445267.1114034  * T);    /* mean elongation       */
    const double M  = norm360(357.5291092 + 35999.0502909   * T);    /* sun's mean anomaly    */
    const double Mp = norm360(134.9633964 + 477198.8675055  * T);    /* moon's mean anomaly   */
    const double F  = norm360(93.2720950  + 483202.0175233  * T);    /* argument of latitude  */
    const double Dr = D * DEG2RAD, Mr = M * DEG2RAD, Mpr = Mp * DEG2RAD, Fr = F * DEG2RAD;

    const double lon = Lp
        + 6.288774 * sin(Mpr)          + 1.274027 * sin(2 * Dr - Mpr)   + 0.658314 * sin(2 * Dr)
        + 0.213618 * sin(2 * Mpr)      - 0.185116 * sin(Mr)             - 0.114332 * sin(2 * Fr)
        + 0.058793 * sin(2 * Dr - 2 * Mpr) + 0.057066 * sin(2 * Dr - Mr - Mpr) + 0.053322 * sin(2 * Dr + Mpr)
        + 0.045758 * sin(2 * Dr - Mr)  - 0.040923 * sin(Mr - Mpr)       - 0.034720 * sin(Dr)
        - 0.030383 * sin(Mr + Mpr)     + 0.015327 * sin(2 * Dr - 2 * Fr) - 0.012528 * sin(Mpr + 2 * Fr)
        + 0.010980 * sin(Mpr - 2 * Fr) + 0.010675 * sin(4 * Dr - Mpr)   + 0.010034 * sin(3 * Mpr);
    const double lat =
          5.128122 * sin(Fr)           + 0.280602 * sin(Mpr + Fr)       + 0.277693 * sin(Mpr - Fr)
        + 0.173237 * sin(2 * Dr - Fr)  + 0.055413 * sin(2 * Dr - Mpr + Fr) + 0.046271 * sin(2 * Dr - Mpr - Fr)
        + 0.032573 * sin(2 * Dr + Fr)  + 0.017198 * sin(2 * Mpr + Fr);
    const double dist_km = 385000.56
        - 20905.355 * cos(Mpr)         - 3699.111 * cos(2 * Dr - Mpr)   - 2955.968 * cos(2 * Dr)
        - 569.925  * cos(2 * Mpr)      + 246.158  * cos(2 * Dr - 2 * Mpr) - 204.586 * cos(2 * Dr - Mr - Mpr)
        - 170.733  * cos(2 * Dr + Mpr) - 152.138  * cos(2 * Dr - Mr)    - 129.620 * cos(Mr - Mpr);

    const double eps  = (23.4392911 - 0.0130042 * T) * DEG2RAD;
    const double lonr = norm360(lon) * DEG2RAD, latr = lat * DEG2RAD;
    const double ra   = atan2(sin(lonr) * cos(eps) - tan(latr) * sin(eps), cos(lonr));
    const double dec  = asin(sin(latr) * cos(eps) + cos(latr) * sin(eps) * sin(lonr));
    *ra_deg    = norm360(ra * RAD2DEG);
    *dec_deg   = dec * RAD2DEG;
    *par_deg   = asin(6378.14 / dist_km) * RAD2DEG;
    *elong_deg = D;
}

/* Greenwich mean sidereal time (degrees) at Julian Day jd. */
static double gmst_deg(double jd)
{
    const double T = (jd - 2451545.0) / 36525.0;
    return norm360(280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * T * T);
}

/* Moon's altitude above the true horizon at jd, minus the standard rise/set threshold h0 (which
 * folds in refraction and the disc's own parallax) — zero crossings of this mark rise/set. */
static double moon_alt_minus_h0(double jd, double latr, double lon)
{
    double ra, dec, par, el;
    moon_radec(jd, &ra, &dec, &par, &el);
    const double lst = norm360(gmst_deg(jd) + lon);
    const double H   = (lst - ra) * DEG2RAD;
    const double decr = dec * DEG2RAD;
    const double sinh = sin(latr) * sin(decr) + cos(latr) * cos(decr) * cos(H);
    const double alt  = asin(sinh) * RAD2DEG;
    const double h0   = 0.7275 * par - 34.0 / 60.0;             /* Meeus rise/set altitude threshold */
    return alt - h0;
}

/* Scans the local day in 10-minute steps to find the first moonrise and first moonset, linearly
 * interpolating between the samples that bracket each altitude crossing. */
static void moon_day(double lat, double lon, double jd0_utc, int utc_off_min, int *rise_min, int *set_min)
{
    const double latr = lat * DEG2RAD;
    *rise_min = *set_min = NOCSIF_ALM_NONE;
    const double jd_local0 = jd0_utc - (double)utc_off_min / 1440.0;   /* local midnight, expressed in UTC */
    const int step = 10;
    double prev = moon_alt_minus_h0(jd_local0, latr, lon);
    for (int m = step; m <= 1440; m += step) {
        double cur = moon_alt_minus_h0(jd_local0 + m / 1440.0, latr, lon);
        if (prev < 0.0 && cur >= 0.0 && *rise_min == NOCSIF_ALM_NONE) {
            *rise_min = (int)lround(m - step + step * (-prev) / (cur - prev));
        } else if (prev >= 0.0 && cur < 0.0 && *set_min == NOCSIF_ALM_NONE) {
            *set_min = (int)lround(m - step + step * prev / (prev - cur));
        }
        prev = cur;
        if (*rise_min != NOCSIF_ALM_NONE && *set_min != NOCSIF_ALM_NONE) break;
    }
    if (*rise_min == 1440) *rise_min = 1439;
    if (*set_min  == 1440) *set_min  = 1439;
}

void nocsif_almanac_day(double lat_deg, double lon_deg, int y, int m, int d, int utc_off_min,
                        nocsif_almanac_day_t *out)
{
    const double jd0 = jd_at_utc_midnight(y, m, d);
    sun_day(lat_deg, lon_deg, jd0, utc_off_min, &out->sunrise_min, &out->sunset_min, &out->sun_ok, &out->sun_up_all_day);
    moon_day(lat_deg, lon_deg, jd0, utc_off_min, &out->moonrise_min, &out->moonset_min);
    /* Moon phase from the mean elongation at local noon: 0 = new moon, 0.5 = full moon. */
    double ra, dec, par, el;
    moon_radec(jd0 + (720.0 - (double)utc_off_min) / 1440.0, &ra, &dec, &par, &el);
    out->moon_age = (float)(el / 360.0);
}
