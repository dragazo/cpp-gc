#include <iostream>
#include <string>
#include <thread>

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
		static const std::size_t offs[] = {offsetof(Person, best_friend)};
		return {std::begin(offs), std::end(offs)};
	}
};

int _id = 0;

struct ListNode
{
	const int id;

	GC::ptr<ListNode> prev;
	GC::ptr<ListNode> next;

	// show a message that says we called ctor
	ListNode() : id(_id++) { std::cerr << id << " - i'm alive!!\n"; }

	// show a message that says we called dtor
	~ListNode() { std::cerr << id << " - i died!!\n"; }
};
template<>
struct GC::outgoing<ListNode>
{
	static GC::outgoing_t get()
	{
		static const std::size_t offsets[] = {offsetof(ListNode, prev), offsetof(ListNode, next)};
		return {std::begin(offsets), std::end(offsets)};
	}
};

void foo()
{
	// create the first node
	GC::ptr<ListNode> root = GC::make<ListNode>();

	// we'll make 100 links in the chain
	GC::ptr<ListNode> *prev = &root;
	for (int i = 0; i < 10; ++i)
	{
		(*prev)->next = GC::make<ListNode>();
		(*prev)->next->prev = *prev;

		prev = &(*prev)->next;
	}

	// then we'll merege the ends into a cycle
	//root->prev = *prev;
	//(*prev)->next = root;
}
// the function that called foo()
void bar()
{
	// we need to call foo()
	foo();

	std::cerr << "\ncalling collect():\n\n";

	// but you know what, just to be safe, let's clean up any objects it left lying around unused
	GC::collect();
}



int main()
{
	{
		bar();
	}
	std::cin.get();

	{
		std::thread t1([]()
		{
			while (true)
			{
				GC::ptr<Person> p = GC::make<Person>();
				p->name = "sally";
				p->age = 17;

				std::cerr << p->name << " - " << p->age << '\n';
			}
		});
		std::thread t2([]()
		{
			while (true)
			{
				GC::collect();
			}
		});

		t1.join();
		t2.join();
	}

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
