#include <iostream>
#include <string>

#include "GarbageCollection.h"

struct data
{
	~data() { std::cerr << "data deleted\n"; }
};

struct fancy
{
private:
	GC::ptr<data> ptr;

	friend GC::outgoing_t fancy_outgoing();

public:
	fancy() : ptr(GC::make<data>()) {}

	data *get_data() { return ptr.get(); }
};

GC::outgoing_t fancy_outgoing()
{
	std::cerr << "custom outgoing\n";

	static const std::size_t offsets[] = {offsetof(fancy, ptr)};

	return {std::begin(offsets), std::end(offsets)};
}

template<>
GC::outgoing_t GC::outgoing<fancy>()
{
	return fancy_outgoing();
}

int main()
{
	{
		GC::ptr<fancy> ptr = GC::make<fancy>();
		
	}
	
	std::cin.get();
	return 0;
}
