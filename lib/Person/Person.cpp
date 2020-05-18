#include <Person.hpp>

void Person::CreateDog(std::string name, int age) {
  m_dogs.emplace_back(name, age);
}

void Person::PrintDogs() const {
  std::cout << m_name << ":\n";
  for (auto const &dog : m_dogs)
    std::cout << "    " << dog.GetName() << ":" << dog.GetAge() << '\n';
}

std::ostream &operator<<(std::ostream &o, Person const &p) {
  return o << p.GetName() << " : " << p.GetAge();
}
