#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "GarbageCollection.h"

std::mutex mut;

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
	ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(10)); }

	// show a message that says we called dtor
	~ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(10)); }
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
	//for(int i = 0; i < 300; ++i)
	{
		GC::ptr<ListNode> a, b;

		a = GC::make<ListNode>();
		b = GC::make<ListNode>();

		a->next = b;
		b->next = a;
	}

	/*
	// create the first node
	GC::ptr<ListNode> root;
	root = GC::make<ListNode>();

	GC::ptr<ListNode> *prev = &root;
	{
		

		// we'll make 10 links in the chain
		
		for (int i = 0; i < 10; ++i)
		{

			auto tmp = GC::make<ListNode>();
			(*prev)->next = tmp;
			(*prev)->next->prev = *prev;

			prev = &(*prev)->next;
		}

	}
	
	// then we'll merege the ends into a cycle
	root->prev = *prev;
	(*prev)->next = root;
	*/
	//root->next = root;
	//root->prev = root;
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
		std::thread t4([]() { int i = 0; while (1) { std::cerr << "collecting pass " << ++i << '\n'; GC::collect(); } });


		t1.join();
		//t2.join();
		//t3.join();
		t4.join();
	}
	std::cin.get();
	return 0;
}
