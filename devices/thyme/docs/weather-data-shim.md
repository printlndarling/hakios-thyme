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
   `forecastHourly`, `forecastDaily`, `weatherAlerts`, and the metadata).
   There is no Swift compiler in this toolchain (`theos/toolchain/linux/
   iphone` is clang 11 only), so the decode is not ours to test in isolation:
   the app's own Swift code does it, and the shim only has to hand it the
   JSON.  The first measurement is therefore the request the app makes and the
   shape it parses, not a standalone decoder.
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

1. An injected dylib in the Weather app that logs every `NSURLSession`/
   `NSURLConnection` request it makes: the host and path the weather client
   uses, and what it sends (a JWT header or none).  That says whether the seam
   is reachable and what the server side of the shim must look like.
2. The shim's answer: a sample WeatherKit-shaped JSON for that request, built
   by hand from the public schema, so the app either renders it or its parse
   error names the field it wanted.  Iterate on the shape from the app's own
   complaints -- no Swift compiler here, the app is the decoder.
3. A host-side converter from an Open-Meteo response to that shape (Python,
   `tools/`), checked against the sample.
4. The weather widget, if it uses the same client, comes with the app.

## What this is not

It is not Apple's forecast.  The numbers would be Open-Meteo's, shown by
Apple's app; the app's own "Data Sources" attribution should be checked and, if
it is Apple-only, said plainly in the CHECKLIST.  The FairPlay/SEP wall remains
for anything that truly needs Apple's service (App Store, media DRM), and that
is not what this shim is for.
