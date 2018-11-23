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

std::unordered_set<GC::info *const *> GC::roots;

std::vector<GC::info*> GC::del_list;

// ---------------------------------------

std::atomic<GC::strategies> GC::_strategy(GC::strategies::timed | GC::strategies::allocfail);

std::atomic<GC::sleep_time_t> GC::_sleep_time(std::chrono::milliseconds(60000));

// ---------- //

// -- misc -- //

// ---------- //

void *GC::aligned_malloc(std::size_t size, std::size_t align)
{
	// calling with 0 yields nullptr
	if (size == 0) return nullptr;

	// allocate enough space for a void*, padding, and the array
	size += sizeof(void*) + align - 1;

	// grab that much space - if that fails, return null
	void *raw = std::malloc(size);
	if (!raw) return nullptr;

	// get the pointer to return (before alignment)
	void *ret = (char*)raw + sizeof(void*);

	// align the return pointer
	ret = (char*)ret + (-(std::intptr_t)ret & (align - 1));

	// store the raw pointer before start of ret array
	*(void**)((char*)ret - sizeof(void*)) = raw;

	// return ret pointer
	return ret;
}
void GC::aligned_free(void *ptr)
{
	// free the raw pointer (freeing nullptr does nothing)
	if (ptr) std::free(*(void**)((char*)ptr - sizeof(void*)));
}

// --------------- //

// -- interface -- //

// --------------- //

void GC::__root(info *const &handle) { roots.insert(&handle); }
void GC::__unroot(info *const &handle) { roots.erase(&handle); }

void GC::__link(info *handle)
{
	// -- put it at the end of the list -- //

	handle->prev = last;
	handle->next = nullptr;

	if (last) last = last->next = handle;
	else first = last = handle;
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
		del_list.push_back(handle);

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

		handle->dtor(handle->obj, handle->count);
	}

	// delete the handles
	// this is done after calling all deleters so that the deletion func can access the handles safely
	for (info *handle : del_list_cpy)
	{
		handle->dealloc(handle->obj);
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
	handle->router(handle->obj, handle->count, +[](info *const &arc)
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

		// -- recalculate root claims for each object -- //

		// this can happen because e.g. a ptr<T> was added to a std::vector<ptr<T>> and thus was not unrooted by GC::make() after construction

		#if GC_COLLECT_MSG
		std::cerr << "collecting - current roots: " << roots.size() << " -> ";
		#endif
		
		// for each object in hte gc database
		for (info *i = first; i; i = i->next)
		{
			// clear its marked flag
			i->marked = false;

			// claim its children (see above comment)
			i->router(i->obj, i->count, GC::__unroot);
		}

		// -- mark and sweep -- //

		#if GC_COLLECT_MSG
		std::cerr << roots.size() << '\n';
		#endif

		// for each root
		for (info *const *i : roots)
		{
			// perform a mark sweep from this root (if it points to something)
			if (*i) __mark_sweep(*i);
		}

		// -- clean anything not marked -- //

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
				del_list.push_back(i);

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

// --------------------- //

// -- auto collection -- //

// --------------------- //

GC::strategies GC::strategy() { return _strategy; }
void GC::strategy(strategies new_strategy) { _strategy = new_strategy; }

GC::sleep_time_t GC::sleep_time() { return _sleep_time; }
void GC::sleep_time(sleep_time_t new_sleep_time) { _sleep_time = new_sleep_time; }

void GC::__start_timed_collect()
{
	// pointer to the thread
	static std::thread *thread = nullptr;

	// if it's null, create it
	if (!thread) thread = new std::thread(__timed_collect_func);
}

// ---------------------------------------------------

void GC::__timed_collect_func()
{
	// try the operation
	try
	{
		// we'll run forever
		while (true)
		{
			// sleep the sleep time
			std::this_thread::sleep_for(sleep_time());

			// if we're using sleep strategy
			if ((int)strategy() & (int)strategies::timed)
			{
				// run a collect pass
				collect();
			}
		}
	}
	// if we ever hit an error, something terrible happened
	catch (...)
	{
		// print error message and terminate with a nonzero code
		std::cerr << "CRITICAL ERROR: garbage collection threw an exception\n";
		std::exit(1);
	}
}
