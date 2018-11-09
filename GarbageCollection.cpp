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

// -------------- //

// -- settings -- //

// -------------- //

// if nonzero, displays a message on cerr that an object was added to gc database (+ its address)
#define GC_SHOW_CREATMSG 0

// if nonzero, displays a message on cerr that an object was deleted (+ its address)
#define GC_SHOW_DELMSG 0

// if nonzero, displays info messages on cerr during GC::collect()
#define GC_COLLECT_MSG 1

// ---------- //

// -- data -- //

// ---------- //

std::mutex GC::mutex;

GC::info *GC::first = nullptr;
GC::info *GC::last = nullptr;

std::unordered_set<GC::info**> GC::roots;

std::unordered_set<GC::info*> GC::del_list;

// --------------- //

// -- interface -- //

// --------------- //

void GC::__root(info *&handle) { roots.insert(&handle); }
void GC::__unroot(info *&handle) { roots.erase(&handle); }

GC::info *GC::__create(void *obj, void(*deleter)(void*), void(*router)(void *, router_fn))
{
	// create a gc entry for it
	info *entry = std::make_unique<info>(obj, deleter, router, 1, last, nullptr).release();

	// put it in the database
	if (last) last = last->next = entry;
	else first = last = entry;

	#if GC_SHOW_CREATMSG
	std::cerr << "gc created " << obj << '\n';
	#endif

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

void GC::__addref(info *handle) { ++handle->ref_count; }
bool GC::__delref(info *handle)
{
	// dec ref count - if ref count is now zero and it's not already being destroyed, destroy it
	if (--handle->ref_count == 0 && !handle->destroying)
	{
		handle->destroying = true;

		// unlink it and add it to delete list
		__unlink(handle);
		del_list.insert(handle);

		return true;
	}

	return false;
}

void GC::handle_del_list()
{
	// make del_list a local object
	decltype(del_list) del_list_cpy;
	{
		std::lock_guard<std::mutex> lock(mutex);
		del_list_cpy = std::move(del_list);
		del_list.clear(); // move assign doesn't guarantee del_list will be empty
	}

	// -- make sure we're not locked at this point - could block -- //

	// destroy each entry
	for (info *handle : del_list_cpy)
	{
		#if GC_SHOW_DELMSG
		std::cerr << "\ngc deleting " << handle->obj << '\n';
		#endif

		handle->deleter(handle->obj);
	}

	// delete the handles
	// this is done after calling all deleters so that the deletion func can access the handles safely
	for (info *handle : del_list_cpy)
	{
		delete handle;
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
	handle->router(handle->obj, [](info *&arc)
	{
		// if it hasn't been marked, recurse to it (only if non-null)
		if (arc && !arc->marked) __mark_sweep(arc);
	});
}

void GC::collect()
{
	#if GC_COLLECT_MSG
	std::size_t collect_count = 0; // number of objects that we scheduled for deletion
	#endif

	{
		std::lock_guard<std::mutex> lock(mutex);

		#if GC_COLLECT_MSG
		std::cerr << "collecting - current roots: " << roots.size() << '\n';
		#endif

		// for each item in the gc database
		for (info *i = first; i; i = i->next)
		{
			// clear its marked flag
			i->marked = false;
		}

		// for each root
		for (info **i : roots)
		{
			// perform a mark sweep from this root (if it points to something)
			if (*i) __mark_sweep(*i);
		}

		// for each item in the gc database
		for (info *i = first, *_next; i; i = _next)
		{
			// record next node
			_next = i->next;

			// if it hasn't been marked and isn't currently being deleted
			if (!i->marked && !i->destroying)
			{
				i->destroying = true;

				// unlink it and add it to the delete list
				__unlink(i);
				del_list.insert(i);

				#if GC_COLLECT_MSG
				++collect_count;
				#endif
			}
		}

		#if GC_COLLECT_MSG
		std::cerr << "collecting - deleting: " << collect_count << '\n';
		#endif
	}

	// -- make sure the mutex is unlocked before starting next step (could halt) -- //

	// handle the del list
	handle_del_list();
}
