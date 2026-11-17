/*
 * HakiOS Maps' own tile view: OpenStreetMap raster tiles drawn with Core Graphics, pan,
 * pinch and double-tap, no WebKit and no VectorKit.  It is the page's fallback (and can be
 * the whole map) when WebKit's renderer does not come up on this port.
 */
#import <UIKit/UIKit.h>

@interface HakiosMapTileView : UIView

/* Degrees; the point at the middle of the view. */
@property (nonatomic, assign) double centerLatitude;
@property (nonatomic, assign) double centerLongitude;

/* OSM zoom level, 3..18 (fetched at one more, drawn at half the pixel size: sharp on this screen). */
@property (nonatomic, assign) NSInteger zoom;

/* Where the phone is, for the blue dot; NO until a fix arrives. */
@property (nonatomic, assign) BOOL hasFix;
@property (nonatomic, assign) double fixLatitude;
@property (nonatomic, assign) double fixLongitude;

/* The tile source: "{z}", "{x}" and "{y}" are substituted.  The app sets these from its
 * configuration file (tiles.conf), because openstreetmap.org is blocked on some networks
 * (mainland China answers with a poisoned address) and a reachable mirror or a keyed
 * service has to be usable without rebuilding the app. */
@property (nonatomic, copy) NSString *tileURLTemplate;
@property (nonatomic, copy) NSString *attributionText;
@property (nonatomic, assign) NSInteger maximumZoom;

- (void)setCenterLatitude:(double)latitude longitude:(double)longitude zoom:(NSInteger)zoom;
- (void)recenterOnFix;

@end
