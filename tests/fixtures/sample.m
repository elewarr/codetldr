// Sample Objective-C module for Tree-sitter testing.
#import <Foundation/Foundation.h>

@protocol Greeter
- (NSString *)greet:(NSString *)name;
@end

NSString* greet(NSString* name) {
    return [NSString stringWithFormat:@"Hello, %@!", name];
}

NSString* farewell(NSString* name) {
    return [NSString stringWithFormat:@"Goodbye, %@!", name];
}

@interface Calculator : NSObject <Greeter>
- (NSInteger)add:(NSInteger)a to:(NSInteger)b;
- (NSInteger)multiply:(NSInteger)a by:(NSInteger)b;
@end

@implementation Calculator

- (NSString *)greet:(NSString *)name {
    return [NSString stringWithFormat:@"Calculator says hello, %@!", name];
}

- (NSInteger)add:(NSInteger)a to:(NSInteger)b {
    return a + b;
}

- (NSInteger)multiply:(NSInteger)a by:(NSInteger)b {
    return a * b;
}

@end

int main(void) {
    @autoreleasepool {
        NSString* msg = greet(@"world");
        NSString* bye = farewell(@"world");
        Calculator* calc = [[Calculator alloc] init];
        NSInteger result = [calc add:2 to:3];
        NSInteger product = [calc multiply:result by:2];
        NSLog(@"%@", msg);
    }
    return 0;
}
