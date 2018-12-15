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

GC::objs_database GC::objs;
GC::roots_database GC::roots;

GC::del_list_database GC::del_list;

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

// ------------------------------------ //

// -- object database implementation -- //

// ------------------------------------ //

void GC::objs_database::add(info *obj)
{
	std::lock_guard<std::mutex> lock(this->mutex);
	this->__add(obj);
}
void GC::objs_database::remove(info *obj)
{
	std::lock_guard<std::mutex> lock(this->mutex);
	this->__remove(obj);
}

void GC::objs_database::__add(info *obj)
{
	// put it at the end of the list
	obj->prev = last;
	obj->next = nullptr;

	// link the other way as well - account for edge cases
	if (last) last = last->next = obj;
	else first = last = obj;
}
void GC::objs_database::__remove(info *obj)
{
	// not using first == last for the first case because in the (illegal) case where
	// handle is not in the gc database this would unlink an unrelated object.
	if (obj == first && obj == last) first = last = nullptr;
	else if (obj == first) (first = first->next)->prev = nullptr;
	else if (obj == last) (last = last->prev)->next = nullptr;
	else
	{
		obj->prev->next = obj->next;
		obj->next->prev = obj->prev;
	}
}

void GC::objs_database::addref(info *obj)
{
	std::lock_guard<std::mutex> lock(this->mutex);
	this->__addref(obj);
}
bool GC::objs_database::delref(info *obj)
{
	std::lock_guard<std::mutex> lock(this->mutex);
	return this->__delref(obj);
}

void GC::objs_database::__addref(info *obj)
{
	++obj->ref_count;
}
bool GC::objs_database::__delref(info *obj)
{
	// dec ref count - if ref count is now zero and it's not already being destroyed, destroy it
	if (--obj->ref_count == 0 && !obj->destroying)
	{
		obj->destroying = true;

		// unlink it and add it to delete list
		objs.__remove(obj);
		del_list.add(obj);

		return true;
	}

	return false;
}

// ----------------------------------- //

// -- roots database implementation -- //

// ----------------------------------- //

void GC::roots_database::add(info *const &root)
{
	std::lock_guard<std::mutex> lock(this->mutex);
	this->contents.insert(&root);
}
void GC::roots_database::remove(info *const &root)
{
	std::lock_guard<std::mutex> lock(this->mutex);
	this->contents.erase(&root);
}

// -------------------------------------- //

// -- del list database implementation -- //

// -------------------------------------- //

void GC::del_list_database::add(info *obj)
{
	std::lock_guard<std::mutex> lock(this->mutex);
	contents.push_back(obj);
}

GC::del_list_database::container_t GC::del_list_database::fetch_all()
{
	std::lock_guard<std::mutex> lock(this->mutex);
	
	// get the current contents (soon to be old contents)
	container_t old_contents = std::move(contents);
	contents.clear(); // clear current contents just to be safe (not all containers do it for us on move)
	
	return old_contents;
}

// --------------- //

// -- interface -- //

// --------------- //

void GC::handle_del_list()
{
	// fetch the del list contents (internally empties the database)
	del_list_database::container_t del_list_cpy = del_list.fetch_all();

	// -- make sure we're not locked at this point - could block -- //

	// destroy each entry
	for (info *handle : del_list_cpy)
	{
		#if GC_SHOW_DELMSG
		std::cerr << "\ngc deleting " << handle->obj << '\n';
		#endif

		handle->destroy();
	}

	// delete the handles
	// this is done after calling all deleters so that the deletion func can access the handles (but not objects) safely
	for (info *handle : del_list_cpy)
	{
		handle->dealloc();
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
	handle->route(+[](info *const &arc)
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
		std::lock_guard<std::mutex> lock(objs.mutex);

		// -- recalculate root claims for each object -- //

		// this can happen because e.g. a ptr<T> was added to a std::vector<ptr<T>> and thus was not unrooted by GC::make() after construction

		#if GC_COLLECT_MSG
		std::cerr << "collecting - current roots: " << roots.size() << " -> ";
		#endif
		
		// for each object in hte gc database
		for (info *i = objs.first; i; i = i->next)
		{
			// clear its marked flag
			i->marked = false;

			// claim its children (see above comment)
			// we only need to do this for the mutable targets because the non-mutable targets are collected immediately upon the object entering gc control
			i->mutable_route(GC::router_unroot);
		}

		// -- mark and sweep -- //

		#if GC_COLLECT_MSG
		std::cerr << roots.size() << '\n';
		#endif

		{
			std::lock_guard<std::mutex> lock(roots.mutex);

			// for each root
			for (info *const *i : roots.contents)
			{
				// perform a mark sweep from this root (if it points to something)
				if (*i) __mark_sweep(*i);
			}
		}

		// -- clean anything not marked -- //

		// for each item in the gc database
		for (info *i = objs.first, *_next; i; i = _next)
		{
			// record next node
			_next = i->next;

			// if it hasn't been marked and isn't currently being deleted
			if (!i->marked && !i->destroying)
			{
				i->destroying = true;

				// unlink it and add it to the delete list
				objs.__remove(i);
				del_list.add(i);

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

// ------------------------------ //

// -- utility router functions -- //

// ------------------------------ //

void GC::router_unroot(info *const &arc)
{
	roots.remove(arc);
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

			// if we're using timed strategy
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
