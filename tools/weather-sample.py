#!/usr/bin/env python3
"""Turn an Open-Meteo forecast into a WeatherKit REST response.

The Weather app decodes a `WeatherKit.Weather` from WeatherKit's REST JSON
(the keys are in WeatherDaemon.framework: currentWeather, forecastHourly,
forecastDaily, forecastNextHour, weatherAlerts; the shapes are Apple's public
WeatherKit REST API).  This port has no FairPlay identity, so the shim
(userland/src/inject_weathershim.m) has to be the data source; this is the
conversion the shim performs, kept here where it can be read and tested on a
host -- and where its output can be embedded as a sample for the first run.

    tools/weather-sample.py --lat 34.34 --lon 108.94 -o out/weather.json
    tools/weather-sample.py --lat 34.34 --lon 108.94 --from-file om.json

With no --from-file it fetches api.open-meteo.com (no key).  Units are the
WeatherKit metric system ("m"): Celsius, km/h, hPa, km, mm, 0..1 fractions.
Apple's field list is WeatherKit REST API, "Weather" and the types under it;
the condition codes are WeatherCondition's raw values.
"""
import argparse
import datetime as dt
import json
import socket
import sys
import urllib.parse
import urllib.request

# Open-Meteo pieces this needs.  "current" is a snapshot, "hourly"/"daily"
# are the series the app renders.
CURRENT = ("temperature_2m,relative_humidity_2m,apparent_temperature,dew_point_2m,"
           "is_day,precipitation,weather_code,cloud_cover,pressure_msl,"
           "wind_speed_10m,wind_direction_10m,wind_gusts_10m")
HOURLY = ("temperature_2m,relative_humidity_2m,apparent_temperature,dew_point_2m,"
          "precipitation_probability,precipitation,weather_code,cloud_cover,"
          "pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m,"
          "visibility,uv_index,is_day")
DAILY = ("weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,"
         "uv_index_max,precipitation_sum,precipitation_probability_max,"
         "wind_speed_10m_max,wind_direction_10m_dominant")

# WMO 4677 code -> WeatherCondition raw value (WeatherKit's own names).
WMO = {
    0: "clear", 1: "mostlyClear", 2: "partlyCloudy", 3: "cloudy",
    45: "foggy", 48: "foggy",
    51: "drizzle", 53: "drizzle", 55: "drizzle",
    56: "freezingDrizzle", 57: "freezingDrizzle",
    61: "rain", 63: "rain", 65: "heavyRain",
    66: "freezingRain", 67: "freezingRain",
    71: "flurries", 73: "snow", 75: "heavySnow", 77: "flurries",
    80: "rain", 81: "rain", 82: "heavyRain",
    85: "flurries", 86: "heavySnow",
    95: "thunderstorms", 96: "strongStorms", 99: "strongStorms",
}


def condition(code):
    return WMO.get(int(code if code is not None else 0), "clear")


def precipitation_type(code):
    code = int(code if code is not None else 0)
    if code in (71, 73, 75, 77, 85, 86):
        return "snow"
    if code in (56, 57, 66, 67):
        return "sleet"
    if code in (96, 99):
        return "hail"
    if code in (51, 53, 55, 61, 63, 65, 80, 81, 82, 95):
        return "rain"
    return "clear"


def fraction(percent):
    return None if percent is None else round(float(percent) / 100.0, 3)


def iso(text):
    """'2026-11-16T15:00' (Open-Meteo, location time) -> '2026-11-16T15:00:00Z'.

    Open-Meteo is asked for timezone=UTC so the wall clock is UTC and the
    'Z' is true; the app formats in the device's zone itself.
    """
    if text is None or text == "":
        return None
    if len(text) == 16:
        text += ":00"
    return text + "Z" if not text.endswith("Z") else text


def metadata(lat, lon, now, expire_hours=1):
    return {
        "attributionURL": "https://weather-data.apple.com/legal-attribution.html",
        "expireTime": iso((now + dt.timedelta(hours=expire_hours)).strftime("%Y-%m-%dT%H:%M:%S")),
        "language": "en",
        "latitude": lat,
        "longitude": lon,
        "providerLogo": "https://weather-data.apple.com/assets/branding/square-mark.png",
        "providerName": "Open-Meteo",
        "readTime": iso(now.strftime("%Y-%m-%dT%H:%M:%S")),
        "reportedTime": iso(now.strftime("%Y-%m-%dT%H:%M:%S")),
        "temporarilyUnavailable": False,
        "units": "m",
        "version": 1,
    }


def convert(om, lat, lon):
    now = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None, microsecond=0)
    cur = om["current"]
    hourly = om["hourly"]
    daily = om["daily"]
    meta = metadata(lat, lon, now)

    current = {
        "asOf": iso(cur.get("time")),
        "cloudCover": fraction(cur.get("cloud_cover")),
        "conditionCode": condition(cur.get("weather_code")),
        "daylight": bool(cur.get("is_day")),
        "humidity": fraction(cur.get("relative_humidity_2m")),
        "precipitationIntensity": cur.get("precipitation"),
        "pressure": cur.get("pressure_msl"),
        "pressureTrend": "steady",
        "temperature": cur.get("temperature_2m"),
        "temperatureApparent": cur.get("apparent_temperature"),
        "temperatureDewPoint": cur.get("dew_point_2m"),
        "uvIndex": int(round(cur.get("uv_index") or 0)),
        "visibility": None,
        "windDirection": cur.get("wind_direction_10m"),
        "windGust": cur.get("wind_gusts_10m"),
        "windSpeed": cur.get("wind_speed_10m"),
    }
    current = {k: v for k, v in current.items() if v is not None}

    hours = []
    for i, t in enumerate(hourly["time"]):
        hour = {
            "forecastStart": iso(t),
            "cloudCover": fraction(hourly["cloud_cover"][i]),
            "conditionCode": condition(hourly["weather_code"][i]),
            "daylight": bool(hourly["is_day"][i]),
            "humidity": fraction(hourly["relative_humidity_2m"][i]),
            "precipitationChance": fraction(hourly["precipitation_probability"][i]),
            "precipitationType": precipitation_type(hourly["weather_code"][i]),
            "pressure": hourly["pressure_msl"][i],
            "pressureTrend": "steady",
            "temperature": hourly["temperature_2m"][i],
            "temperatureApparent": hourly["apparent_temperature"][i],
            "temperatureDewPoint": hourly["dew_point_2m"][i],
            "uvIndex": int(round(hourly["uv_index"][i] or 0)),
            "visibility": None if hourly["visibility"][i] is None else round(hourly["visibility"][i] / 1000.0, 2),
            "windDirection": hourly["wind_direction_10m"][i],
            "windGust": hourly["wind_gusts_10m"][i],
            "windSpeed": hourly["wind_speed_10m"][i],
            "precipitationAmount": hourly["precipitation"][i],
        }
        hours.append({k: v for k, v in hour.items() if v is not None})

    days = []
    for i, t in enumerate(daily["time"]):
        start = dt.datetime.fromisoformat(t)
        day = {
            "forecastStart": iso(t + "T00:00:00"),
            "forecastEnd": iso((start + dt.timedelta(days=1)).strftime("%Y-%m-%dT%H:%M:%S")),
            "conditionCode": condition(daily["weather_code"][i]),
            "temperatureMax": daily["temperature_2m_max"][i],
            "temperatureMin": daily["temperature_2m_min"][i],
            "precipitationChance": fraction(daily["precipitation_probability_max"][i]),
            "precipitationType": precipitation_type(daily["weather_code"][i]),
            "precipitationAmount": daily["precipitation_sum"][i],
            "snowfallAmount": 0,
            "maxUvIndex": int(round(daily["uv_index_max"][i] or 0)),
            "sunrise": iso(daily["sunrise"][i]),
            "sunset": iso(daily["sunset"][i]),
            "moonPhase": "full",
            "windSpeed": daily["wind_speed_10m_max"][i],
            "windDirection": daily["wind_direction_10m_dominant"][i],
            # The app's daily row draws from these two; without them it may
            # fall back to the day as a whole.
            "daytimeForecast": {
                "forecastStart": iso(t + "T06:00:00"),
                "forecastEnd": iso(t + "T18:00:00"),
                "cloudCover": fraction(hourly["cloud_cover"][i * 24 + 12]) if len(hourly["time"]) > i * 24 + 12 else 0.5,
                "conditionCode": condition(daily["weather_code"][i]),
                "humidity": fraction(hourly["relative_humidity_2m"][i * 24 + 12]) if len(hourly["time"]) > i * 24 + 12 else 0.5,
                "precipitationChance": fraction(daily["precipitation_probability_max"][i]),
                "precipitationType": precipitation_type(daily["weather_code"][i]),
                "precipitationAmount": daily["precipitation_sum"][i],
                "snowfallAmount": 0,
                "windSpeed": daily["wind_speed_10m_max"][i],
                "windDirection": daily["wind_direction_10m_dominant"][i],
            },
        }
        days.append({k: v for k, v in day.items() if v is not None})

    return {
        "currentWeather": current,
        "forecastHourly": {"hours": hours, "metadata": meta},
        "forecastDaily": {"days": days, "metadata": meta},
        "weatherAlerts": {"alerts": [], "metadata": meta},
    }


def fetch(lat, lon):
    # This host resolves api.open-meteo.com to an IPv6 address first and the
    # WSL network has no route to it; the phone's stack is IPv4-only as well,
    # so ask for IPv4 (a full getaddrinfo filter would be the general fix).
    real_getaddrinfo = socket.getaddrinfo
    socket.getaddrinfo = lambda host, port, family=0, type=0, proto=0, flags=0: \
        real_getaddrinfo(host, port, socket.AF_INET, type, proto, flags)
    try:
        return fetch_url(lat, lon)
    finally:
        socket.getaddrinfo = real_getaddrinfo


def fetch_url(lat, lon):
    query = urllib.parse.urlencode({
        "latitude": lat, "longitude": lon,
        "current": CURRENT, "hourly": HOURLY, "daily": DAILY,
        "timezone": "UTC", "forecast_days": 10,
        "wind_speed_unit": "kmh",
    })
    url = "https://api.open-meteo.com/v1/forecast?" + query
    with urllib.request.urlopen(url, timeout=30) as response:
        return json.load(response)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lat", type=float, required=True)
    parser.add_argument("--lon", type=float, required=True)
    parser.add_argument("--from-file", help="an Open-Meteo response to convert instead of fetching")
    parser.add_argument("-o", "--out", help="write here instead of stdout")
    args = parser.parse_args()

    if args.from_file:
        with open(args.from_file, "r", encoding="utf-8") as f:
            om = json.load(f)
    else:
        om = fetch(args.lat, args.lon)
    out = convert(om, args.lat, args.lon)
    text = json.dumps(out, ensure_ascii=False, separators=(",", ":"))
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"weather-sample: {args.out}: {len(text)} bytes", file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()
