#include <Foundation/Foundation.h>

@interface Person : NSObject
@property(copy, nonatomic) NSString *name;
@property int age;

- (void) printName;
@end

@implementation Person
- (void) printName {
  NSLog(@"%@ is 32", self.name);
}
@end

int main() {
  int i = 4;
  printf("%d\n", i);
  for (int j = 0; j < 10; ++j) {
    printf("%d\n", j);
  }

  return i;
}
