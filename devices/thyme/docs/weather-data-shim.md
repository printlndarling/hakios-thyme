# A Weather that shows numbers without Apple's FairPlay

The owner's Weather app resolves the place and then stays on its loading
screen.  The reason is not location and not the daemon's JWT switch: the app's
own WeatherKit client carries the `fairplay-client` entitlement, looks
`com.apple.fairplayd` and `.versioned` up at startup, and never asks
`com.apple.weatherd.weatherkit` for anything.  The port has no fairplayd and no
SEP, and a stand-in that owns both names changes nothing: the app looks them
up, does not connect, and goes nowhere (CHECKLIST, "Weather: the app's own
fairplayd gate"; `logs/140045-run.log`).  Apple's forecast service wants a
device FairPlay identity that this hardware cannot produce, so the stock data
path cannot be turned on by a switch.

What can be done is to put a data source where WeatherKit's client would fetch:
the app renders a `WeatherKit.Weather`; that type is public and `Codable`, so a
forecast from any service can be turned into one and handed over inside the
process.

## The pieces

1. **The model.**  `WeatherKit.Weather` (and `CurrentWeather`, `HourWeather`,
   `DayWeather`, `WeatherCondition`, the `Measurement` values) is public and
   `Codable`.  A JSON in WeatherKit's own REST shape decodes into it with
   `JSONDecoder`.  The public REST API's response is documented by Apple
   (`/api/v1/weather/{language}/{latitude}/{longitude}`: `currentWeather`,
   `forecastHourly`, `forecastDaily`, `weatherAlerts`, and the metadata).  A
   small test tool should decode a hand-written response into `Weather` on the
   phone first: that proves the shape and the framework's availability without
   touching the app.
2. **The source.**  A service that needs no device identity: Open-Meteo
   (`https://api.open-meteo.com/v1/forecast`) has current conditions, hourly
   and daily values and needs no key.  Its JSON maps to WeatherKit's fields
   (temperature, apparent temperature, humidity, pressure, wind, condition
   code, precipitation, UV, sunrise/sunset, visibility).
3. **The hand-over.**  Where the app (or `weatherd`, if the app is made to use
   it) would fetch, the shim answers instead.  The injection mechanism exists:
   the kernel's `exec_inject` gives any program named in `root/etc/inject.conf`
   a dylib (`userland/build.sh`'s INJECT lines).  A dylib in the Weather app
   that interposes the data-service client, or the URL loading underneath it,
   can return the constructed `Weather`/JSON.  Which seam is reachable is the
   first thing to measure: the app's `WeatherDaemon` client is Swift, and
   `NSURLSession` may be old enough to interpose by class.

## Order of work

1. `weatherkit_test`: decode a sample WeatherKit-shaped JSON into `Weather` on
   the phone and print its fields (a scene like the others).
2. A host-side converter from an Open-Meteo response to that shape (Python,
   `tools/`), checked against the sample.
3. The app-side shim: the smallest hook that makes Weather render a
   constructed `Weather`; `NSURLSession` interception first, the
   `WeatherDaemon` client second.
4. The weather widget, if it uses the same client, comes with the app.

## What this is not

It is not Apple's forecast.  The numbers would be Open-Meteo's, shown by
Apple's app; the app's own "Data Sources" attribution should be checked and, if
it is Apple-only, said plainly in the CHECKLIST.  The FairPlay/SEP wall remains
for anything that truly needs Apple's service (App Store, media DRM), and that
is not what this shim is for.
