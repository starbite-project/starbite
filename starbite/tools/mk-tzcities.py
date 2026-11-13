#!/usr/bin/env python3
"""Starbite OS: the city table behind World Clock and Weather search (Track A, 8a), from GeoNames.

Usage: python3 -I mk-tzcities.py cities15000.txt countryInfo.txt starbite/sbos/kern/tzdb.c OUT.c
(the GeoNames files from https://download.geonames.org/export/dump/: cities15000.zip unpacked, countryInfo.txt).

Keeps cities whose time zone is one of kern/tzdb.c's zones (the ones Starbite OS models), at most MAX of them:
every capital, each zone's own city, then the largest by population. Writes a C table: each city's name (UTF-8),
its country (an index into a country table), its zone (an index into tzdb[]) and its place to 0.0001 degree.

The data: GeoNames (https://www.geonames.org), licensed CC BY 4.0. The credit goes in Settings > About > Legal."""
import re
import sys

MAX = 3000

# IANA zones whose offset and daylight rule in 2026 are the same as a zone tzdb.c models: their cities take that zone
# (the clock is right; the abbreviation is the modelled zone's). Only exact matches: zones with transitions of their
# own (Moldova, Egypt, Chile, ...) stay out rather than show a wrong time.
ALIAS = {}
def _al(target, *zs):
    for z in zs:
        ALIAS[z] = target
_al("Europe/Berlin", "Europe/Oslo", "Europe/Copenhagen", "Europe/Zurich", "Europe/Vienna", "Europe/Brussels",
    "Europe/Prague", "Europe/Budapest", "Europe/Bratislava", "Europe/Ljubljana", "Europe/Zagreb", "Europe/Belgrade",
    "Europe/Sarajevo", "Europe/Skopje", "Europe/Podgorica", "Europe/Tirane", "Europe/Luxembourg", "Europe/Monaco",
    "Europe/Malta", "Europe/Andorra", "Europe/Gibraltar", "Europe/Vaduz", "Europe/San_Marino", "Europe/Vatican",
    "Europe/Busingen", "Africa/Ceuta", "Arctic/Longyearbyen")
_al("Europe/Helsinki", "Europe/Riga", "Europe/Tallinn", "Europe/Vilnius", "Europe/Sofia", "Europe/Bucharest",
    "Asia/Nicosia", "Asia/Famagusta", "Europe/Mariehamn")
_al("Europe/Lisbon", "Atlantic/Canary", "Atlantic/Faroe", "Atlantic/Madeira")
_al("Europe/London", "Europe/Guernsey", "Europe/Jersey", "Europe/Isle_of_Man")
_al("America/New_York", "America/Detroit", "America/Indiana/Indianapolis", "America/Indiana/Vincennes",
    "America/Indiana/Winamac", "America/Indiana/Marengo", "America/Indiana/Petersburg", "America/Indiana/Vevay",
    "America/Kentucky/Louisville", "America/Kentucky/Monticello", "America/Nassau")
_al("America/Chicago", "America/Indiana/Knox", "America/Indiana/Tell_City", "America/Menominee",
    "America/North_Dakota/Center", "America/North_Dakota/New_Salem", "America/North_Dakota/Beulah",
    "America/Winnipeg", "America/Matamoros")
_al("America/Denver", "America/Boise", "America/Edmonton", "America/Ciudad_Juarez")
_al("America/Los_Angeles", "America/Tijuana")
_al("America/Mexico_City", "America/Regina", "America/Guatemala", "America/El_Salvador", "America/Tegucigalpa",
    "America/Managua", "America/Costa_Rica", "America/Monterrey", "America/Merida", "America/Chihuahua",
    "America/Bahia_Banderas")
_al("America/Phoenix", "America/Hermosillo", "America/Mazatlan")
_al("America/Bogota", "America/Guayaquil", "America/Panama", "America/Cancun", "America/Jamaica")
_al("America/Caracas", "America/La_Paz", "America/Manaus", "America/Santo_Domingo", "America/Puerto_Rico",
    "America/Port_of_Spain", "America/Barbados", "America/Guyana")
_al("America/Sao_Paulo", "America/Montevideo", "America/Belem", "America/Fortaleza", "America/Recife",
    "America/Bahia", "America/Maceio", "America/Cayenne", "America/Paramaribo")
_al("America/Argentina/Buenos_Aires", "America/Argentina/Cordoba", "America/Argentina/Salta",
    "America/Argentina/Jujuy", "America/Argentina/Tucuman", "America/Argentina/Catamarca",
    "America/Argentina/La_Rioja", "America/Argentina/San_Juan", "America/Argentina/Mendoza",
    "America/Argentina/San_Luis", "America/Argentina/Rio_Gallegos", "America/Argentina/Ushuaia")
_al("Africa/Lagos", "Africa/Douala", "Africa/Libreville", "Africa/Luanda", "Africa/Malabo", "Africa/Niamey",
    "Africa/Ndjamena", "Africa/Porto-Novo", "Africa/Bangui", "Africa/Brazzaville", "Africa/Tunis")
_al("Africa/Maputo", "Africa/Harare", "Africa/Lusaka", "Africa/Lubumbashi", "Africa/Gaborone", "Africa/Blantyre",
    "Africa/Bujumbura", "Africa/Kigali", "Africa/Windhoek", "Africa/Tripoli", "Africa/Khartoum", "Africa/Juba")
_al("Africa/Johannesburg", "Africa/Maseru", "Africa/Mbabane")
_al("Africa/Nairobi", "Africa/Dar_es_Salaam", "Africa/Kampala", "Africa/Mogadishu", "Africa/Djibouti",
    "Africa/Asmara")
_al("Africa/Accra", "Africa/Abidjan", "Africa/Bamako", "Africa/Conakry", "Africa/Freetown", "Africa/Lome",
    "Africa/Ouagadougou", "Africa/Monrovia", "Africa/Banjul", "Africa/Bissau", "Africa/Nouakchott")
_al("Asia/Riyadh", "Asia/Kuwait", "Asia/Bahrain", "Asia/Qatar", "Asia/Aden", "Asia/Baghdad")
_al("Asia/Dubai", "Asia/Muscat", "Asia/Baku", "Asia/Tbilisi", "Asia/Yerevan")
_al("Asia/Karachi", "Asia/Tashkent", "Asia/Samarkand", "Asia/Almaty", "Asia/Dushanbe", "Asia/Ashgabat")
_al("Asia/Kolkata", "Asia/Colombo")
_al("Asia/Bangkok", "Asia/Ho_Chi_Minh", "Asia/Phnom_Penh", "Asia/Vientiane")
_al("Asia/Jakarta", "Asia/Pontianak")
_al("Asia/Singapore", "Asia/Kuala_Lumpur", "Asia/Kuching", "Asia/Makassar", "Asia/Brunei")
_al("Asia/Hong_Kong", "Asia/Macau")
_al("Asia/Seoul", "Asia/Pyongyang")
_al("Asia/Tokyo", "Asia/Jayapura")


def main():
    cities_p, countries_p, tzdb_p, out_p = sys.argv[1:5]
    zones = re.findall(r'^\s*\{ "([A-Za-z_]+/[A-Za-z_/+-]+)", "([^"]*)"', open(tzdb_p, encoding="utf-8").read(), re.M)
    zone_ix = {z: i for i, (z, _) in enumerate(zones)}
    zone_city = {z: c.encode("latin-1", "backslashreplace").decode("unicode_escape").encode("latin-1").decode("utf-8")
                 for z, c in zones}
    cnames = {}
    for line in open(countries_p, encoding="utf-8"):
        if line.startswith("#"):
            continue
        f = line.rstrip("\n").split("\t")
        if len(f) > 4:
            cnames[f[0]] = f[4]
    rows = []
    for line in open(cities_p, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) < 18:
            continue
        f[17] = ALIAS.get(f[17], f[17])
        if f[17] not in zone_ix or f[8] not in cnames:
            continue
        name = f[1].strip()
        if not name or len(name.encode("utf-8")) > 40:
            continue
        rows.append(dict(name=name, cc=f[8], lat=float(f[4]), lon=float(f[5]), code=f[7],
                         pop=int(f[14] or 0), zone=f[17]))
    keep, seen = [], set()

    def take(r):
        key = (r["name"], r["cc"])
        if key in seen:
            return
        seen.add(key)
        keep.append(r)

    for r in sorted((r for r in rows if r["code"] == "PPLC"), key=lambda r: -r["pop"]):
        take(r)
    for z, city in zone_city.items():   # each zone's own city: the most populous one of that name in the zone
        m = [r for r in rows if r["zone"] == z and r["name"] == city]
        if m:
            take(max(m, key=lambda r: r["pop"]))
    for r in sorted(rows, key=lambda r: -r["pop"]):
        if len(keep) >= MAX:
            break
        take(r)
    keep.sort(key=lambda r: (r["name"].casefold(), r["cc"]))
    ccs = sorted({r["cc"] for r in keep})
    cc_ix = {c: i for i, c in enumerate(ccs)}

    def cstr(s):
        out = []
        for b in s.encode("utf-8"):
            out.append("\\%03o" % b if b >= 0x80 or b in (0x22, 0x5c) or b < 0x20 else chr(b))   # octal: never runs on
        return '"' + "".join(out) + '"'

    with open(out_p, "w", encoding="utf-8") as o:
        o.write("/* SPDX-License-Identifier: CC-BY-4.0\n")
        o.write(" * Generated by starbite/tools/mk-tzcities.py: do not edit. %d cities, %d countries, from GeoNames\n" % (len(keep), len(ccs)))
        o.write(" * (https://www.geonames.org, CC BY 4.0: the credit is in Settings > About > Legal), the cities in\n")
        o.write(" * kern/tzdb.c's zones: every capital, each zone's own city, then the largest. Sorted by name. */\n")
        o.write('#include "tzcities.h"\n\n')
        o.write("const char *const tzc_country[%d] = {\n" % len(ccs))
        for c in ccs:
            o.write("    %s,\n" % cstr(cnames[c]))
        o.write("};\n")
        o.write("const int tzc_ncountry = %d;\n\n" % len(ccs))
        o.write("const struct tzcity tzcities[%d] = {\n" % len(keep))
        for r in keep:
            o.write("    { %s, %d, %d, %d, %d },\n" % (cstr(r["name"]), cc_ix[r["cc"]], zone_ix[r["zone"]],
                                                     round(r["lat"] * 1e4), round(r["lon"] * 1e4)))
        o.write("};\n")
        o.write("const int tzcities_n = %d;\n" % len(keep))
    print("tzcities: %d cities, %d countries, %d zones covered" % (len(keep), len(ccs), len({r['zone'] for r in keep})))


if __name__ == "__main__":
    main()
