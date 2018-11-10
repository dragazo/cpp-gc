#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "GarbageCollection.h"

struct ptr_set
{
	GC::ptr<int> a, b, c, d, e, f, g, h;
};
template<> struct GC::router<ptr_set>
{
	static void route(ptr_set &set, router_fn func)
	{
		GC::route(set.a, func);
		GC::route(set.b, func);
		GC::route(set.c, func);
		GC::route(set.d, func);
		GC::route(set.e, func);
		GC::route(set.f, func);
		GC::route(set.g, func);
		GC::route(set.h, func);
	}
};

struct ListNode
{
	GC::ptr<ListNode> prev;
	GC::ptr<ListNode> next;

	ptr_set set;

	// show a message that says we called ctor
	ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(2)); }

	// show a message that says we called dtor
	~ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(2)); }
};
template<> struct GC::router<ListNode>
{
	static void route(ListNode &node, router_fn func)
	{
		GC::route(node.prev, func);
		GC::route(node.next, func);

		GC::route(node.set, func);
	}
};

// creates a linked list that has a cycle
void foo()
{
	{
		//GC::collect();
		
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

		//GC::collect();
		//std::cerr << "\n\n";
	}
}

int main()
{
	GC::set_strategy(GC::strategy::manual);

	GC::make<int>();

	{
		std::thread t1([]() { while (1) foo(); });
		std::thread t2([]() { int i = 0; while (1) { std::cerr << "collecting pass " << ++i << '\n'; GC::collect(); } });
		
		t1.join();
		t2.join();
	}

	std::cin.get();
	return 0;
}
