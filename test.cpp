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

template<typename T>
struct wrap
{
	GC::ptr<T> ptr;
};
template<typename T>
struct GC::router<wrap<T>>
{
	static void route(wrap<T> &w, router_fn fn)
	{
		GC::route(w.ptr, fn);
	}
};

struct base1
{
	int a;
};
struct base2
{
	int b;
};
struct derived : base1, base2
{

};

int main()
{
	GC::strategy(GC::strategies::manual);

	GC::ptr<int> ip = GC::make<int>(46);
	GC::ptr<int> ip_self = ip;
	std::cerr << "      int val:  " << *ip << '\n';

	GC::ptr<const int> const_ip = GC::make<const int>(47);
	std::cerr << "const int val:  " << *const_ip << '\n';

	GC::ptr<const int> const_2 = ip;
	GC::ptr<const int> const_self = const_2;
	//GC::ptr<int> non_const_ref = const_2;
	std::cerr << "const int cast: " << *const_2 << '\n';

	GC::ptr<wrap<wrap<int>>> p = GC::make<wrap<wrap<int>>>();
	p->ptr = GC::make<wrap<int>>();
	p->ptr->ptr = GC::make<int>();

	ListNode n;

	GC::ptr<derived> dp = GC::make<derived>();
	GC::ptr<base1> bp1 = GC::make<base1>();
	GC::ptr<base2> bp2 = GC::make<base2>();

	dp->a = 123;
	dp->b = 456;

	std::cerr << '\n';

	GC::ptr<base1> dp_as_b1 = dp;
	GC::ptr<base2> dp_as_b2 = dp;

	std::cerr << "a: " << dp_as_b1->a << '\n';
	std::cerr << "b: " << dp_as_b2->b << '\n';

	//GC::ptr<derived> wrong_dp = bp;

	//dp_as_b = dp;
	//dp_as_b = 67;

	/**/
	{
		std::thread t1([]() { while (1) foo(); });
		std::thread t2([]() { int i = 0; while (1) { std::cerr << "collecting pass " << ++i << '\n'; GC::collect(); } });
		
		t1.join();
		t2.join();
	}
	/**/

	std::cin.get();
	return 0;
}
