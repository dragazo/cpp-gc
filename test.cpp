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

struct ListNode
{
	GC::ptr<ListNode> prev;
	GC::ptr<ListNode> next;

	// show a message that says we called ctor
	ListNode() { std::cerr << "i'm alive!!\n"; }

	// show a message that says we called dtor
	~ListNode() { std::cerr << "i died!!\n"; }
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
// creates a linked list that has a cycle
void foo()
{
	// create the first node
	GC::ptr<ListNode> root = GC::make<ListNode>();

	
	// we'll make 10 links in the chain
	GC::ptr<ListNode> *prev = &root;
	for (int i = 0; i < 10; ++i)
	{
		(*prev)->next = GC::make<ListNode>();
		(*prev)->next->prev = *prev;

		prev = &(*prev)->next;
	}

	// then we'll merege the ends into a cycle
	root->prev = *prev;
	(*prev)->next = root;
	
}
// the function that called foo()
void async_bar()
{
	// we need to call foo()
	foo();

	std::cerr << "\n\ncalling collect():\n\n";

	// run GC::collect in another thread while we work on other stuff
	std::thread thr(GC::collect);
	thr.detach();

	// ... do other stuff - anything not involving gc operations will not block ... //
}

int main()
{
	{
		std::thread t1([]() { while (1) foo(); });
		//std::thread t2([]() { while (1) foo(); });
		//std::thread t3([]() { while (1) foo(); });
		std::thread t4([]() { while (1) GC::collect(); });


		t1.join();
		//t2.join();
		//t3.join();
		t4.join();
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
