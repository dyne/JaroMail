#import "NSStringExt.h"
#import "Mutt.h"

#include <string.h>
#include <unistd.h>

@implementation Mutt

/* opens mutt with a file */

+ (void) newMessageFromFile:(NSString *)path
{
	NSDebug(@"Opening mutt with file %@", path);

	/* create the source to the script */
	NSString *source = [ NSString stringWithFormat:@"tell application \"Terminal\"\nactivate\ndo script \"mutt -H '%@' \"\nend tell", path ];

	/* create the NSAppleScript object with the source */
#	warning NSAppleScript has a leak when initializing objects
	NSAppleScript *script = [ [ [ NSAppleScript alloc ] initWithSource:source ] autorelease ];
	
	NSDebug(@"The compiled script is \n<<EOF\n%@\nEOF", [ script source ]);

	/* create a dictionary in which the execution will store it's errors */
	NSMutableDictionary *dict = [ NSMutableDictionary dictionary ];

	/* execute */
	if ([ script executeAndReturnError:&dict ]){
		NSDebug(@"Script executed OK");
	} else {
		NSDebug(@"Couldn't execute script:\n%@", dict);
	}
}

/* makes a temp file */

+ (void) newMessageWithContent:(NSString *)content
{
	NSDebug(@"creating message with content\n<<EOF\n%@\nEOF", content);

	/* use the mktemp(3) family of functions (standard C library) to create a temporary file, openned atomically in the users home directory */
	/* mkstemp takes a template - it replaces the XX part at the end with random stuff, and then opens the file safely, that is only if it doesn't exist */
	/* we first build a format, which should be like /tmp/501/Temporary\ Items/MailtoMutt-XXXXXXXX, and then get the cString out of it */
	/* the following code section leaves us with template, a c string which we have to dispose of */

#	warning UTF8String created - we might prefer ASCII. How do we do it cleanly? NSString has got a notion of a default encoding (user preferences setting), which affects the cString methods
	NSDebug(@"Preparing UTF8 string");
	const char *constTemplate = [ [ NSString stringWithFormat:@"%@/MailtoMutt-XXXXXXXX", NSTemporaryDirectory() ] UTF8String ]; /* create the filename. It's constant, so we have to copy it */
	NSDebug(@"Determining length of constTemplate");
	size_t templateLength = (strlen(constTemplate) + 1) * sizeof(char); /* the length of the string, plus the null byte */
	NSDebug(@"created template %s with length %d", constTemplate, templateLength);

	char *template;
#	warning malloc fail will not raise exception but simply return
	if ((template = (char *)malloc(templateLength)) == NULL){ /* malloc this length */
		NSDebug(@"Malloc failed");
		return; // should raise NSException instead
	}

	(void)strlcpy(template, constTemplate, templateLength); /* copy the string to a non const one. beh. */
	NSDebug(@"copied string");
	int fd = mkstemp(template); /* make the template */
	NSDebug(@"mkstmp created <<%s>>", template);

	/* create a filehandle out of the filedescriptor that mkstemp gave us */
	NSFileHandle *fh = [ [ [ NSFileHandle alloc ] initWithFileDescriptor:fd ] autorelease ];
	
	
	NSDebug(@"Writing message:\n<<EOF\n%@\nEOF", content);
	[ fh writeData:[ content dataUsingEncoding:NSUTF8StringEncoding ] ]; /* write the contents into the file */
	[ fh closeFile ]; /* finished writing */

		
	/* use the temp file to create a mutt message */
	[ [ self class ] newMessageFromFile:[ NSString stringWithUTF8String:template ] ];
	
	NSDebug(@"deleting temp file");
#	warning zombie files left in temp dir, cant find logical way to clean up
	/* unlink(template); don't delete the file - mutt probably hasn't openned it. If we can monitor vnode /access/, maybe this can be solved */
	free(template); /* deallocate the template string */
}

/* concatenates content to message */

+ (void) newMessageTo:(NSString *)to withSubject:(NSString *)subject andBody:(NSString *)body
	{
	[ self newMessageWithContent:[ NSString stringWithFormat:@"To: %@\nSubject: %@\n\n%@",[ to headerEscape], [ subject headerEscape ], body ] ];
}

/* all 3 pieces */

+ (void) newMessageWithSubject:(NSString *)subject andBody:(NSString *)body
{
	[ [ self class ] newMessageTo:@"" withSubject:subject andBody:body ];
}
+ (void) newMessageTo:(NSString *)to withBody:(NSString *)body
{
	[ [ self class ] newMessageTo:to withSubject:@"" andBody:body ];
}
+ (void) newMessageTo:(NSString *)to withSubject:(NSString *)subject
{
	[ [ self class ] newMessageTo:to withSubject:subject andBody:@"" ];
}
/* very cheap */
+ (void) newMessageWithBody:(NSString *)body
	{
	[ [ self class ] newMessageTo:@"" withBody:body ];
}
+ (void) newMessageTo:(NSString *)to
{
	[ [ self class ] newMessageTo:to withSubject:@"" ];
}
+ (void) newMessageWithSubject:(NSString *)subject
{
	[ [ self class ] newMessageTo:@"" withSubject:subject ];
}
+ (void) newMessage
{
	[ [ self class ] newMessageTo:@"" ];
}
@end
