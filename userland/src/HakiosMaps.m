/*
 * HakiOS Maps: a map screen that does not go through VectorKit.
 *
 * Apple Maps draws its map with VectorKit's Metal pipelines, which this port does not
 * serve yet: the app's UI is there but the canvas stays black (CHECKLIST, "Maps").  This
 * application is the working screen: a web view over the bundled Leaflet and
 * OpenStreetMap's raster tiles -- images on the path Safari already renders on this port.
 *
 * The web view is WKWebView, but with WebKit's GPU process kept out of DOM rendering where
 * the SPI allows it: on this port the GPU process starts, creates its Metal device and a
 * 1125x1536 IOSurface, then aborts (logs/174007-run.log, pid 63), and the view stays its
 * own background colour.  Rendering in the WebContent process is the path the home screen's
 * CA plug-in already draws.  The page is loaded from a string (not a file URL) with the
 * CSS and JS inlined, so the load does not depend on WKWebView's file-access rules.
 *
 * It is installed the way LocProbe and HakiosWeather are: a scene copies the bundle from
 * /redwood/fw/hmaps into /var/jb/Applications, uicache registers it, uiopen opens
 * com.hakios.maps.  What it does goes to /private/var/tmp/hmaps.log (world-writable, and
 * readable from a scene) and to NSLog; an application's NSLog alone does not reach the
 * run's console.  Tile traffic is OpenStreetMap's own, credited on the page.
 */
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <CoreLocation/CoreLocation.h>
#import <QuartzCore/QuartzCore.h>
#import "HakiosMapTileView.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void
hm_log_path(const char *path, NSString *line)
{
	FILE *log = fopen(path, "a");
	time_t now;
	struct tm tm;
	char stamp[32];

	if (log == NULL) {
		return;
	}
	now = time(NULL);
	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
	fprintf(log, "%s %s\n", stamp, line.UTF8String);
	fclose(log);
}

static void
hm_note(NSString *format, ...)
{
	va_list args;
	va_start(args, format);
	NSString *line = [[NSString alloc] initWithFormat:format arguments:args];

	va_end(args);
	NSLog(@"HakiosMaps: %@", line);
	hm_log_path("/private/var/tmp/hmaps.log", line);
	hm_log_path("/var/jb/hmaps/maps.log", line);
}

/* WebKit's own knobs are SPI; set them when the class answers, and say what happened. */
static BOOL
hm_set_bool(id object, NSString *name, BOOL value)
{
	SEL selector = NSSelectorFromString(name);

	if (object == nil || ![object respondsToSelector:selector]) {
		return NO;
	}
	typedef void (*Setter)(id, SEL, BOOL);
	Setter setter = (Setter)[object methodForSelector:selector];

	setter(object, selector, value);
	return YES;
}

@interface HMapsController : UIViewController <WKNavigationDelegate, WKUIDelegate, CLLocationManagerDelegate,
    UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, strong) WKWebView *web;
@property (nonatomic, strong) HakiosMapTileView *tileView;
@property (nonatomic, strong) UIButton *locateButton;
@property (nonatomic, strong) UITextField *searchField;
@property (nonatomic, strong) UITableView *resultsTable;
@property (nonatomic, strong) NSArray<NSDictionary *> *results;
@property (nonatomic, strong) NSURLSession *searchSession;
@property (nonatomic, strong) CLLocationManager *manager;
@property (nonatomic, assign) double latitude;
@property (nonatomic, assign) double longitude;
@property (nonatomic, assign) BOOL hasFix;
@property (nonatomic, assign) BOOL pageReady;
@property (nonatomic, assign) BOOL terminated;
@property (nonatomic, assign) BOOL fellBack;
@property (nonatomic, assign) BOOL autoSearch;
@end

@implementation HMapsController

- (NSString *)pageString
{
	NSString *directory = [[NSBundle mainBundle] resourcePath];
	NSString *html = [NSString stringWithContentsOfFile:[directory stringByAppendingPathComponent:@"index.html"]
	    encoding:NSUTF8StringEncoding error:NULL];
	NSString *css = [NSString stringWithContentsOfFile:[directory stringByAppendingPathComponent:@"leaflet.css"]
	    encoding:NSUTF8StringEncoding error:NULL];
	NSString *js = [NSString stringWithContentsOfFile:[directory stringByAppendingPathComponent:@"leaflet.js"]
	    encoding:NSUTF8StringEncoding error:NULL];

	if (html == nil) {
		return nil;
	}
	if (css != nil) {
		html = [html stringByReplacingOccurrencesOfString:
		    @"<link rel=\"stylesheet\" href=\"leaflet.css\">"
		    withString:[NSString stringWithFormat:@"<style>%@</style>", css]];
	}
	if (js != nil) {
		html = [html stringByReplacingOccurrencesOfString:@"<script src=\"leaflet.js\"></script>"
		    withString:[NSString stringWithFormat:@"<script>%@</script>", js]];
	}
	return html;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = [UIColor colorWithWhite:0.91 alpha:1.0];

	WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
	WKPreferences *preferences = configuration.preferences;

	/* The GPU process aborts on this port (see the file comment); render in-process instead. */
	hm_note(@"SPI: DOM rendering in the GPU process: %@",
	    hm_set_bool(preferences, @"_setUseGPUProcessForDOMRenderingEnabled:", NO) ? @"off" : @"no such setter");
	hm_note(@"SPI: canvas in the GPU process: %@",
	    hm_set_bool(preferences, @"_setUseGPUProcessForCanvasRenderingEnabled:", NO) ? @"off" : @"no such setter");
	hm_note(@"SPI: WebGL in the GPU process: %@",
	    hm_set_bool(preferences, @"_setUseGPUProcessForWebGLEnabled:", NO) ? @"off" : @"no such setter");

	self.web = [[WKWebView alloc] initWithFrame:self.view.bounds configuration:configuration];
	self.web.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	self.web.navigationDelegate = self;
	self.web.UIDelegate = self;
	self.web.opaque = NO;
	self.web.backgroundColor = [UIColor colorWithWhite:0.91 alpha:1.0];
	[self.view addSubview:self.web];
	[self addSearchBar];

	NSString *html = [self pageString];

	if (html == nil) {
		hm_note(@"the bundle's index.html is missing");
		return;
	}
	hm_note(@"loading the page from a string (%lu characters)", (unsigned long)html.length);
	[self.web loadHTMLString:html baseURL:[NSURL URLWithString:@"https://tile.openstreetmap.org/"]];

	/* WebKit's renderer does not come up on every boot of this port; the tile view below is
	 * the map either way, so the page gets fourteen seconds and no more. */
	[NSTimer scheduledTimerWithTimeInterval:14.0 target:self selector:@selector(fallbackFired:) userInfo:nil repeats:NO];

	self.manager = [[CLLocationManager alloc] init];
	self.manager.delegate = self;
	if ([self.manager respondsToSelector:@selector(requestWhenInUseAuthorization)]) {
		[self.manager requestWhenInUseAuthorization];
	}
	[self.manager startUpdatingLocation];

	/* A run cannot type: a scene leaves a query in /private/var/tmp/hmaps-search.txt and the
	 * search runs by itself three seconds in, so the Nominatim path is exercised end to end. */
	[NSTimer scheduledTimerWithTimeInterval:3.0 target:self selector:@selector(autoSearchFired:) userInfo:nil repeats:NO];
}

- (void)autoSearchFired:(NSTimer *)timer
{
	NSString *query = [NSString stringWithContentsOfFile:@"/private/var/tmp/hmaps-search.txt"
	    encoding:NSUTF8StringEncoding error:NULL];

	(void)timer;
	query = [query stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	if (query.length == 0) {
		return;
	}
	hm_note(@"auto search: %@", query);
	self.autoSearch = YES;
	self.searchField.text = query;
	[self searchSubmitted];
}

/* ---------------------------------------------------------------- search (Nominatim) */

- (void)addSearchBar
{
	UIControl *padding = [[UIControl alloc] initWithFrame:CGRectMake(0, 0, 12, 40)];

	self.searchField = [[UITextField alloc] initWithFrame:CGRectMake(12, 44, self.view.bounds.size.width - 24, 40)];
	self.searchField.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.96];
	self.searchField.layer.cornerRadius = 10;
	self.searchField.placeholder = @"搜索地点（OpenStreetMap）";
	self.searchField.font = [UIFont systemFontOfSize:16];
	self.searchField.returnKeyType = UIReturnKeySearch;
	self.searchField.clearButtonMode = UITextFieldViewModeWhileEditing;
	self.searchField.leftView = padding;
	self.searchField.leftViewMode = UITextFieldViewModeAlways;
	self.searchField.autoresizingMask = UIViewAutoresizingFlexibleWidth;
	[self.searchField addTarget:self action:@selector(searchSubmitted) forControlEvents:UIControlEventEditingDidEndOnExit];
	[self.view addSubview:self.searchField];

	self.resultsTable = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
	self.resultsTable.dataSource = self;
	self.resultsTable.delegate = self;
	self.resultsTable.rowHeight = 54;
	self.resultsTable.hidden = YES;
	self.resultsTable.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.97];
	self.resultsTable.layer.cornerRadius = 10;
	self.resultsTable.separatorInset = UIEdgeInsetsMake(0, 12, 0, 12);
	[self.view addSubview:self.resultsTable];

	NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration defaultSessionConfiguration];

	configuration.HTTPAdditionalHeaders = @{ @"User-Agent" : @"HakiosMaps/1.0 (HakiOS, thyme)" };
	configuration.timeoutIntervalForRequest = 20;
	self.searchSession = [NSURLSession sessionWithConfiguration:configuration];
}

- (void)searchSubmitted
{
	NSString *query = [self.searchField.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];

	if (query.length == 0) {
		return;
	}
	NSString *escaped = [query stringByAddingPercentEncodingWithAllowedCharacters:
	    [NSCharacterSet URLQueryAllowedCharacterSet]];
	NSString *address = [NSString stringWithFormat:
	    @"https://nominatim.openstreetmap.org/search?format=jsonv2&limit=6&q=%@", escaped];
	NSURL *url = [NSURL URLWithString:address];

	hm_note(@"search: %@", query);
	[self.searchField resignFirstResponder];
	if (url == nil) {
		return;
	}
	NSURLSessionDataTask *task = [self.searchSession dataTaskWithURL:url
	    completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
		NSArray *items = nil;

		if (data != nil) {
			id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];

			if ([json isKindOfClass:[NSArray class]]) {
				items = json;
			}
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			self.results = (items != nil) ? items : @[];
			hm_note(@"search: %lu results (%@)", (unsigned long)self.results.count,
			    error != nil ? error.localizedDescription : @"ok");

			CGRect field = self.searchField.frame;
			CGFloat height = MIN(54.0 * (CGFloat)self.results.count, self.view.bounds.size.height - 220);

			self.resultsTable.frame = CGRectMake(field.origin.x, CGRectGetMaxY(field) + 6,
			    field.size.width, MAX(height, 0));
			self.resultsTable.hidden = (self.results.count == 0);
			[self.resultsTable reloadData];
			[self.view bringSubviewToFront:self.resultsTable];

			/* Nobody can tap in a run: the first hit is taken for the scene. */
			if (self.autoSearch && self.results.count > 0) {
				self.autoSearch = NO;
				hm_note(@"auto search: taking the first hit");
				[self tableView:self.resultsTable didSelectRowAtIndexPath:[NSIndexPath indexPathForRow:0 inSection:0]];
			}
		});
	}];

	[task resume];
}

- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section
{
	(void)table;
	(void)section;
	return (NSInteger)self.results.count;
}

- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = [table dequeueReusableCellWithIdentifier:@"hit"];

	if (cell == nil) {
		cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"hit"];
	}
	NSDictionary *item = self.results[(NSUInteger)indexPath.row];
	id name = item[@"display_name"], type = item[@"type"];

	cell.textLabel.text = [name isKindOfClass:[NSString class]] ? name : @"?";
	cell.textLabel.font = [UIFont systemFontOfSize:14];
	cell.textLabel.numberOfLines = 2;
	cell.detailTextLabel.text = [type isKindOfClass:[NSString class]] ? type : @"";
	cell.detailTextLabel.font = [UIFont systemFontOfSize:11];
	return cell;
}

- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	NSDictionary *item = self.results[(NSUInteger)indexPath.row];
	double latitude = [item[@"lat"] doubleValue];
	double longitude = [item[@"lon"] doubleValue];

	(void)table;
	self.resultsTable.hidden = YES;
	[self.searchField resignFirstResponder];
	hm_note(@"search: going to %.5f, %.5f", latitude, longitude);
	[self goToLatitude:latitude longitude:longitude];
}

- (void)goToLatitude:(double)latitude longitude:(double)longitude
{
	if (self.tileView != nil) {
		[self.tileView setCenterLatitude:latitude longitude:longitude zoom:MAX(15, self.tileView.zoom)];
		return;
	}
	if (self.pageReady && self.web != nil) {
		NSString *call = [NSString stringWithFormat:@"window.hmapsGo && window.hmapsGo(%.6f, %.6f);",
		    latitude, longitude];

		[self.web evaluateJavaScript:call completionHandler:nil];
	}
}

- (void)fallbackFired:(NSTimer *)timer
{
	(void)timer;
	if (self.pageReady || self.fellBack) {
		return;
	}
	self.fellBack = YES;
	hm_note(@"WebKit has not drawn the page: the native tile view takes over");
	[self.web removeFromSuperview];
	self.web = nil;

	self.tileView = [[HakiosMapTileView alloc] initWithFrame:self.view.bounds];
	self.tileView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	if (self.hasFix) {
		self.tileView.fixLatitude = self.latitude;
		self.tileView.fixLongitude = self.longitude;
		self.tileView.hasFix = YES;
		[self.tileView recenterOnFix];
	}
	[self.view insertSubview:self.tileView atIndex:0];

	self.locateButton = [UIButton buttonWithType:UIButtonTypeSystem];
	[self.locateButton setTitle:@"定位" forState:UIControlStateNormal];
	self.locateButton.titleLabel.font = [UIFont boldSystemFontOfSize:17];
	self.locateButton.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.94];
	self.locateButton.layer.cornerRadius = 12;
	self.locateButton.frame = CGRectMake(12, 92, 76, 44);
	[self.locateButton addTarget:self action:@selector(locateTapped) forControlEvents:UIControlEventTouchUpInside];
	[self.view addSubview:self.locateButton];

	UIButton *zoomIn = [UIButton buttonWithType:UIButtonTypeSystem];
	UIButton *zoomOut = [UIButton buttonWithType:UIButtonTypeSystem];
	NSArray *zoomButtons = @[ zoomIn, zoomOut ];
	NSArray *zoomTitles = @[ @"+", @"−" ];
	NSArray *zoomActions = @[ @"zoomInTapped", @"zoomOutTapped" ];

	for (NSUInteger i = 0; i < zoomButtons.count; i++) {
		UIButton *button = zoomButtons[i];

		[button setTitle:zoomTitles[i] forState:UIControlStateNormal];
		button.titleLabel.font = [UIFont boldSystemFontOfSize:22];
		button.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.94];
		button.layer.cornerRadius = 10;
		button.frame = CGRectMake(self.view.bounds.size.width - 60, 92 + i * 56, 44, 44);
		button.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin;
		[button addTarget:self action:NSSelectorFromString(zoomActions[i]) forControlEvents:UIControlEventTouchUpInside];
		[self.view addSubview:button];
	}
	[self givePosition];
}

- (void)locateTapped
{
	if (self.tileView != nil && self.hasFix) {
		[self.tileView recenterOnFix];
	}
}

- (void)zoomInTapped
{
	if (self.tileView != nil) {
		self.tileView.zoom = MIN(18, self.tileView.zoom + 1);
		[self.tileView setNeedsDisplay];
	}
}

- (void)zoomOutTapped
{
	if (self.tileView != nil) {
		self.tileView.zoom = MAX(3, self.tileView.zoom - 1);
		[self.tileView setNeedsDisplay];
	}
}

- (void)givePosition
{
	if (!self.hasFix) {
		return;
	}
	if (self.tileView != nil) {
		self.tileView.fixLatitude = self.latitude;
		self.tileView.fixLongitude = self.longitude;
		self.tileView.hasFix = YES;
		return;
	}
	if (!self.pageReady || self.web == nil) {
		return;
	}
	NSString *call = [NSString stringWithFormat:@"window.hmapsFix && window.hmapsFix(%.6f, %.6f);",
	    self.latitude, self.longitude];

	[self.web evaluateJavaScript:call completionHandler:^(id result, NSError *error) {
		if (error != nil) {
			hm_note(@"hmapsFix failed: %@", error.localizedDescription);
		}
	}];
}

- (void)locationManager:(CLLocationManager *)manager didUpdateLocations:(NSArray<CLLocation *> *)locations
{
	CLLocation *location = locations.lastObject;

	if (location == nil) {
		return;
	}
	self.latitude = location.coordinate.latitude;
	self.longitude = location.coordinate.longitude;
	self.hasFix = YES;
	hm_note(@"position %.5f, %.5f (+/- %.0f m)", self.latitude, self.longitude, location.horizontalAccuracy);
	[self givePosition];
}

- (void)locationManager:(CLLocationManager *)manager didFailWithError:(NSError *)error
{
	hm_note(@"position failed: %@", error.localizedDescription);
}

- (void)webView:(WKWebView *)web didStartProvisionalNavigation:(WKNavigation *)navigation
{
	hm_note(@"navigation started");
}

- (void)webView:(WKWebView *)web didCommitNavigation:(WKNavigation *)navigation
{
	hm_note(@"navigation committed");
}

- (void)webView:(WKWebView *)web didFinishNavigation:(WKNavigation *)navigation
{
	self.pageReady = YES;
	hm_note(@"page loaded");
	[self givePosition];

	/* What the page itself says it is: the answer that separates "no WebKit content" from
	 * "content but no paint". */
	[web evaluateJavaScript:@"document.readyState + ' / ' + document.title"
	    completionHandler:^(id result, NSError *error) {
		hm_note(@"page says: %@ (error %@)", result, error.localizedDescription);
	}];
}

- (void)webView:(WKWebView *)web didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
	hm_note(@"navigation failed: %@", error.localizedDescription);
}

- (void)webView:(WKWebView *)web didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
	hm_note(@"provisional failed: %@", error.localizedDescription);
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)web
{
	hm_note(@"the WebContent process terminated (a reload follows)");
	if (!self.terminated) {
		self.terminated = YES;
		[web reload];
	}
}

@end

@interface HMapsDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) HMapsController *root;
@end

@implementation HMapsDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	self.root = [[HMapsController alloc] init];
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = self.root;
	[self.window makeKeyAndVisible];
	hm_note(@"app delegate: window up");
	return YES;
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([HMapsDelegate class]));
	}
}
