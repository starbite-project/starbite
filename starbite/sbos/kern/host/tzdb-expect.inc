/* generated from Python's zoneinfo (the Mac's tzdata) on 2026-11-12: every zone's offset in mid-January and
 * mid-July 2027, and every transition in 2026 and 2027 to the second (Vancouver: from 2026-03-09, after B.C.'s
 * last change to UTC-7 all year) */
static const struct { const char *z; int jan, jul; } OFFS[] = {
    { "America/Anchorage", -540, -480 },
    { "America/Los_Angeles", -480, -420 },
    { "America/Vancouver", -420, -420 },
    { "America/Phoenix", -420, -420 },
    { "America/Denver", -420, -360 },
    { "America/Chicago", -360, -300 },
    { "America/Mexico_City", -360, -360 },
    { "America/New_York", -300, -240 },
    { "America/Toronto", -300, -240 },
    { "America/Bogota", -300, -300 },
    { "America/Lima", -300, -300 },
    { "America/Caracas", -240, -240 },
    { "America/Halifax", -240, -180 },
    { "America/St_Johns", -210, -150 },
    { "America/Sao_Paulo", -180, -180 },
    { "America/Argentina/Buenos_Aires", -180, -180 },
    { "Etc/UTC", 0, 0 },
    { "Europe/Lisbon", 0, 60 },
    { "Europe/London", 0, 60 },
    { "Europe/Dublin", 0, 60 },
    { "Europe/Madrid", 60, 120 },
    { "Europe/Paris", 60, 120 },
    { "Europe/Amsterdam", 60, 120 },
    { "Europe/Berlin", 60, 120 },
    { "Europe/Rome", 60, 120 },
    { "Europe/Stockholm", 60, 120 },
    { "Europe/Warsaw", 60, 120 },
    { "Europe/Athens", 120, 180 },
    { "Europe/Helsinki", 120, 180 },
    { "Europe/Kyiv", 120, 180 },
    { "Europe/Istanbul", 180, 180 },
    { "Europe/Moscow", 180, 180 },
    { "Africa/Dakar", 0, 0 },
    { "Africa/Accra", 0, 0 },
    { "Africa/Lagos", 60, 60 },
    { "Africa/Kinshasa", 60, 60 },
    { "Africa/Algiers", 60, 60 },
    { "Africa/Johannesburg", 120, 120 },
    { "Africa/Maputo", 120, 120 },
    { "Africa/Nairobi", 180, 180 },
    { "Africa/Addis_Ababa", 180, 180 },
    { "Asia/Riyadh", 180, 180 },
    { "Asia/Tehran", 210, 210 },
    { "Asia/Dubai", 240, 240 },
    { "Asia/Karachi", 300, 300 },
    { "Asia/Kolkata", 330, 330 },
    { "Asia/Kathmandu", 345, 345 },
    { "Asia/Dhaka", 360, 360 },
    { "Asia/Bangkok", 420, 420 },
    { "Asia/Jakarta", 420, 420 },
    { "Asia/Singapore", 480, 480 },
    { "Asia/Shanghai", 480, 480 },
    { "Asia/Hong_Kong", 480, 480 },
    { "Asia/Taipei", 480, 480 },
    { "Asia/Manila", 480, 480 },
    { "Asia/Seoul", 540, 540 },
    { "Asia/Tokyo", 540, 540 },
    { "Pacific/Honolulu", -600, -600 },
    { "Australia/Perth", 480, 480 },
    { "Australia/Darwin", 570, 570 },
    { "Australia/Adelaide", 630, 570 },
    { "Australia/Brisbane", 600, 600 },
    { "Australia/Sydney", 660, 600 },
    { "Australia/Melbourne", 660, 600 },
    { "Australia/Hobart", 660, 600 },
    { "Pacific/Guam", 600, 600 },
    { "Pacific/Auckland", 780, 720 },
    { "Pacific/Fiji", 720, 720 },
    { "Pacific/Tongatapu", 780, 780 },
};
static const struct { const char *z; long long t; int before, after; } TRANS[] = {
    { "America/Anchorage", 1772967600LL, -540, -480 },   /* 2026-03-08 11:00:00 UTC */
    { "America/Anchorage", 1793527200LL, -480, -540 },   /* 2026-11-01 10:00:00 UTC */
    { "America/Anchorage", 1805022000LL, -540, -480 },   /* 2027-03-14 11:00:00 UTC */
    { "America/Anchorage", 1825581600LL, -480, -540 },   /* 2027-11-07 10:00:00 UTC */
    { "America/Los_Angeles", 1772964000LL, -480, -420 },   /* 2026-03-08 10:00:00 UTC */
    { "America/Los_Angeles", 1793523600LL, -420, -480 },   /* 2026-11-01 09:00:00 UTC */
    { "America/Los_Angeles", 1805018400LL, -480, -420 },   /* 2027-03-14 10:00:00 UTC */
    { "America/Los_Angeles", 1825578000LL, -420, -480 },   /* 2027-11-07 09:00:00 UTC */
    { "America/Denver", 1772960400LL, -420, -360 },   /* 2026-03-08 09:00:00 UTC */
    { "America/Denver", 1793520000LL, -360, -420 },   /* 2026-11-01 08:00:00 UTC */
    { "America/Denver", 1805014800LL, -420, -360 },   /* 2027-03-14 09:00:00 UTC */
    { "America/Denver", 1825574400LL, -360, -420 },   /* 2027-11-07 08:00:00 UTC */
    { "America/Chicago", 1772956800LL, -360, -300 },   /* 2026-03-08 08:00:00 UTC */
    { "America/Chicago", 1793516400LL, -300, -360 },   /* 2026-11-01 07:00:00 UTC */
    { "America/Chicago", 1805011200LL, -360, -300 },   /* 2027-03-14 08:00:00 UTC */
    { "America/Chicago", 1825570800LL, -300, -360 },   /* 2027-11-07 07:00:00 UTC */
    { "America/New_York", 1772953200LL, -300, -240 },   /* 2026-03-08 07:00:00 UTC */
    { "America/New_York", 1793512800LL, -240, -300 },   /* 2026-11-01 06:00:00 UTC */
    { "America/New_York", 1805007600LL, -300, -240 },   /* 2027-03-14 07:00:00 UTC */
    { "America/New_York", 1825567200LL, -240, -300 },   /* 2027-11-07 06:00:00 UTC */
    { "America/Toronto", 1772953200LL, -300, -240 },   /* 2026-03-08 07:00:00 UTC */
    { "America/Toronto", 1793512800LL, -240, -300 },   /* 2026-11-01 06:00:00 UTC */
    { "America/Toronto", 1805007600LL, -300, -240 },   /* 2027-03-14 07:00:00 UTC */
    { "America/Toronto", 1825567200LL, -240, -300 },   /* 2027-11-07 06:00:00 UTC */
    { "America/Halifax", 1772949600LL, -240, -180 },   /* 2026-03-08 06:00:00 UTC */
    { "America/Halifax", 1793509200LL, -180, -240 },   /* 2026-11-01 05:00:00 UTC */
    { "America/Halifax", 1805004000LL, -240, -180 },   /* 2027-03-14 06:00:00 UTC */
    { "America/Halifax", 1825563600LL, -180, -240 },   /* 2027-11-07 05:00:00 UTC */
    { "America/St_Johns", 1772947800LL, -210, -150 },   /* 2026-03-08 05:30:00 UTC */
    { "America/St_Johns", 1793507400LL, -150, -210 },   /* 2026-11-01 04:30:00 UTC */
    { "America/St_Johns", 1805002200LL, -210, -150 },   /* 2027-03-14 05:30:00 UTC */
    { "America/St_Johns", 1825561800LL, -150, -210 },   /* 2027-11-07 04:30:00 UTC */
    { "Europe/Lisbon", 1774746000LL, 0, 60 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Lisbon", 1792890000LL, 60, 0 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Lisbon", 1806195600LL, 0, 60 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Lisbon", 1824944400LL, 60, 0 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/London", 1774746000LL, 0, 60 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/London", 1792890000LL, 60, 0 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/London", 1806195600LL, 0, 60 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/London", 1824944400LL, 60, 0 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Dublin", 1774746000LL, 0, 60 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Dublin", 1792890000LL, 60, 0 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Dublin", 1806195600LL, 0, 60 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Dublin", 1824944400LL, 60, 0 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Madrid", 1774746000LL, 60, 120 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Madrid", 1792890000LL, 120, 60 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Madrid", 1806195600LL, 60, 120 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Madrid", 1824944400LL, 120, 60 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Paris", 1774746000LL, 60, 120 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Paris", 1792890000LL, 120, 60 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Paris", 1806195600LL, 60, 120 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Paris", 1824944400LL, 120, 60 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Amsterdam", 1774746000LL, 60, 120 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Amsterdam", 1792890000LL, 120, 60 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Amsterdam", 1806195600LL, 60, 120 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Amsterdam", 1824944400LL, 120, 60 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Berlin", 1774746000LL, 60, 120 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Berlin", 1792890000LL, 120, 60 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Berlin", 1806195600LL, 60, 120 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Berlin", 1824944400LL, 120, 60 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Rome", 1774746000LL, 60, 120 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Rome", 1792890000LL, 120, 60 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Rome", 1806195600LL, 60, 120 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Rome", 1824944400LL, 120, 60 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Stockholm", 1774746000LL, 60, 120 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Stockholm", 1792890000LL, 120, 60 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Stockholm", 1806195600LL, 60, 120 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Stockholm", 1824944400LL, 120, 60 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Warsaw", 1774746000LL, 60, 120 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Warsaw", 1792890000LL, 120, 60 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Warsaw", 1806195600LL, 60, 120 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Warsaw", 1824944400LL, 120, 60 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Athens", 1774746000LL, 120, 180 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Athens", 1792890000LL, 180, 120 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Athens", 1806195600LL, 120, 180 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Athens", 1824944400LL, 180, 120 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Helsinki", 1774746000LL, 120, 180 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Helsinki", 1792890000LL, 180, 120 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Helsinki", 1806195600LL, 120, 180 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Helsinki", 1824944400LL, 180, 120 },   /* 2027-10-31 01:00:00 UTC */
    { "Europe/Kyiv", 1774746000LL, 120, 180 },   /* 2026-03-29 01:00:00 UTC */
    { "Europe/Kyiv", 1792890000LL, 180, 120 },   /* 2026-10-25 01:00:00 UTC */
    { "Europe/Kyiv", 1806195600LL, 120, 180 },   /* 2027-03-28 01:00:00 UTC */
    { "Europe/Kyiv", 1824944400LL, 180, 120 },   /* 2027-10-31 01:00:00 UTC */
    { "Australia/Adelaide", 1775320200LL, 630, 570 },   /* 2026-04-04 16:30:00 UTC */
    { "Australia/Adelaide", 1791045000LL, 570, 630 },   /* 2026-10-03 16:30:00 UTC */
    { "Australia/Adelaide", 1806769800LL, 630, 570 },   /* 2027-04-03 16:30:00 UTC */
    { "Australia/Adelaide", 1822494600LL, 570, 630 },   /* 2027-10-02 16:30:00 UTC */
    { "Australia/Sydney", 1775318400LL, 660, 600 },   /* 2026-04-04 16:00:00 UTC */
    { "Australia/Sydney", 1791043200LL, 600, 660 },   /* 2026-10-03 16:00:00 UTC */
    { "Australia/Sydney", 1806768000LL, 660, 600 },   /* 2027-04-03 16:00:00 UTC */
    { "Australia/Sydney", 1822492800LL, 600, 660 },   /* 2027-10-02 16:00:00 UTC */
    { "Australia/Melbourne", 1775318400LL, 660, 600 },   /* 2026-04-04 16:00:00 UTC */
    { "Australia/Melbourne", 1791043200LL, 600, 660 },   /* 2026-10-03 16:00:00 UTC */
    { "Australia/Melbourne", 1806768000LL, 660, 600 },   /* 2027-04-03 16:00:00 UTC */
    { "Australia/Melbourne", 1822492800LL, 600, 660 },   /* 2027-10-02 16:00:00 UTC */
    { "Australia/Hobart", 1775318400LL, 660, 600 },   /* 2026-04-04 16:00:00 UTC */
    { "Australia/Hobart", 1791043200LL, 600, 660 },   /* 2026-10-03 16:00:00 UTC */
    { "Australia/Hobart", 1806768000LL, 660, 600 },   /* 2027-04-03 16:00:00 UTC */
    { "Australia/Hobart", 1822492800LL, 600, 660 },   /* 2027-10-02 16:00:00 UTC */
    { "Pacific/Auckland", 1775311200LL, 780, 720 },   /* 2026-04-04 14:00:00 UTC */
    { "Pacific/Auckland", 1790431200LL, 720, 780 },   /* 2026-09-26 14:00:00 UTC */
    { "Pacific/Auckland", 1806760800LL, 780, 720 },   /* 2027-04-03 14:00:00 UTC */
    { "Pacific/Auckland", 1821880800LL, 720, 780 },   /* 2027-09-25 14:00:00 UTC */
};
