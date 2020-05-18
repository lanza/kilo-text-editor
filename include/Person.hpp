#pragma once
#include <iostream>
#include <string>
#include <vector>

class Person {
  std::string m_name;
  int m_age;

  std::vector<Person> m_dogs;

public:
  Person(std::string const &name, int age) : m_name{name}, m_age{age} {}
  int GetAge() const { return m_age; }
  std::string const &GetName() const { return m_name; }

  void CreateDog(std::string name, int age);
  void PrintDogs() const;
};

std::ostream &operator<<(std::ostream &o, Person const &p);
