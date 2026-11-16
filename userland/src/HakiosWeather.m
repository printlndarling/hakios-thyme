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

/* The same code as an SF Symbol, the way the stock app draws the sky. */
static NSString *
hw_symbol(NSInteger code)
{
	switch (code) {
	case 0: return @"sun.max.fill";
	case 1: return @"sun.min.fill";
	case 2: return @"cloud.sun.fill";
	case 3: return @"cloud.fill";
	case 45: case 48: return @"cloud.fog.fill";
	case 51: case 53: case 55: case 56: case 57: return @"cloud.drizzle.fill";
	case 61: case 63: case 80: case 81: return @"cloud.rain.fill";
	case 65: case 82: return @"cloud.heavyrain.fill";
	case 66: case 67: return @"cloud.sleet.fill";
	case 71: case 73: case 75: case 77: case 85: case 86: return @"cloud.snow.fill";
	case 95: return @"cloud.bolt.rain.fill";
	case 96: case 99: return @"cloud.bolt.fill";
	default: return @"cloud.fill";
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

static UIColor *hw_white(CGFloat alpha) { return [UIColor colorWithWhite:1.0 alpha:alpha]; }

/* The sky the screen stands on. */
@interface HWBackgroundView : UIView
@end
@implementation HWBackgroundView
+ (Class)layerClass { return [CAGradientLayer class]; }
@end

@interface HWRootController : UIViewController
@property (nonatomic, strong) UILabel *cityLabel;
@property (nonatomic, strong) UILabel *temperatureLabel;
@property (nonatomic, strong) UIImageView *conditionIcon;
@property (nonatomic, strong) UILabel *conditionLabel;
@property (nonatomic, strong) UILabel *detailsLabel;
@property (nonatomic, strong) UIStackView *hourlyStack;
@property (nonatomic, strong) UIStackView *dailyStack;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UIButton *refreshButton;
@end

@implementation HWRootController

- (UILabel *)label:(UIFont *)font color:(UIColor *)color
{
	UILabel *label = [[UILabel alloc] init];

	label.font = font;
	label.textColor = color;
	label.numberOfLines = 0;
	label.textAlignment = NSTextAlignmentLeft;
	return label;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	[self buildInterface];
}

- (void)buildInterface
{
	UIScrollView *scroll = [[UIScrollView alloc] init];
	UIStackView *content;
	UIStackView *header;
	UIStackView *conditionRow;
	UIScrollView *hourlyScroll;
	UILabel *hoursTitle = [self label:[UIFont systemFontOfSize:15 weight:UIFontWeightSemibold] color:hw_white(0.9)];
	UILabel *daysTitle = [self label:[UIFont systemFontOfSize:15 weight:UIFontWeightSemibold] color:hw_white(0.9)];

	{
		HWBackgroundView *background = [[HWBackgroundView alloc] initWithFrame:self.view.bounds];

		background.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
		((CAGradientLayer *)background.layer).colors = @[
			(id)[UIColor colorWithRed:0.20 green:0.47 blue:0.70 alpha:1.0].CGColor,
			(id)[UIColor colorWithRed:0.10 green:0.29 blue:0.48 alpha:1.0].CGColor,
		];
		((CAGradientLayer *)background.layer).startPoint = CGPointMake(0.5, 0.0);
		((CAGradientLayer *)background.layer).endPoint = CGPointMake(0.5, 1.0);
		[self.view addSubview:background];
	}

	self.cityLabel = [self label:[UIFont systemFontOfSize:30 weight:UIFontWeightSemibold] color:UIColor.whiteColor];
	self.cityLabel.text = @"正在定位…";
	self.temperatureLabel = [self label:[UIFont systemFontOfSize:92 weight:UIFontWeightThin] color:UIColor.whiteColor];
	self.temperatureLabel.text = @"--°";
	self.conditionIcon = [[UIImageView alloc] init];
	self.conditionIcon.contentMode = UIViewContentModeScaleAspectFit;
	self.conditionIcon.tintColor = UIColor.whiteColor;
	[self.conditionIcon setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
	[self.conditionIcon.widthAnchor constraintEqualToConstant:24].active = YES;
	[self.conditionIcon.heightAnchor constraintEqualToConstant:24].active = YES;
	self.conditionLabel = [self label:[UIFont systemFontOfSize:21 weight:UIFontWeightRegular] color:UIColor.whiteColor];
	self.detailsLabel = [self label:[UIFont systemFontOfSize:14 weight:UIFontWeightRegular] color:hw_white(0.85)];

	conditionRow = [[UIStackView alloc] initWithArrangedSubviews:@[self.conditionIcon, self.conditionLabel]];
	conditionRow.axis = UILayoutConstraintAxisHorizontal;
	conditionRow.spacing = 8;
	conditionRow.alignment = UIStackViewAlignmentCenter;

	header = [[UIStackView alloc] initWithArrangedSubviews:@[
		self.cityLabel, self.temperatureLabel, conditionRow, self.detailsLabel,
	]];
	header.axis = UILayoutConstraintAxisVertical;
	header.alignment = UIStackViewAlignmentLeading;
	header.spacing = 6;

	hoursTitle.text = @"未来 12 小时";
	self.hourlyStack = [[UIStackView alloc] init];
	self.hourlyStack.axis = UILayoutConstraintAxisHorizontal;
	self.hourlyStack.spacing = 14;
	self.hourlyStack.alignment = UIStackViewAlignmentTop;

	hourlyScroll = [[UIScrollView alloc] init];
	hourlyScroll.showsHorizontalScrollIndicator = NO;
	[self.hourlyStack.heightAnchor constraintEqualToConstant:84].active = YES;
	[hourlyScroll addSubview:self.hourlyStack];
	self.hourlyStack.translatesAutoresizingMaskIntoConstraints = NO;
	[NSLayoutConstraint activateConstraints:@[
		[self.hourlyStack.topAnchor constraintEqualToAnchor:hourlyScroll.contentLayoutGuide.topAnchor],
		[self.hourlyStack.bottomAnchor constraintEqualToAnchor:hourlyScroll.contentLayoutGuide.bottomAnchor],
		[self.hourlyStack.leadingAnchor constraintEqualToAnchor:hourlyScroll.contentLayoutGuide.leadingAnchor],
		[self.hourlyStack.trailingAnchor constraintEqualToAnchor:hourlyScroll.contentLayoutGuide.trailingAnchor],
		[self.hourlyStack.heightAnchor constraintEqualToAnchor:hourlyScroll.frameLayoutGuide.heightAnchor],
	]];

	daysTitle.text = @"未来几天";
	self.dailyStack = [[UIStackView alloc] init];
	self.dailyStack.axis = UILayoutConstraintAxisVertical;
	self.dailyStack.spacing = 14;

	self.statusLabel = [self label:[UIFont systemFontOfSize:12 weight:UIFontWeightRegular] color:hw_white(0.7)];
	self.refreshButton = [UIButton buttonWithType:UIButtonTypeSystem];
	[self.refreshButton setTitle:@"刷新" forState:UIControlStateNormal];
	[self.refreshButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
	self.refreshButton.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
	self.refreshButton.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;

	content = [[UIStackView alloc] initWithArrangedSubviews:@[
		header, hoursTitle, hourlyScroll, daysTitle, self.dailyStack, self.statusLabel, self.refreshButton,
	]];
	content.axis = UILayoutConstraintAxisVertical;
	content.spacing = 20;
	content.alignment = UIStackViewAlignmentFill;

	scroll.alwaysBounceVertical = YES;
	[scroll addSubview:content];
	[self.view addSubview:scroll];
	scroll.translatesAutoresizingMaskIntoConstraints = NO;
	content.translatesAutoresizingMaskIntoConstraints = NO;
	[NSLayoutConstraint activateConstraints:@[
		[scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
		[scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
		[content.topAnchor constraintEqualToAnchor:scroll.topAnchor constant:18],
		[content.bottomAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:-18],
		[content.leadingAnchor constraintEqualToAnchor:scroll.leadingAnchor constant:22],
		[content.trailingAnchor constraintEqualToAnchor:scroll.trailingAnchor constant:-22],
		[content.widthAnchor constraintEqualToAnchor:scroll.widthAnchor constant:-44],
	]];

	self.view.backgroundColor = UIColor.clearColor;
}

- (UIView *)hourCellWithHour:(NSString *)hour symbol:(NSString *)symbol temperature:(double)temperature
{
	UILabel *hourLabel = [self label:[UIFont systemFontOfSize:12 weight:UIFontWeightMedium] color:hw_white(0.8)];
	UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol]];
	UILabel *temperatureLabel = [self label:[UIFont monospacedDigitSystemFontOfSize:16 weight:UIFontWeightSemibold]
	    color:UIColor.whiteColor];
	UIStackView *cell = [[UIStackView alloc] initWithArrangedSubviews:@[hourLabel, icon, temperatureLabel]];

	hourLabel.text = [hour stringByAppendingString:@"时"];
	hourLabel.textAlignment = NSTextAlignmentCenter;
	temperatureLabel.text = [NSString stringWithFormat:@"%.0f°", temperature];
	temperatureLabel.textAlignment = NSTextAlignmentCenter;
	icon.tintColor = UIColor.whiteColor;
	icon.contentMode = UIViewContentModeScaleAspectFit;
	[icon.widthAnchor constraintEqualToConstant:22].active = YES;
	[icon.heightAnchor constraintEqualToConstant:22].active = YES;
	cell.axis = UILayoutConstraintAxisVertical;
	cell.alignment = UIStackViewAlignmentCenter;
	cell.spacing = 6;
	[cell.widthAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
	return cell;
}

- (UIView *)dayRowWithName:(NSString *)name symbol:(NSString *)symbol condition:(NSString *)condition
    high:(double)high low:(double)low
{
	UILabel *dayLabel = [self label:[UIFont systemFontOfSize:16 weight:UIFontWeightSemibold] color:UIColor.whiteColor];
	UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol]];
	UILabel *conditionLabel = [self label:[UIFont systemFontOfSize:15 weight:UIFontWeightRegular] color:hw_white(0.85)];
	UILabel *temperatureLabel = [self label:[UIFont monospacedDigitSystemFontOfSize:16 weight:UIFontWeightMedium]
	    color:UIColor.whiteColor];
	UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[dayLabel, icon, conditionLabel, temperatureLabel]];

	dayLabel.text = name;
	[dayLabel.widthAnchor constraintEqualToConstant:52].active = YES;
	conditionLabel.text = condition;
	icon.tintColor = UIColor.whiteColor;
	icon.contentMode = UIViewContentModeScaleAspectFit;
	[icon.widthAnchor constraintEqualToConstant:22].active = YES;
	[icon.heightAnchor constraintEqualToConstant:22].active = YES;
	temperatureLabel.text = [NSString stringWithFormat:@"%.0f° / %.0f°", high, low];
	temperatureLabel.textAlignment = NSTextAlignmentRight;
	[row setCustomSpacing:10 afterView:dayLabel];
	[row setCustomSpacing:8 afterView:icon];
	row.axis = UILayoutConstraintAxisHorizontal;
	row.alignment = UIStackViewAlignmentCenter;
	return row;
}

- (void)showCity:(NSString *)city
{
	dispatch_async(dispatch_get_main_queue(), ^{ self.cityLabel.text = city; });
}

- (void)showCurrentTemperature:(double)temperature condition:(NSString *)condition symbol:(NSString *)symbol
    details:(NSString *)details
{
	dispatch_async(dispatch_get_main_queue(), ^{
		self.temperatureLabel.text = [NSString stringWithFormat:@"%.0f°", temperature];
		self.conditionLabel.text = condition;
		self.conditionIcon.image = [UIImage systemImageNamed:symbol];
		self.detailsLabel.text = details;
	});
}

- (void)showHours:(NSArray<NSDictionary *> *)hours
{
	dispatch_async(dispatch_get_main_queue(), ^{
		for (UIView *view in self.hourlyStack.arrangedSubviews) {
			[self.hourlyStack removeArrangedSubview:view];
			[view removeFromSuperview];
		}
		for (NSDictionary *hour in hours) {
			[self.hourlyStack addArrangedSubview:[self hourCellWithHour:hour[@"hour"] symbol:hour[@"symbol"]
			    temperature:[hour[@"temperature"] doubleValue]]];
		}
	});
}

- (void)showDays:(NSArray<NSDictionary *> *)days
{
	dispatch_async(dispatch_get_main_queue(), ^{
		for (UIView *view in self.dailyStack.arrangedSubviews) {
			[self.dailyStack removeArrangedSubview:view];
			[view removeFromSuperview];
		}
		for (NSDictionary *day in days) {
			[self.dailyStack addArrangedSubview:[self dayRowWithName:day[@"name"] symbol:day[@"symbol"]
			    condition:day[@"condition"] high:[day[@"high"] doubleValue] low:[day[@"low"] doubleValue]]];
		}
	});
}

- (void)showStatus:(NSString *)status
{
	dispatch_async(dispatch_get_main_queue(), ^{ self.statusLabel.text = status; });
}

@end

@interface HWAppDelegate : UIResponder <UIApplicationDelegate, CLLocationManagerDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) HWRootController *root;
@property (nonatomic, strong) CLLocationManager *manager;
@property (nonatomic, assign) BOOL fetched;
@property (nonatomic, assign) BOOL located;
@property (nonatomic, assign) BOOL fetching;
@property (nonatomic, assign) double lastLatitude;
@property (nonatomic, assign) double lastLongitude;
@property (nonatomic, assign) NSUInteger attempts;
@property (nonatomic, assign) BOOL fallbackPending;
@property (nonatomic, assign) BOOL wttrTried;
@property (nonatomic, strong) CLLocation *fixLocation;
@property (nonatomic, assign) BOOL geocodeRetried;
@property (nonatomic, strong) NSTimer *fallback;
@end

@implementation HWAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	self.root = [[HWRootController alloc] init];
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = self.root;
	[self.window makeKeyAndVisible];
	[self.root.refreshButton addTarget:self action:@selector(refreshTapped) forControlEvents:UIControlEventTouchUpInside];

	hw_note(@"launched: services %d, authorization %ld", (int)[CLLocationManager locationServicesEnabled],
	    (long)[CLLocationManager authorizationStatus]);

	self.manager = [[CLLocationManager alloc] init];
	self.manager.delegate = self;
	if ([self.manager respondsToSelector:@selector(requestWhenInUseAuthorization)]) {
		[self.manager requestWhenInUseAuthorization];
	}
	[self.manager startUpdatingLocation];
	/* A fix, or thirty seconds: the screen must show weather either way. */
	self.fallback = [NSTimer scheduledTimerWithTimeInterval:30.0 target:self selector:@selector(fallbackFired)
	    userInfo:nil repeats:NO];
	return YES;
}

- (void)refreshTapped
{
	[self.root showStatus:@"正在刷新…"];
	self.attempts = 0;
	self.manager.delegate = self;
	[self.manager startUpdatingLocation];
	[self.fallback invalidate];
	self.fallback = [NSTimer scheduledTimerWithTimeInterval:30.0 target:self selector:@selector(fallbackFired)
	    userInfo:nil repeats:NO];
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
	hw_note(@"no fix in thirty seconds: fetching for Xi'an");
	self.fallbackPending = YES;
	[self.root showCity:@"西安市"];
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
	fromFallback = self.fallbackPending;
	self.fallbackPending = NO;
	self.located = YES;
	[manager stopUpdatingLocation];
	hw_note(@"fix: %.4f, %.4f (+/- %.0f m)%@", location.coordinate.latitude, location.coordinate.longitude,
	    location.horizontalAccuracy, fromFallback ? @", replacing the fallback" : @"");
	self.fixLocation = location;
	[self reverseGeocode:location];
	if (fromFallback) {
		/* the fallback's answer, if it landed, is for the wrong point: let
		 * the real fix fetch (a result from the old coordinates is dropped
		 * in the completion by the staleness check) */
		self.fetching = NO;
		self.attempts = 0;
		self.wttrTried = NO;
	}
	[self fetchForLatitude:location.coordinate.latitude longitude:location.coordinate.longitude];
}

- (void)reverseGeocode:(CLLocation *)location
{
	[[[CLGeocoder alloc] init] reverseGeocodeLocation:location completionHandler:^(NSArray<CLPlacemark *> *places,
	    NSError *error) {
		CLPlacemark *place = places.firstObject;

		if (place != nil) {
			/* the district first: "长安区, 西安市" is the precision the
			 * stock app showed, not "西安" */
			NSString *district = place.subLocality ?: place.subAdministrativeArea;
			NSString *city = place.locality ?: place.administrativeArea ?: place.name;
			NSString *name;

			if (district.length > 0 && city.length > 0 && ![district isEqualToString:city]) {
				name = [NSString stringWithFormat:@"%@, %@", district, city];
			} else {
				name = city.length > 0 ? city : (district.length > 0 ? district : @"当前位置");
			}
			hw_note(@"place: %@ (error %@)", name, error.localizedDescription);
			[self.root showCity:name];
			return;
		}
		hw_note(@"place: no placemark (error %@)", error.localizedDescription);
		/* the geocoder asks Apple's service; at launch the link may not be up
		 * yet and it fails at once -- ask again ten seconds later, once */
		if (!self.geocodeRetried) {
			self.geocodeRetried = YES;
			dispatch_async(dispatch_get_main_queue(), ^{
				[NSTimer scheduledTimerWithTimeInterval:10.0 target:self
				    selector:@selector(geocodeAgain) userInfo:nil repeats:NO];
			});
		} else {
			[self.root showCity:@"当前位置"];
		}
	}];
}

- (void)geocodeAgain
{
	if (self.fixLocation != nil) {
		[self reverseGeocode:self.fixLocation];
	}
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
	[self.root showStatus:@"正在获取天气…"];
	hw_note(@"fetch %lu: %.4f, %.4f", (unsigned long)self.attempts, latitude, longitude);
	task = [[NSURLSession sharedSession] dataTaskWithRequest:request completionHandler:^(NSData *data,
	    NSURLResponse *response, NSError *error) {
		NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ?
		    (NSInteger)[(NSHTTPURLResponse *)response statusCode] : -1;
		BOOL again = self.attempts < 20;
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
			hw_note(@"fetch %lu failed: status %ld, domain %@ code %ld%@", (unsigned long)self.attempts,
			    (long)status, error.domain, (long)error.code, again ? @", will retry" : @"");
			[self.root showStatus:again ? @"正在重试（等待网络）…" :
			    [NSString stringWithFormat:@"获取失败：%@", error.localizedDescription ?: @"无数据"]];
			if (!self.wttrTried) {
				/* the port's TLS to Open-Meteo stalls sometimes (-1001);
				 * wttr.in answers over plain HTTP, and ATS allows it */
				self.wttrTried = YES;
				[self fetchWttrForLatitude:latitude longitude:longitude];
				return;
			}
			if (again) {
				/* the completion runs on a URLSession queue: a timer made
				 * there is added to that thread's run loop, which never runs
				 * (the screen sat at "正在重试" for a whole run, 19:52) */
				dispatch_async(dispatch_get_main_queue(), ^{
					[NSTimer scheduledTimerWithTimeInterval:15.0 target:self selector:@selector(retryFired)
					    userInfo:nil repeats:NO];
				});
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


/* WWO's weather codes (wttr.in's) as the WMO numbers the rest of the app
 * speaks, so the condition text and the SF Symbol come from one place. */
static NSInteger
hw_wmo_from_wwo(NSInteger wwo)
{
	switch (wwo) {
	case 113: return 0;
	case 116: return 2;
	case 119: return 3;
	case 122: return 3;
	case 143: return 45;
	case 248: case 260: return 45;
	case 176: case 263: case 266: case 293: case 296: return 61;
	case 299: case 302: return 63;
	case 305: case 308: return 65;
	case 353: return 80;
	case 356: case 359: return 82;
	case 200: case 386: case 389: return 95;
	case 392: case 395: return 95;
	case 227: case 230: return 75;
	case 281: case 284: return 56;
	case 311: case 314: return 66;
	case 317: case 320: case 323: case 326: return 71;
	case 329: case 332: return 73;
	case 335: case 338: return 75;
	case 350: case 374: case 377: return 66;
	case 362: case 365: return 71;
	case 368: case 371: return 73;
	default: return 3;
	}
}

/* wttr.in over plain HTTP: the fallback when the port's TLS to Open-Meteo
 * stalls (NSURLErrorTimedOut -1001, logs/203031-run.log).  Its j1 JSON has
 * current conditions, today and tomorrow hour by hour, and three days. */
- (void)fetchWttrForLatitude:(double)latitude longitude:(double)longitude
{
	NSString *url = [NSString stringWithFormat:@"http://wttr.in/%.2f,%.2f?format=j1", latitude, longitude];
	NSURLRequest *request = [NSURLRequest requestWithURL:[NSURL URLWithString:url]
	    cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:15];
	NSURLSessionDataTask *task;

	if (self.fetching) {
		return;
	}
	self.fetching = YES;
	hw_note(@"fetch (wttr.in): %.4f, %.4f", latitude, longitude);
	task = [[NSURLSession sharedSession] dataTaskWithRequest:request completionHandler:^(NSData *data,
	    NSURLResponse *response, NSError *error) {
		NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ?
		    (NSInteger)[(NSHTTPURLResponse *)response statusCode] : -1;
		BOOL current = latitude == self.lastLatitude && longitude == self.lastLongitude;

		if (!current) {
			return;
		}
		self.fetching = NO;
		if (data == nil || error != nil) {
			hw_note(@"fetch (wttr.in) failed: status %ld, domain %@ code %ld", (long)status,
			    error.domain, (long)error.code);
			[self.root showStatus:@"获取失败（网络不可用）"];
			return;
		}
		hw_note(@"fetch (wttr.in): status %ld, %lu bytes", (long)status, (unsigned long)data.length);
		[self renderWttrData:data];
	}];
	[task resume];
}

- (void)renderWttrData:(NSData *)data
{
	NSDictionary *root = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
	NSDictionary *now;
	NSArray *days;
	NSMutableArray<NSDictionary *> *hourCells = [NSMutableArray array];
	NSMutableArray<NSDictionary *> *dayRows = [NSMutableArray array];
	NSArray *dayNames = @[@"周日", @"周一", @"周二", @"周三", @"周四", @"周五", @"周六"];

	if (![root isKindOfClass:[NSDictionary class]]) {
		[self.root showStatus:@"数据无法解析"];
		return;
	}
	now = [root[@"current_condition"] isKindOfClass:[NSArray class]] ?
	    [root[@"current_condition"] firstObject] : nil;
	days = [root[@"weather"] isKindOfClass:[NSArray class]] ? root[@"weather"] : @[];
	if (![now isKindOfClass:[NSDictionary class]] || days.count == 0) {
		[self.root showStatus:@"数据缺少当前天气"];
		return;
	}

	/* the hours: today's from the current one, then tomorrow's */
	for (NSUInteger d = 0; d < days.count && hourCells.count < 12; d++) {
		NSArray *hourly = [days[d][@"hourly"] isKindOfClass:[NSArray class]] ? days[d][@"hourly"] : @[];

		for (NSUInteger i = 0; i < hourly.count && hourCells.count < 12; i++) {
			NSDictionary *hour = hourly[i];
			NSInteger code = hw_wmo_from_wwo([hour[@"weatherCode"] integerValue]);
			double temperature = hw_value(hour[@"tempC"], -999);
			NSInteger clock = [hour[@"time"] integerValue] / 100;

			if (temperature < -100) {
				continue;
			}
			[hourCells addObject:@{
				@"hour": [NSString stringWithFormat:@"%02ld", (long)clock],
				@"temperature": @(temperature),
				@"symbol": hw_symbol(code),
			}];
		}
	}

	for (NSUInteger d = 0; d < days.count && dayRows.count < 7; d++) {
		NSDictionary *day = days[d];
		NSArray *hourly = [day[@"hourly"] isKindOfClass:[NSArray class]] ? day[@"hourly"] : @[];
		NSDictionary *noon = hourly.count > 4 ? hourly[4] : hourly.firstObject;
		NSInteger code = hw_wmo_from_wwo([noon[@"weatherCode"] integerValue]);
		double high = hw_value(day[@"maxtempC"], -999);
		double low = hw_value(day[@"mintempC"], -999);
		NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
		NSDate *date;
		NSString *name;

		if (high < -100 || low < -100) {
			continue;
		}
		formatter.dateFormat = @"yyyy-MM-dd";
		date = [formatter dateFromString:day[@"date"]];
		name = date != nil ? dayNames[MAX(0, [[NSCalendar currentCalendar] component:NSCalendarUnitWeekday
		    fromDate:date] - 1)] : day[@"date"];
		[dayRows addObject:@{
			@"name": name,
			@"condition": hw_condition(code),
			@"symbol": hw_symbol(code),
			@"high": @(high),
			@"low": @(low),
		}];
	}

	{
		NSInteger code = hw_wmo_from_wwo([now[@"weatherCode"] integerValue]);

		[self.root showCurrentTemperature:hw_value(now[@"temp_C"], 0) condition:hw_condition(code)
		    symbol:hw_symbol(code)
		    details:[NSString stringWithFormat:@"体感 %.0f° · 湿度 %.0f%% · 风速 %.0f km/h",
		    hw_value(now[@"FeelsLikeC"], 0), hw_value(now[@"humidity"], 0),
		    hw_value(now[@"windspeedKmph"], 0)]];
	}
	[self.root showHours:hourCells];
	[self.root showDays:dayRows];
	[self.root showStatus:@"数据：wttr.in（备用）"];
}

- (void)renderData:(NSData *)data
{
	NSDictionary *root = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
	NSDictionary *current;
	NSDictionary *hourly;
	NSDictionary *daily;
	NSArray *hours;
	NSArray *days;
	NSMutableArray<NSDictionary *> *hourCells = [NSMutableArray array];
	NSMutableArray<NSDictionary *> *dayRows = [NSMutableArray array];

	if (![root isKindOfClass:[NSDictionary class]]) {
		[self.root showStatus:@"数据无法解析"];
		return;
	}
	current = root[@"current"];
	hourly = root[@"hourly"];
	daily = root[@"daily"];
	if (![current isKindOfClass:[NSDictionary class]]) {
		[self.root showStatus:@"数据缺少当前天气"];
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
	for (NSUInteger i = start; i < hours.count && hourCells.count < 12; i++) {
		double temperature;
		double code;

		if (i >= hourTemps.count || i >= hourCodes.count) {
			break;
		}
		temperature = hw_value(hourTemps[i], -999);
		code = hw_value(hourCodes[i], -999);
		if (temperature < -100 || code < 0) {
			continue;
		}
		[hourCells addObject:@{
			@"hour": hw_hour(hours[i]),
			@"temperature": @(temperature),
			@"symbol": hw_symbol((NSInteger)code),
		}];
	}

	NSArray *dayCodes = [daily[@"weather_code"] isKindOfClass:[NSArray class]] ? daily[@"weather_code"] : @[];
	NSArray *dayMax = [daily[@"temperature_2m_max"] isKindOfClass:[NSArray class]] ? daily[@"temperature_2m_max"] : @[];
	NSArray *dayMin = [daily[@"temperature_2m_min"] isKindOfClass:[NSArray class]] ? daily[@"temperature_2m_min"] : @[];
	NSArray *dayNames = @[@"周日", @"周一", @"周二", @"周三", @"周四", @"周五", @"周六"];
	for (NSUInteger i = 0; i < days.count && dayRows.count < 7; i++) {
		NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
		NSDate *date;
		NSString *name;
		double code;
		double high;
		double low;

		if (i >= dayCodes.count || i >= dayMax.count || i >= dayMin.count) {
			break;
		}
		formatter.dateFormat = @"yyyy-MM-dd";
		date = [formatter dateFromString:days[i]];
		name = date != nil ? dayNames[MAX(0, [[NSCalendar currentCalendar] component:NSCalendarUnitWeekday
		    fromDate:date] - 1)] : days[i];
		code = hw_value(dayCodes[i], -999);
		high = hw_value(dayMax[i], -999);
		low = hw_value(dayMin[i], -999);
		if (code < 0 || high < -100 || low < -100) {
			continue;
		}
		[dayRows addObject:@{
			@"name": name,
			@"condition": hw_condition((NSInteger)code),
			@"symbol": hw_symbol((NSInteger)code),
			@"high": @(high),
			@"low": @(low),
		}];
	}

	{
		NSNumber *temperature = current[@"temperature_2m"];
		NSNumber *feels = current[@"apparent_temperature"];
		NSNumber *humidity = current[@"relative_humidity_2m"];
		NSNumber *wind = current[@"wind_speed_10m"];
		NSNumber *code = current[@"weather_code"];
		NSString *condition = [code isKindOfClass:[NSNumber class]] ? hw_condition(code.integerValue) : @"—";
		NSString *symbol = [code isKindOfClass:[NSNumber class]] ? hw_symbol(code.integerValue) : @"cloud.fill";
		NSString *details = [NSString stringWithFormat:@"体感 %.0f° · 湿度 %.0f%% · 风速 %.0f km/h",
		    hw_value(feels, 0), hw_value(humidity, 0), hw_value(wind, 0)];

		[self.root showCurrentTemperature:hw_value(temperature, 0) condition:condition symbol:symbol details:details];
	}
	[self.root showHours:hourCells];
	[self.root showDays:dayRows];
	[self.root showStatus:@"数据：Open-Meteo"];
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([HWAppDelegate class]));
	}
}
