#include <iostream>
#include <cstdlib>
#include <utility>
#include <memory>
#include <mutex>
#include <list>
#include <vector>
#include <algorithm>
#include <unordered_set>

#include "GarbageCollection.h"

// ---------- //

// -- data -- //

// ---------- //

std::mutex GC::mutex;

GC::info *GC::first = nullptr;
GC::info *GC::last = nullptr;

std::unordered_set<GC::info**> GC::roots;

// --------------- //

// -- interface -- //

// --------------- //

void GC::root(info *&handle)
{
	std::lock_guard<std::mutex> lock(mutex);
	roots.insert(&handle);
}
void GC::unroot(info *&handle)
{
	std::lock_guard<std::mutex> lock(mutex);
	roots.erase(&handle);
}

GC::info *GC::create(void *obj, void(*deleter)(void*), outgoing_t(*outgoing)())
{
	std::lock_guard<std::mutex> lock(mutex);
	
	// create a gc entry for it
	info *entry = new info(obj, deleter, outgoing, 1, last, nullptr);

	// put it in the database
	if (last) last = last->next = entry;
	else first = last = entry;

	// return the gc alloc entry
	return entry;
}

void GC::addref(info *handle)
{
	std::lock_guard<std::mutex> lock(mutex);
	++handle->ref_count;
}
void GC::delref(info *handle)
{
	std::size_t ref_count;

	{
		std::lock_guard<std::mutex> lock(mutex);

		// dec ref count and store result
		ref_count = --handle->ref_count;

		// if ref count is now zero, remove it from the gc database
		if (ref_count)
		{
			// remove it from the gc database
			if (handle == first && handle == last) first = last = nullptr;
			else if (handle == first) (first = first->next)->prev = nullptr;
			else if (handle == last) (last = last->prev)->next = nullptr;
			else
			{
				handle->prev->next = handle->next;
				handle->next->prev = handle->prev;
			}
		}
	}

	// -- make sure the mutex is unlocked before starting next step (could halt) -- //

	// if ref count fell to zero, delete the object
	if (ref_count == 0)
	{
		std::cerr << "\ngc deleting " << handle->obj << '\n';

		// call its deleter for the stored object
		handle->deleter(handle->obj);

		// delete the info object itself
		delete handle;
	}
}

void GC::collect()
{
	//std::lock_guard<std::mutex> lock(gc_mutex);
	
}
