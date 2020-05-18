#include <gtest/gtest.h>
#include <dbg.h>
#include <Person.hpp>

TEST(TestPerson, SaysHello) {
  Person p{"Muffin", 15};
  dbg(p);
  ASSERT_EQ(p.GetAge(), 15);
  ASSERT_EQ(p.GetName(), "Muffin");
}
