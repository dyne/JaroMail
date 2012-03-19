/* High-level abstraction of the mutt mailer
 * Implemented using AppleScripts and Terminal.app */


#import <Foundation/Foundation.h>

@interface Mutt : NSObject
/* Currently we cannot abstract the instance of the object into an instance of mutt.
 * Hence all the methods are class methods, which return no value, and start a new mutt in a new terminal window */
+ (void)newMessageWithContent:(NSString *)content;												// create a message from a string, should be RFC822
+ (void)newMessageFromFile:(NSString *)file;													// create a message from a file, should be RFC822
+ (void)newMessageTo:(NSString *)to withSubject:(NSString *)subject andBody:(NSString *)body;   // create a message with a subject, a recipient and a body as strings
+ (void)newMessageTo:(NSString *)to withSubject:(NSString *)subject;							// various permutions.. blech.
+ (void)newMessageTo:(NSString *)to withBody:(NSString *)body;
+ (void)newMessageTo:(NSString *)to;
+ (void)newMessageWithSubject:(NSString *)subject andBody:(NSString *)body;
+ (void)newMessageWithSubject:(NSString *)subject;
+ (void)newMessageWithBody:(NSString *)body;
+ (void)newMessage;																				// create an empty message
@end