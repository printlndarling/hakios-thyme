/*
 * HakiOS Weather: the daily weather screen, without WeatherKit.
 *
 * The stock Weather app cannot fetch on this port: WeatherKit signs its token
 * request with Mescal/FairPlay, which needs the SEP, and the app stops inside
 * its own client before it asks weatherd for anything (CHECKLIST, "Weather:
 * the app's own fairplayd gate").  This application is the working screen:
 * CoreLocation gives the position the port already resolves, Open-Meteo
 * (no key, no device identity) gives the numbers, and the forecast is drawn
 * with UIKit alone -- no Metal, no WeatherKit.
 *
 * It is installed the way LocProbe is (a scene copies it to
 * /var/jb/Applications, uicache registers it), and records what it did in
 * /var/jb/hweather/weather.log as well as NSLog, because an application's
 * NSLog does not reach the run's console.
 */
#import <UIKit/UIKit.h>
#import <CoreLocation/CoreLocation.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void
hw_note(NSString *format, ...)
{
	va_list args;
	va_start(args, format);
	NSString *line = [[NSString alloc] initWithFormat:format arguments:args];
	FILE *log;
	time_t now;
	struct tm tm;
	char stamp[32];

	va_end(args);
	NSLog(@"HakiosWeather: %@", line);
	log = fopen("/var/jb/hweather/weather.log", "a");
	if (log == NULL) {
		NSString *caches = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES) firstObject];

		if (caches != nil) {
			log = fopen([caches stringByAppendingPathComponent:@"weather.log"].UTF8String, "a");
		}
	}
	if (log != NULL) {
		now = time(NULL);
		localtime_r(&now, &tm);
		strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
		fprintf(log, "%s %s\n", stamp, line.UTF8String);
		fclose(log);
	}
}

/* WMO 4677 weather code -> what the screen says (the short form the stock app
 * would use, in Chinese as the owner reads it). */
static NSString *
hw_condition(NSInteger code)
{
	switch (code) {
	case 0: return @"晴";
	case 1: return @"晴间多云";
	case 2: return @"多云";
	case 3: return @"阴";
	case 45: case 48: return @"雾";
	case 51: case 53: case 55: return @"毛毛雨";
	case 56: case 57: return @"冻毛毛雨";
	case 61: return @"小雨";
	case 63: return @"中雨";
	case 65: return @"大雨";
	case 66: case 67: return @"冻雨";
	case 71: return @"小雪";
	case 73: return @"雪";
	case 75: return @"大雪";
	case 77: return @"米雪";
	case 80: case 81: return @"阵雨";
	case 82: return @"强阵雨";
	case 85: case 86: return @"阵雪";
	case 95: return @"雷阵雨";
	case 96: case 99: return @"雷暴伴冰雹";
	default: return @"—";
	}
}

/* JSON numbers are NSNumbers; a null in the array is NSNull and would throw
 * on doubleValue.  -999 means "no value" and the row is skipped. */
static double
hw_value(id object, double fallback)
{
	return [object isKindOfClass:[NSNumber class]] ? [object doubleValue] : fallback;
}

/* Open-Meteo's hourly time is local wall time ("2026-11-16T17:00"): the hour
 * is all the strip shows. */
static NSString *
hw_hour(NSString *iso)
{
	if (iso.length >= 13 && [iso characterAtIndex:10] == 'T') {
		return [iso substringWithRange:NSMakeRange(11, 2)];
	}
	return @"--";
}

@interface HWAppDelegate : UIResponder <UIApplicationDelegate, CLLocationManagerDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) CLLocationManager *manager;
@property (nonatomic, strong) UILabel *cityLabel;
@property (nonatomic, strong) UILabel *temperatureLabel;
@property (nonatomic, strong) UILabel *conditionLabel;
@property (nonatomic, strong) UILabel *detailsLabel;
@property (nonatomic, strong) UILabel *hoursLabel;
@property (nonatomic, strong) UILabel *daysLabel;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, assign) BOOL fetched;
@property (nonatomic, assign) BOOL located;
@property (nonatomic, assign) BOOL fetching;
@property (nonatomic, assign) double lastLatitude;
@property (nonatomic, assign) double lastLongitude;
@property (nonatomic, assign) NSUInteger attempts;
@property (nonatomic, strong) NSTimer *fallback;
@end

@implementation HWAppDelegate

- (UILabel *)labelWithFont:(UIFont *)font color:(UIColor *)color
{
	UILabel *label = [[UILabel alloc] init];

	label.font = font;
	label.textColor = color;
	label.numberOfLines = 0;
	label.textAlignment = NSTextAlignmentCenter;
	return label;
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	UIViewController *controller = [[UIViewController alloc] init];
	UIButton *refresh = [UIButton buttonWithType:UIButtonTypeSystem];
	UIColor *dim = [UIColor colorWithWhite:0.85 alpha:1.0];

	controller.view.backgroundColor = [UIColor colorWithRed:0.16 green:0.42 blue:0.62 alpha:1.0];
	self.cityLabel = [self labelWithFont:[UIFont systemFontOfSize:30 weight:UIFontWeightSemibold] color:UIColor.whiteColor];
	self.temperatureLabel = [self labelWithFont:[UIFont systemFontOfSize:84 weight:UIFontWeightThin] color:UIColor.whiteColor];
	self.conditionLabel = [self labelWithFont:[UIFont systemFontOfSize:22 weight:UIFontWeightRegular] color:UIColor.whiteColor];
	self.detailsLabel = [self labelWithFont:[UIFont systemFontOfSize:15 weight:UIFontWeightRegular] color:dim];
	self.hoursLabel = [self labelWithFont:[UIFont systemFontOfSize:15 weight:UIFontWeightRegular] color:UIColor.whiteColor];
	self.daysLabel = [self labelWithFont:[UIFont systemFontOfSize:15 weight:UIFontWeightRegular] color:UIColor.whiteColor];
	self.statusLabel = [self labelWithFont:[UIFont systemFontOfSize:13 weight:UIFontWeightRegular] color:dim];

	self.cityLabel.text = @"正在定位…";
	self.temperatureLabel.text = @"--°";
	self.conditionLabel.text = @"";
	self.detailsLabel.text = @"";
	self.hoursLabel.text = @"";
	self.daysLabel.text = @"";
	self.statusLabel.text = @"";

	[refresh setTitle:@"刷新" forState:UIControlStateNormal];
	[refresh addTarget:self action:@selector(refreshTapped) forControlEvents:UIControlEventTouchUpInside];

	UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[
		self.cityLabel, self.temperatureLabel, self.conditionLabel, self.detailsLabel,
		self.hoursLabel, self.daysLabel, self.statusLabel, refresh,
	]];
	stack.axis = UILayoutConstraintAxisVertical;
	stack.spacing = 10;
	stack.alignment = UIStackViewAlignmentCenter;

	UIScrollView *scroll = [[UIScrollView alloc] initWithFrame:controller.view.bounds];
	stack.translatesAutoresizingMaskIntoConstraints = NO;
	[scroll addSubview:stack];
	[controller.view addSubview:scroll];
	scroll.translatesAutoresizingMaskIntoConstraints = NO;
	[NSLayoutConstraint activateConstraints:@[
		[scroll.topAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.topAnchor],
		[scroll.leadingAnchor constraintEqualToAnchor:controller.view.leadingAnchor],
		[scroll.trailingAnchor constraintEqualToAnchor:controller.view.trailingAnchor],
		[scroll.bottomAnchor constraintEqualToAnchor:controller.view.bottomAnchor],
		[stack.topAnchor constraintEqualToAnchor:scroll.topAnchor constant:24],
		[stack.bottomAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:-24],
		[stack.leadingAnchor constraintEqualToAnchor:scroll.leadingAnchor constant:20],
		[stack.trailingAnchor constraintEqualToAnchor:scroll.trailingAnchor constant:-20],
		[stack.widthAnchor constraintEqualToAnchor:scroll.widthAnchor constant:-40],
	]];

	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = controller;
	[self.window makeKeyAndVisible];

	hw_note(@"launched: services %d, authorization %ld", (int)[CLLocationManager locationServicesEnabled],
	    (long)[CLLocationManager authorizationStatus]);

	self.manager = [[CLLocationManager alloc] init];
	self.manager.delegate = self;
	if ([self.manager respondsToSelector:@selector(requestWhenInUseAuthorization)]) {
		[self.manager requestWhenInUseAuthorization];
	}
	[self.manager startUpdatingLocation];
	/* A fix, or eight seconds: the screen must show weather either way. */
	self.fallback = [NSTimer scheduledTimerWithTimeInterval:30.0 target:self selector:@selector(fallbackFired)
	    userInfo:nil repeats:NO];
	return YES;
}

- (void)refreshTapped
{
	self.attempts = 0;
	self.manager.delegate = self;
	[self.manager startUpdatingLocation];
	[self.fallback invalidate];
	self.fallback = [NSTimer scheduledTimerWithTimeInterval:30.0 target:self selector:@selector(fallbackFired)
	    userInfo:nil repeats:NO];
	/* A manual refusal of the fallback fetch is not a reason to stop; the
	 * fetch itself starts again when a fix or the fallback timer comes. */
	if (self.located) {
		[self fetchForLatitude:self.lastLatitude longitude:self.lastLongitude];
	}
}

/* The link may not be up when the application starts (the scene joins the
 * network beside it), so a failed fetch is tried again until one succeeds:
 * fifteen seconds apart, eight times. */
- (void)retryFired
{
	if ([UIApplication sharedApplication].applicationState != UIApplicationStateActive) {
		return;
	}
	hw_note(@"retrying (%lu of 8)", (unsigned long)self.attempts + 1);
	[self fetchForLatitude:self.lastLatitude longitude:self.lastLongitude];
}

- (void)fallbackFired
{
	if (self.fetched || self.located) {
		return;
	}
	/* Thirty seconds, not eight: a cold locationd on this port takes tens of
	 * seconds (the stock app's own screen waited that long for "Chang'an,
	 * Xi'an"), and a wrong-district fallback is worse than a short wait. */
	hw_note(@"no fix in thirty seconds: fetching for Xi'an");
	self.cityLabel.text = @"西安市";
	[self fetchForLatitude:34.34 longitude:108.94];
}

- (void)locationManager:(CLLocationManager *)manager didUpdateLocations:(NSArray<CLLocation *> *)locations
{
	CLLocation *location = locations.lastObject;
	BOOL fromFallback;

	if (location == nil || location.horizontalAccuracy < 0) {
		return;
	}
	if (self.located && location.horizontalAccuracy > 100) {
		/* a coarse update after a good fix is not worth a second answer */
		return;
	}
	fromFallback = self.fetched && self.lastLatitude == 34.34 && self.lastLongitude == 108.94;
	self.located = YES;
	[manager stopUpdatingLocation];
	hw_note(@"fix: %.4f, %.4f (+/- %.0f m)%@", location.coordinate.latitude, location.coordinate.longitude,
	    location.horizontalAccuracy, fromFallback ? @", replacing the fallback" : @"");
	[[[CLGeocoder alloc] init] reverseGeocodeLocation:location completionHandler:^(NSArray<CLPlacemark *> *places,
	    NSError *error) {
		CLPlacemark *place = places.firstObject;

		dispatch_async(dispatch_get_main_queue(), ^{
			if (place != nil) {
				/* the district first: "长安区, 西安市" is the
				 * precision the stock app showed, not "西安" */
				NSString *district = place.subLocality ?: place.subAdministrativeArea;
				NSString *city = place.locality ?: place.administrativeArea ?: place.name;

				if (district.length > 0 && city.length > 0 && ![district isEqualToString:city]) {
					self.cityLabel.text = [NSString stringWithFormat:@"%@, %@", district, city];
				} else {
					self.cityLabel.text = city.length > 0 ? city : (district.length > 0 ? district : @"当前位置");
				}
			} else {
				self.cityLabel.text = @"当前位置";
			}
			hw_note(@"place: %@ (error %@)", self.cityLabel.text, error.localizedDescription);
		});
	}];
	if (fromFallback) {
		/* the fallback's answer, if it landed, is for the wrong point: let
		 * the real fix fetch (a result from the old coordinates is dropped
		 * in the completion by the staleness check) */
		self.fetching = NO;
		self.attempts = 0;
	}
	[self fetchForLatitude:location.coordinate.latitude longitude:location.coordinate.longitude];
}

- (void)locationManager:(CLLocationManager *)manager didFailWithError:(NSError *)error
{
	hw_note(@"location failed: %ld %@", (long)error.code, error.localizedDescription);
	[self fallbackFired];
}

/* Open-Meteo, no key: current conditions, the next hours, seven days. */
- (void)fetchForLatitude:(double)latitude longitude:(double)longitude
{
	NSString *url = [NSString stringWithFormat:@"https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
	    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
	    "&hourly=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min"
	    "&timezone=auto&forecast_days=7", latitude, longitude];
	NSURLRequest *request = [NSURLRequest requestWithURL:[NSURL URLWithString:url]
	    cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:20];
	NSURLSessionDataTask *task;

	if (self.fetching) {
		return;
	}
	self.fetching = YES;
	self.lastLatitude = latitude;
	self.lastLongitude = longitude;
	self.attempts += 1;
	self.statusLabel.text = @"正在获取天气…";
	hw_note(@"fetch %lu: %.4f, %.4f", (unsigned long)self.attempts, latitude, longitude);
	task = [[NSURLSession sharedSession] dataTaskWithRequest:request completionHandler:^(NSData *data,
	    NSURLResponse *response, NSError *error) {
		NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ?
		    (NSInteger)[(NSHTTPURLResponse *)response statusCode] : -1;
		BOOL again = self.attempts < 8;
		BOOL current;

		/* A fallback answer that arrives after a real fix must not draw
		 * (or clear the live request's flag). */
		current = latitude == self.lastLatitude && longitude == self.lastLongitude;
		if (!current) {
			hw_note(@"fetch %lu for %.2f,%.2f dropped: the position changed",
			    (unsigned long)self.attempts, latitude, longitude);
			return;
		}
		self.fetching = NO;
		if (data == nil || error != nil) {
			hw_note(@"fetch %lu failed: status %ld, %@%@", (unsigned long)self.attempts, (long)status,
			    error.localizedDescription, again ? @", will retry" : @"");
			dispatch_async(dispatch_get_main_queue(), ^{
				self.statusLabel.text = again ? @"正在重试（等待网络）…" :
				    [NSString stringWithFormat:@"获取失败：%@", error.localizedDescription ?: @"无数据"];
			});
			if (again) {
				[NSTimer scheduledTimerWithTimeInterval:15.0 target:self selector:@selector(retryFired)
				    userInfo:nil repeats:NO];
			}
			return;
		}
		self.fetched = YES;
		hw_note(@"fetch %lu: status %ld, %lu bytes", (unsigned long)self.attempts, (long)status,
		    (unsigned long)data.length);
		[self renderData:data];
	}];
	[task resume];
}

- (void)renderData:(NSData *)data
{
	NSDictionary *root = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
	NSDictionary *current;
	NSDictionary *hourly;
	NSDictionary *daily;
	NSArray *hours;
	NSArray *days;
	NSMutableArray<NSString *> *hourLines = [NSMutableArray array];
	NSMutableArray<NSString *> *dayLines = [NSMutableArray array];

	if (![root isKindOfClass:[NSDictionary class]]) {
		dispatch_async(dispatch_get_main_queue(), ^{ self.statusLabel.text = @"数据无法解析"; });
		return;
	}
	current = root[@"current"];
	hourly = root[@"hourly"];
	daily = root[@"daily"];
	if (![current isKindOfClass:[NSDictionary class]]) {
		dispatch_async(dispatch_get_main_queue(), ^{ self.statusLabel.text = @"数据缺少当前天气"; });
		return;
	}
	hours = [hourly[@"time"] isKindOfClass:[NSArray class]] ? hourly[@"time"] : @[];
	days = [daily[@"time"] isKindOfClass:[NSArray class]] ? daily[@"time"] : @[];

	/* The next twelve hours from now: Open-Meteo's hourly array starts at
	 * local midnight of today. */
	NSArray *hourTemps = [hourly[@"temperature_2m"] isKindOfClass:[NSArray class]] ? hourly[@"temperature_2m"] : @[];
	NSArray *hourCodes = [hourly[@"weather_code"] isKindOfClass:[NSArray class]] ? hourly[@"weather_code"] : @[];
	NSString *nowHour = nil;
	{
		NSDateFormatter *formatter = [[NSDateFormatter alloc] init];

		formatter.dateFormat = @"yyyy-MM-dd'T'HH";
		nowHour = [formatter stringFromDate:[NSDate date]];
	}
	NSUInteger start = 0;
	for (NSUInteger i = 0; i < hours.count; i++) {
		if ([hours[i] isKindOfClass:[NSString class]] && [hours[i] hasPrefix:nowHour]) {
			start = i;
			break;
		}
	}
	for (NSUInteger i = start; i < hours.count && hourLines.count < 12; i++) {
		if (i >= hourTemps.count || i >= hourCodes.count) {
			break;
		}
		{
			double temperature = hw_value(hourTemps[i], -999);
			double code = hw_value(hourCodes[i], -999);

			if (temperature < -100 || code < 0) {
				continue;
			}
			[hourLines addObject:[NSString stringWithFormat:@"%@时 %.0f° %@", hw_hour(hours[i]),
			    temperature, hw_condition((NSInteger)code)]];
		}
	}

	NSArray *dayCodes = [daily[@"weather_code"] isKindOfClass:[NSArray class]] ? daily[@"weather_code"] : @[];
	NSArray *dayMax = [daily[@"temperature_2m_max"] isKindOfClass:[NSArray class]] ? daily[@"temperature_2m_max"] : @[];
	NSArray *dayMin = [daily[@"temperature_2m_min"] isKindOfClass:[NSArray class]] ? daily[@"temperature_2m_min"] : @[];
	NSArray *dayNames = @[@"周日", @"周一", @"周二", @"周三", @"周四", @"周五", @"周六"];
	for (NSUInteger i = 0; i < days.count && i < 7; i++) {
		NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
		NSDate *date;
		NSString *name;

		if (i >= dayCodes.count || i >= dayMax.count || i >= dayMin.count) {
			break;
		}
		formatter.dateFormat = @"yyyy-MM-dd";
		date = [formatter dateFromString:days[i]];
		name = date != nil ? dayNames[MAX(0, [[NSCalendar currentCalendar] component:NSCalendarUnitWeekday
		    fromDate:date] - 1)] : days[i];
		{
			double code = hw_value(dayCodes[i], -999);
			double high = hw_value(dayMax[i], -999);
			double low = hw_value(dayMin[i], -999);

			if (code < 0 || high < -100 || low < -100) {
				continue;
			}
			[dayLines addObject:[NSString stringWithFormat:@"%@ %@ %.0f° / %.0f°", name,
			    hw_condition((NSInteger)code), high, low]];
		}
	}

	dispatch_async(dispatch_get_main_queue(), ^{
		NSNumber *temperature = current[@"temperature_2m"];
		NSNumber *feels = current[@"apparent_temperature"];
		NSNumber *humidity = current[@"relative_humidity_2m"];
		NSNumber *wind = current[@"wind_speed_10m"];
		NSNumber *code = current[@"weather_code"];

		if ([temperature isKindOfClass:[NSNumber class]]) {
			self.temperatureLabel.text = [NSString stringWithFormat:@"%.0f°", temperature.doubleValue];
		}
		if ([code isKindOfClass:[NSNumber class]]) {
			self.conditionLabel.text = hw_condition(code.integerValue);
		}
		self.detailsLabel.text = [NSString stringWithFormat:@"体感 %.0f° · 湿度 %.0f%% · 风速 %.0f km/h",
		    hw_value(feels, 0), hw_value(humidity, 0), hw_value(wind, 0)];
		self.hoursLabel.text = [@"未来 12 小时\n" stringByAppendingString:[hourLines componentsJoinedByString:@"\n"]];
		self.daysLabel.text = [@"未来 7 天\n" stringByAppendingString:[dayLines componentsJoinedByString:@"\n"]];
		self.statusLabel.text = @"数据：Open-Meteo";
	});
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([HWAppDelegate class]));
	}
}
