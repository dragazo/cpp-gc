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

void GC::__root(info *&handle) { roots.insert(&handle); }
void GC::root(info *&handle)
{
	std::lock_guard<std::mutex> lock(mutex);
	roots.insert(&handle);
}

void GC::__unroot(info *&handle) { roots.erase(&handle); }
void GC::unroot(info *&handle)
{
	std::lock_guard<std::mutex> lock(mutex);
	roots.erase(&handle);
}

GC::info *GC::__create(void *obj, void(*deleter)(void*), outgoing_t(*outgoing)())
{
	// create a gc entry for it
	info *entry = new info(obj, deleter, outgoing, 1, last, nullptr);

	// put it in the database
	if (last) last = last->next = entry;
	else first = last = entry;

	// return the gc alloc entry
	return entry;
}
GC::info *GC::create(void *obj, void(*deleter)(void*), outgoing_t(*outgoing)())
{
	std::lock_guard<std::mutex> lock(mutex);
	return __create(obj, deleter, outgoing);	
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

void GC::__addref(info *handle) { ++handle->ref_count; }
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

		// if ref count is now zero, we should destroy it
		if (ref_count == 0)
		{
			// if it's already scheduled for destruction, stop
			if (handle->destroying.test_and_set()) return;

			// unlink it from the gc database
			__unlink(handle);
		}
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

		// if it hasn't been marked, recurse to it (only if non-null)
		if (arc && !arc->marked) __mark_sweep(arc);
	}
}

void GC::collect()
{
	std::cerr << "roots: " << roots.size() << '\n';

	std::vector<info*> del_list; // the list of handles to delete
	
	{
		std::lock_guard<std::mutex> lock(mutex);

		// for each item in the gc database
		for (info *i = first; i; i = i->next)
		{
			// clear its marked flag
			first->marked = false;
		}

		// for each root
		for (info **i : roots)
		{
			// perform a mark sweep from this root (if it points to something)
			if (*i) __mark_sweep(*i);
		}

		// for each item in the gc database
		for (info *i = first; i; i = i->next)
		{
			// if it hasn't been marked and isn't currently being deleted
			if (!i->marked && !i->destroying.test_and_set())
			{
				// unlink it and add it to the delete list
				__unlink(i);
				del_list.push_back(i);

				// inc reference count so delref() won't result in another delete attempt
				++i->ref_count;
			}
		}
	}

	// -- make sure the mutex is unlocked before starting next step (could halt) -- //

	// for each entry in the delete list
	for (info *i : del_list)
	{
		std::cerr << "\ngc deleting " << i->obj << '\n';

		// destroy it
		__destroy(i);
	}
}
