#include <Foundation/Foundation.h>

@interface Person : NSObject
@property(copy, nonatomic) NSString *name;
@property int age;

- (void) printName;
@end

@implementation Person
- (void) printName {
  NSLog(@"%@", self.name);
}
@end

int main() {

}
