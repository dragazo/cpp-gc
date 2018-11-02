#include <iostream>
#include <string>

#include "GarbageCollection.h"

struct thingy
{
	int *a;
	int *b;
};

constexpr std::size_t ptr_offsets[] =
{
	offsetof(thingy, a),
	offsetof(thingy, b),
};

struct base1 { virtual ~base1() = default; };
struct base2 { virtual ~base2() = default; };
struct derived : base1, base2 {};

GC::outgoing_t get_ptr_offsets() { return {std::begin(ptr_offsets), std::end(ptr_offsets)}; }

constexpr auto oa = offsetof(thingy, a);
constexpr auto ob = offsetof(thingy, b);

int main()
{
	GC::ptr<int> i3;
	{
		GC::ptr<int> i = GC::make<int>(69);
		GC::ptr<std::string> str = GC::make<std::string>("hello world!! ayeee lmao :3");

		GC::ptr<int> i2 = i;

		GC::ptr<int> i4;

		i2 = i;
		i3 = i2;

		//i3.reset();

		//i3 = nullptr;

		if (i3 == i2) std::cout << "same obj\n\n";
		else std::cout << "different obj\n\n";

		if (i) std::cout << "val: " << *i << '\n'; else std::cout << "BAD!!\n";
		if (str) std::cout << "msg: " << *str << '\n'; else std::cout << "BAD!!\n";

		//gc_ptr<int> i2 = i;
	}
	/*
	gc_info *gc_i = nullptr, *gc_s = nullptr;
	gc_register(gc_i);
	gc_register(gc_s);

	gc_i = gc_create(new int(69), [](void *ptr) { delete (int*)ptr; }, nullptr);
	gc_s = gc_create(new std::string("hello world!! lmao :3"), [](void *ptr) { delete (std::string*)ptr; }, nullptr);

	std::cout << "val: " << *(int*)gc_i->obj << '\n';
	std::cout << "msg: " << *(std::string*)gc_s->obj << '\n';

	int i;
	thingy t;

	derived d;
	base1 &b1 = d;
	base2 &b2 = d;

	#define check(i) std::cerr << "orig: " << (i) << " -- root: " << get_polymorphic_root(i) << '\n';

	check(&i);
	check(&t);
	check(&std::cin);
	check(&d);
	check(&b1);
	check(&b2);

	gc_delref(gc_s);
	gc_delref(gc_i);
	*/
	std::cin.get();
	return 0;
}
