#import <Foundation/Foundation.h>

@interface URLHandler : NSObject
- (void)awakeFromNib;								// initial action, registers apple event
- (void)getUrl:(NSAppleEventDescriptor *)event;		// handles the GetURL event
@end
