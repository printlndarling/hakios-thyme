/*
 * HakiOS Maps' own tile view (see HakiosMapTileView.h).
 *
 * The math is the slippy-map one: x = (lon+180)/360, y = the Mercator term, both times
 * 128 * 2^(z+1) points, because each 256-pixel tile is fetched at one zoom level more and
 * drawn at 128 points -- 384 device pixels of image into 384 device pixels of screen on the
 * phone's 3x panel, which keeps the labels readable.
 *
 * Tiles are fetched with NSURLSession and kept in a dictionary (bounded); a tile that
 * arrives or fails calls setNeedsDisplay, and drawRect paints only what is on screen.
 * The pan and pinch recognizers move the center directly: during a pinch the drawing is
 * scaled by pinchScale and, when the fingers lift, the zoom is the old one plus that factor
 * in powers of two, so the map never sits at a fractional zoom.
 */
#import "HakiosMapTileView.h"

#define HM_TILE_POINTS 128.0
#define HM_MIN_ZOOM 3
#define HM_MAX_ZOOM 18
#define HM_TILE_CACHE_MAX 512

static double
hm_mercator_y(double latitude)
{
	double s = sin(latitude * M_PI / 180.0);

	if (s > 0.9999) {
		s = 0.9999;
	} else if (s < -0.9999) {
		s = -0.9999;
	}
	return 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
}

static double
hm_inverse_mercator_y(double y)
{
	return 90.0 - 360.0 * atan(exp((0.5 - y) * 2.0 * M_PI)) / M_PI;
}

@interface HakiosMapTileView () <NSURLSessionDataDelegate>
@property (nonatomic, strong) NSMutableDictionary<NSString *, id> *tiles;   /* UIImage, or NSNull */
@property (nonatomic, strong) NSMutableSet<NSString *> *pending;
@property (nonatomic, strong) NSURLSession *session;
@property (nonatomic, assign) CGFloat pinchScale;
@property (nonatomic, assign) CGPoint pinchCenter;
@property (nonatomic, assign) BOOL recenterOnce;
@end

@implementation HakiosMapTileView

- (instancetype)initWithFrame:(CGRect)frame
{
	self = [super initWithFrame:frame];
	if (self == nil) {
		return nil;
	}
	self.backgroundColor = [UIColor colorWithWhite:0.91 alpha:1.0];
	self.opaque = YES;
	self.multipleTouchEnabled = YES;
	_tiles = [NSMutableDictionary dictionary];
	_pending = [NSMutableSet set];
	_zoom = 14;
	_centerLatitude = 34.3416;      /* Xi'an, until a fix arrives */
	_centerLongitude = 108.9398;
	_pinchScale = 1.0;

	NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration defaultSessionConfiguration];

	configuration.HTTPAdditionalHeaders = @{ @"User-Agent" : @"HakiosMaps/1.0 (HakiOS, thyme)" };
	configuration.timeoutIntervalForRequest = 20;
	_session = [NSURLSession sessionWithConfiguration:configuration delegate:self delegateQueue:nil];

	UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(panned:)];
	UIPinchGestureRecognizer *pinch = [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(pinched:)];
	UITapGestureRecognizer *doubleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(doubleTapped:)];
	UITapGestureRecognizer *singleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(singleTapped:)];

	doubleTap.numberOfTapsRequired = 2;
	[singleTap requireGestureRecognizerToFail:doubleTap];
	[self addGestureRecognizer:pan];
	[self addGestureRecognizer:pinch];
	[self addGestureRecognizer:doubleTap];
	[self addGestureRecognizer:singleTap];
	return self;
}

/* ---------------------------------------------------------------- the world, in points */

- (double)worldSize
{
	return HM_TILE_POINTS * (double)(1ULL << (self.zoom + 1));
}

- (CGPoint)centerInWorld
{
	double world = [self worldSize];
	double x = (self.centerLongitude + 180.0) / 360.0;
	double y = hm_mercator_y(self.centerLatitude);

	return CGPointMake(x * world, y * world);
}

- (void)setCenterFromWorld:(CGPoint)world
{
	double size = [self worldSize];
	double x = world.x / size;
	double y = world.y / size;

	x = fmod(x, 1.0);
	if (x < 0) {
		x += 1.0;
	}
	if (y < 0.02) {
		y = 0.02;
	} else if (y > 0.98) {
		y = 0.98;
	}
	self.centerLongitude = x * 360.0 - 180.0;
	self.centerLatitude = hm_inverse_mercator_y(y);
}

- (void)setCenterLatitude:(double)latitude longitude:(double)longitude zoom:(NSInteger)zoom
{
	self.centerLatitude = latitude;
	self.centerLongitude = longitude;
	self.zoom = MAX(HM_MIN_ZOOM, MIN(HM_MAX_ZOOM, zoom));
	[self setNeedsDisplay];
}

- (void)recenterOnFix
{
	if (!self.hasFix) {
		return;
	}
	self.recenterOnce = NO;
	[self setCenterLatitude:self.fixLatitude longitude:self.fixLongitude zoom:MAX(self.zoom, 15)];
}

- (void)setHasFix:(BOOL)hasFix
{
	_hasFix = hasFix;
	[self setNeedsDisplay];
}

- (void)setFixLatitude:(double)latitude
{
	_fixLatitude = latitude;
	[self setNeedsDisplay];
}

- (void)setFixLongitude:(double)longitude
{
	_fixLongitude = longitude;
	[self setNeedsDisplay];
}

/* ---------------------------------------------------------------- gestures */

- (void)panned:(UIPanGestureRecognizer *)pan
{
	CGPoint translation = [pan translationInView:self];

	if (pan.state == UIGestureRecognizerStateBegan || pan.state == UIGestureRecognizerStateChanged) {
		CGPoint center = [self centerInWorld];

		center.x -= translation.x / self.pinchScale;
		center.y -= translation.y / self.pinchScale;
		[self setCenterFromWorld:center];
		[pan setTranslation:CGPointZero inView:self];
		[self setNeedsDisplay];
	}
}

- (void)pinched:(UIPinchGestureRecognizer *)pinch
{
	if (pinch.state == UIGestureRecognizerStateBegan || pinch.state == UIGestureRecognizerStateChanged) {
		self.pinchScale = MAX(0.25, MIN(4.0, pinch.scale));
		[self setNeedsDisplay];
	} else if (pinch.state == UIGestureRecognizerStateEnded || pinch.state == UIGestureRecognizerStateCancelled) {
		double delta = log2(self.pinchScale);
		NSInteger step = (NSInteger)llround(delta);

		self.pinchScale = 1.0;
		if (step != 0) {
			self.zoom = MAX(HM_MIN_ZOOM, MIN(HM_MAX_ZOOM, self.zoom + step));
		}
		[self setNeedsDisplay];
	}
}

- (void)doubleTapped:(UITapGestureRecognizer *)tap
{
	CGPoint point = [tap locationInView:self];
	CGPoint center = [self centerInWorld];

	/* The point under the fingers stays under them when the zoom goes up. */
	self.zoom = MAX(HM_MIN_ZOOM, MIN(HM_MAX_ZOOM, self.zoom + 1));
	center.x = (center.x - self.bounds.size.width / 2.0 + (point.x - self.bounds.size.width / 2.0) / 2.0);
	center.y = (center.y - self.bounds.size.height / 2.0 + (point.y - self.bounds.size.height / 2.0) / 2.0);
	[self setCenterFromWorld:center];
	[self setNeedsDisplay];
}

- (void)singleTapped:(UITapGestureRecognizer *)tap
{
	(void)tap;
}

/* ---------------------------------------------------------------- tiles */

- (void)requestTile:(NSString *)key
{
	if (self.tiles[key] != nil || [self.pending containsObject:key]) {
		return;
	}
	NSArray<NSString *> *parts = [key componentsSeparatedByString:@"/"];

	if (parts.count != 3) {
		return;
	}
	NSString *url = [NSString stringWithFormat:@"https://tile.openstreetmap.org/%@/%@/%@.png",
	    parts[0], parts[1], parts[2]];
	NSURLSessionDataTask *task = [self.session dataTaskWithURL:[NSURL URLWithString:url]];

	task.taskDescription = key;
	[self.pending addObject:key];
	[task resume];
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task
    didReceiveData:(NSData *)data
{
	NSString *key = task.taskDescription;
	UIImage *image = [UIImage imageWithData:data];

	if (key == nil) {
		return;
	}
	if (image != nil) {
		@synchronized (self.tiles) {
			if (self.tiles.count > HM_TILE_CACHE_MAX) {
				[self.tiles removeAllObjects];
			}
			self.tiles[key] = image;
		}
	} else {
		self.tiles[key] = [NSNull null];
	}
	[self.pending removeObject:key];
	dispatch_async(dispatch_get_main_queue(), ^{
		[self setNeedsDisplay];
	});
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error
{
	NSString *key = task.taskDescription;

	if (key == nil) {
		return;
	}
	[self.pending removeObject:key];
	if (error != nil) {
		@synchronized (self.tiles) {
			self.tiles[key] = [NSNull null];        /* no retry storm for this one */
		}
	}
}

/* ---------------------------------------------------------------- drawing */

- (void)drawRect:(CGRect)rect
{
	CGContextRef context = UIGraphicsGetCurrentContext();
	double world = [self worldSize] * self.pinchScale;
	CGPoint center = [self centerInWorld];
	NSInteger fetchZoom = self.zoom + 1;
	NSInteger tilesPerSide = 1 << fetchZoom;
	double tilePoints = HM_TILE_POINTS * self.pinchScale;
	CGFloat width = self.bounds.size.width, height = self.bounds.size.height;
	double originX = center.x * self.pinchScale - width / 2.0;
	double originY = center.y * self.pinchScale - height / 2.0;

	(void)world;
	CGContextSetShouldSmoothFonts(context, false);
	for (NSInteger tx = (NSInteger)floor(originX / tilePoints); (double)tx * tilePoints < originX + width; tx++) {
		for (NSInteger ty = (NSInteger)floor(originY / tilePoints); (double)ty * tilePoints < originY + height; ty++) {
			NSInteger wrappedX = tx % tilesPerSide;

			if (wrappedX < 0) {
				wrappedX += tilesPerSide;
			}
			if (ty < 0 || ty >= tilesPerSide) {
				continue;
			}
			NSString *key = [NSString stringWithFormat:@"%ld/%ld/%ld", (long)fetchZoom, (long)wrappedX, (long)ty];
			id tile = self.tiles[key];

			if (tile == nil) {
				[self requestTile:key];
				tile = nil;
			}
			if (tile == [NSNull null] || tile == nil) {
				continue;
			}
			CGRect tileRect = CGRectMake((double)tx * tilePoints - originX,
			    (double)ty * tilePoints - originY, tilePoints + 1.0, tilePoints + 1.0);

			[(UIImage *)tile drawInRect:tileRect];
		}
	}

	/* The position, when there is one. */
	if (self.hasFix) {
		double size = [self worldSize];
		double x = (self.fixLongitude + 180.0) / 360.0 * size * self.pinchScale;
		double y = hm_mercator_y(self.fixLatitude) * size * self.pinchScale;
		CGFloat px = x - originX, py = y - originY;

		if (px > -40 && px < width + 40 && py > -40 && py < height + 40) {
			CGContextSetFillColorWithColor(context, [UIColor whiteColor].CGColor);
			CGContextFillEllipseInRect(context, CGRectMake(px - 11, py - 11, 22, 22));
			CGContextSetFillColorWithColor(context, [UIColor colorWithRed:0.04 green:0.52 blue:1.0 alpha:1.0].CGColor);
			CGContextFillEllipseInRect(context, CGRectMake(px - 8, py - 8, 16, 16));
		}
	}

	/* The attribution OpenStreetMap's licence requires. */
	UIFont *font = [UIFont systemFontOfSize:11];
	NSDictionary *attributes = @{ NSFontAttributeName : font, NSForegroundColorAttributeName : [UIColor colorWithWhite:0.25 alpha:1.0] };
	NSString *credit = @"© OpenStreetMap contributors";
	CGSize creditSize = [credit sizeWithAttributes:attributes];
	CGRect creditRect = CGRectMake(width - creditSize.width - 8, height - creditSize.height - 6,
	    creditSize.width, creditSize.height);

	CGContextSetFillColorWithColor(context, [UIColor colorWithWhite:1.0 alpha:0.7].CGColor);
	CGContextFillRect(context, CGRectInset(creditRect, -4, -2));
	[credit drawAtPoint:creditRect.origin withAttributes:attributes];
}

@end
