#include <iostream>
#include <string>

#include "GarbageCollection.h"

struct fancy
{
	GC::ptr<fancy> ptr;
};

GC::outgoing_t fancy_outgoing()
{
	std::cerr << "custom outgoing\n";

	static const std::size_t offsets[] = {offsetof(fancy, ptr)};
	return {std::begin(offsets), std::end(offsets)};
}
template<> GC::outgoing_t GC::outgoing<fancy>() { return fancy_outgoing(); }

int main()
{
	{
		GC::ptr<fancy> ptr = GC::make<fancy>();
		ptr->ptr = ptr;
	}
	
	std::cerr << "\n\nrunning collect:\n\n";

	GC::collect();

	std::cin.get();
	return 0;
}
