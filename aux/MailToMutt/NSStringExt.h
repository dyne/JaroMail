/* Extensions to the NSString class */

#import "Foundation/Foundation.h"

@interface NSString (Misc)
- (NSString *) headerEscape; // makes a string suitable for inserting into an RFC822 header
- (NSString *) urlDecode;    // decodes strings like "foo+bar%20gorch" into "foo bar gorch"
@end