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

void GC::__unlink(info *handle)
{
	// not using first == last for the first case because in the (illegal) case where
	// handle is not in the gc database this would unlink an unrelated object.
	if (handle == first && handle == last) first = last = nullptr;
	else if (handle == first) (first = first->next)->prev = nullptr;
	else if (handle == last) (last = last->prev)->next = nullptr;
	else
	{
		handle->prev->next = handle->next;
		handle->next->prev = handle->prev;
	}
}
void GC::__destroy(info *handle)
{
	handle->deleter(handle->obj);
	delete handle;
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
		if (ref_count == 0) __unlink(handle);
	}

	// -- make sure the mutex is unlocked before starting next step (could halt) -- //

	// if ref count fell to zero, delete the object
	if (ref_count == 0)
	{
		std::cerr << "\ngc deleting " << handle->obj << '\n';

		__destroy(handle);
	}
}

// ---------------- //

// -- collection -- //

// ---------------- //

void GC::__mark_sweep(info *handle)
{
	// mark this handle
	handle->marked = true;

	// for each outgoing arc
	for (outgoing_t outs = handle->outgoing(); outs.first != outs.second; ++outs.first)
	{
		// get the outgoing arc
		info *&arc = *(info**)((char*)handle->obj + *outs.first);

		// if it hasn't been marked, recurse to it
		if (!arc->marked) __mark_sweep(arc);
	}
}

void GC::collect()
{
	std::vector<info*> del_list; // the list of handles to delete
	
	{
		std::lock_guard<std::mutex> lock(mutex);

		// for each item in the gc database
		for (info *i = first; i != nullptr; i = i->next)
		{
			// clear its marked flag
			first->marked = false;
		}

		// for each root
		for (info **i : roots)
		{
			// perform a mark sweep from this root
			__mark_sweep(*i);
		}

		// for each item in the gc database
		for (info *i = first; i != nullptr; i = i->next)
		{
			// if it hasn't been marked
			if (!i->marked)
			{
				// unlink it and add it to the delete list
				__unlink(i);
				del_list.push_back(i);
			}
		}
	}

	// -- make sure the mutex is unlocked before starting next step (could halt) -- //

	// for each entry in the delete list
	for (info *i : del_list)
	{
		std::cerr << "\ngc deleting " << i->obj << '\n';

		// inc reference count so GC::ptr dtor won't result in another delete attempt
		++i->ref_count;

		// destroy it
		__destroy(i);
	}
}
