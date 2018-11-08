#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "GarbageCollection.h"

struct ListNode
{
	GC::ptr<ListNode> prev;
	GC::ptr<ListNode> next;

	// show a message that says we called ctor
	ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(2)); }

	// show a message that says we called dtor
	~ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(2)); }
};
template<>
struct GC::router<ListNode>
{
	static void route(void *obj, router_fn func)
	{
		ListNode &node = *(ListNode*)obj;

		func(&node.prev);
		func(&node.next);
	}
};

// creates a linked list that has a cycle
void foo()
{
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
}

int main()
{
	{
		std::thread t1([]() { while (1) foo(); });
		std::thread t2([]() { int i = 0; while (1) { std::cerr << "collecting pass " << ++i << '\n'; GC::collect(); } });
		
		t1.join();
		t2.join();
	}

	std::cin.get();
	return 0;
}
