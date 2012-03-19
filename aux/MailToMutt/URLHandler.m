#import <Cocoa/Cocoa.h>

#import "URLHandler.h"
#import "NSStringExt.h"
#import "Mutt.h"

@implementation URLHandler
- (void)awakeFromNib
{ /* this is from ed2k Helper, it registers that we want the getUrl method to handle kAEGetURL - an appleevent. It's all magic to me. */
	[[ NSAppleEventManager sharedAppleEventManager ]
        setEventHandler: self
            andSelector: @selector(getUrl:)
          forEventClass: kInternetEventClass
             andEventID: kAEGetURL ];
	NSDebug(@"Registered for apple event");
}
- (void)getUrl:(NSAppleEventDescriptor *)event
{
	NSString *urlString = [[event paramDescriptorForKeyword:'----'] stringValue ];
	NSDebug(@"GetURL apple event received with <<%@>>", urlString);
	NSURL *url = [ NSURL URLWithString:urlString ]; // get the URL delivered by the apple event
	
	if (url == nil){ // if NSURL did not eat it, then we give up
		NSDebug(@"couldn't parse URL");
		return;
	}
	NSDebug(@"URL successfully parsed");
	
	NSArray *parts = [[url resourceSpecifier] componentsSeparatedByString:@"?" ]; // get an address, and a query string
	
	NSString *email = [ [ parts objectAtIndex:0 ] urlDecode ];  // this is the email part of the URL

	NSMutableDictionary *paramDict = [ NSMutableDictionary dictionaryWithObjectsAndKeys:@"",@"subject",@"",@"body",nil ];

	if ([ parts count ] > 1){
		NSDebug(@"there are %d parts of the URL to check, %@.", [ parts count ], ([ parts count ] > 2 ? @"but there should only be 1 or 2" : @"which is good"));
		NSArray *params = [ [ parts objectAtIndex:1 ] componentsSeparatedByString:@"&" ]; // split into param=value pairs
		NSDebug(@"There are %d parameters (key=value) in the second part of the URL (the query string)", [ params count ]);
		NSEnumerator *cursor = [ params objectEnumerator ];
	
		id string; // points to an object, a param=value pair in this case, by walking params
	
		/* very cheap query param "parsing" */
		while(string = [ cursor nextObject ]){
			NSArray *kvp = [ string componentsSeparatedByString:@"=" ]; // seperate into key and value
			if ([ kvp count ] != 2){
				NSDebug(@"KVP count is %d!=2. Doesn't look like a valid query string part.", [ kvp count] );
				continue;
			}
			NSString *key = [ kvp objectAtIndex:0 ]; // this shuts up the warnings - i still don't know how to cast an object like [ (NSString *)[ kvp objectAtIndex: 0 ] compare:@"subject" ]
			NSDebug(@"param %@ found in URL, with value %@", key, [[ kvp objectAtIndex:1 ] urlDecode ]);
			if (![ key compare:@"subject" ] || ![ key compare:@"body" ]) // these are what we want
				[ paramDict setValue:[[ kvp objectAtIndex:1 ] urlDecode ] forKey:key ]; // urlDecode was stolen from iJournal
		}
	}

	NSDebug(@"Going to mail '%@' with subject '%@' and body:\n<<EOF\n%@\nEOF", email, [ paramDict valueForKey:@"subject" ], [ paramDict valueForKey:@"body" ]);
	
	/* create the message using ultraleet abstraction of Mutt ;-) */
	[ Mutt newMessageTo:email withSubject:[ paramDict valueForKey:@"subject" ] andBody:[ paramDict valueForKey:@"body" ] ];
}
@end
