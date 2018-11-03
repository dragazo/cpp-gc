#include <iostream>
#include <string>

#include "GarbageCollection.h"

struct Person
{
	std::string name;
	int age;

	GC::ptr<Person> best_friend;
};
template<> struct GC::outgoing<Person>
{
	static GC::outgoing_t get()
	{
		const std::size_t offs[] = {offsetof(Person, best_friend)};
		return {std::begin(offs), std::end(offs)};
	}
};

int main()
{
	{
		GC::ptr<Person> sally = GC::make<Person>();
		sally->name = "Sally Sallison";
		sally->age = 16;

		GC::ptr<Person> veronica = GC::make<Person>();
		veronica->name = "Veronica Vallian";
		veronica->age = 15;

		sally->best_friend = veronica;
		veronica->best_friend = sally;
	}
	std::cerr << "\n\nrunning collect:\n\n";

	GC::collect();

	std::cin.get();
	return 0;
}
