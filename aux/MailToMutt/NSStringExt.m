/* adapted from the iJournal project ( http://sf.net/projects/ijournal ) */

#import "NSStringExt.h"


@implementation NSString (Misc)
- (NSString *) headerEscape
{
	if ([ self length ] > 0){
		NSMutableString *clean = [ self mutableCopy ];
		
		/* clean up the string so that it's line breaks are all CRLF, and followed by spaces */
		[ clean replaceOccurrencesOfString:@"\r" withString:@"\n" options:0 range:NSMakeRange(0, [ clean length ]) ]; /* make all CRs be LFs */
		NSArray *lines = [ clean componentsSeparatedByString:@"\n" ]; /* split on LFs */
		[ clean release ]; /* throw away the changed string, we no longer need it */
		
		if ([ lines count ] > 0){
			NSEnumerator *cursor = [ lines objectEnumerator ]; /* iterate the array of lines */
			
			clean = [ [ [ NSMutableString alloc ] initWithCapacity:([ self length ] + 20) ] autorelease ]; /* better to malloc a bit extra than to malloc many times */
			[ clean appendString:[ cursor nextObject ] ]; /* add the first part now, we don't want it with a CRLF before it */
			
			NSString *line;
			while(line = [ cursor nextObject ]){
				if ([ line length ] > 0){ /* some lines might be empty, we don't want them */
					[ clean appendString:@"\015\012    " ]; /* RFC822 header continuations are CRLF, then whitespace */
					[ clean appendString:line ];
				} else {
					NSDebug(@"Empty line");
				}
			}

			return clean;
		}
	}

	NSDebug(@"Empty string escaped for header");

	return @"";
}
- (NSString *) urlDecode
{
    NSString *decoded;
    NSMutableString *decodedSpaces;

    // first convert the +'s back into whitespaces
    decodedSpaces = [NSMutableString stringWithString:self];
    [decodedSpaces replaceOccurrencesOfString:@"+"
                                   withString:@" "
                                      options:0
                                        range:NSMakeRange(0, [self length])];
    
    // then convert all the %-escaped sequences back into chars
	decoded = (NSString *)CFURLCreateStringByReplacingPercentEscapes(NULL,(CFStringRef)decodedSpaces, (CFStringRef)@""); /* NSString has stringByReplacingPercentEscapesUsingEncding - i don't know which encoding. UTF8 is probably a safe bet */
    [ decoded autorelease ]; // if i got the docs right, CFUrlCreateString.... returns an object with a reference count of 1, as if i did alloc on my own

    return decoded;
}
@end
