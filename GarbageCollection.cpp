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
	++obj->ref_count;
}
void GC::objs_database::delref(info *obj)
{
	// dec ref count - if ref count is now zero and it's not already being destroyed, destroy it
	if (--obj->ref_count == 0 && !obj->destroying.test_and_set())
	{
		// add obj to the del list
		GC::del_list.add(obj);

		// if we're the collector thread
		if (GC::collection_deadlock_protector::this_is_collector_thread())
		{
			// the (synchronous) call to GC::collect() will handle del list, so we don't need to do anything else
		}
		// otherwise we're not the collector thread
		else
		{
			// in this case it's fine for us to block on the call to handle del list
			//GC::del_list.handle_all();
		}
	}
}

// ----------------------------------- //

// -- roots database implementation -- //

// ----------------------------------- //

void GC::roots_database::add(const std::atomic<info*> &root)
{
	std::lock_guard<std::mutex> lock(this->queues_mutex);
	
	remove_queue.erase(&root);
	add_queue.insert(&root);
}
void GC::roots_database::remove(const std::atomic<info*> &root)
{
	std::lock_guard<std::mutex> lock(this->queues_mutex);
	
	add_queue.erase(&root);
	remove_queue.insert(&root);
}

void GC::roots_database::__update_content()
{
	std::lock_guard<std::mutex> add_queue_lock(queues_mutex);

	// we assume we have a lock on the content mutex

	// perform all the queued add operations - then empty the container
	for (const auto &i : add_queue) content.insert(i);
	add_queue.clear();

	// perform all the queued remove operations - then empty the container
	for (const auto &i : remove_queue) content.erase(i);
	remove_queue.clear();
}

// -------------------------------------- //

// -- del list database implementation -- //

// -------------------------------------- //

void GC::del_list_database::add(info *obj)
{
	std::lock_guard<std::mutex> lock(this->mutex);
	contents.push_back(obj);
}
void GC::del_list_database::__add(info *obj)
{
	contents.push_back(obj);
}

void GC::del_list_database::handle_all()
{
	// extract all the del list contents before beginning
	// this is so calls to del list functions during this action won't deadlock
	decltype(this->contents) contents_cpy;
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		contents_cpy = std::move(this->contents);
		this->contents.clear(); // clear the contents just to be sure (not all std containers do this on move)
	}

	// for each delete entry
	for (info *handle : contents_cpy)
	{
		#if GC_SHOW_DELMSG
		std::cerr << "\ngc deleting " << handle->obj << '\n';
		#endif

		// unlink it
		objs.remove(handle);

		// destroy it
		if (!handle->destroy_completed) handle->destroy();
	}

	// delete the handles
	// this is done after calling all deleters so that the deletion func can access the handles (but not objects) safely
	for (info *handle : contents_cpy)
	{
		handle->dealloc();
	}
}

// ----------------------------------- //

// -- collection deadlock protector -- //

// ----------------------------------- //

std::mutex GC::collection_deadlock_protector::internal_mutex;

std::thread::id GC::collection_deadlock_protector::collector_thread;

bool GC::collection_deadlock_protector::begin_collection()
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// if there's already a collector thread, fail
	if (collector_thread != std::thread::id()) return false;

	// otherwise mark the calling thread as the collector thread
	collector_thread = std::this_thread::get_id();

	return true;
}

void GC::collection_deadlock_protector::end_collection()
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// mark that there is no longer a collector thread
	collector_thread = std::thread::id();
}

bool GC::collection_deadlock_protector::this_is_collector_thread()
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// return true iff the calling thread is the collector thread
	return collector_thread == std::this_thread::get_id();
}

// ---------------- //

// -- collection -- //

// ---------------- //

void GC::__mark_sweep(info *handle)
{
	// mark this handle
	handle->marked = true;

	// for each outgoing arc
	handle->route(+[](const std::atomic<info*> &arc)
	{
		info *raw = arc.load();

		// if it hasn't been marked, recurse to it (only if non-null)
		if (raw && !raw->marked) __mark_sweep(arc);
	});
}

void GC::collect()
{
	{
		// begin a collection action - if that fails early exit.
		// the logic behind this is that several back-to-back garbage collections would be pointless, so we can skip overlapping ones.
		if (!GC::collection_deadlock_protector::begin_collection()) return;
		// also make a sentry object to end the collection action in case we forget or somehow trigger an exception
		struct collection_ender_t
		{
			~collection_ender_t() { GC::collection_deadlock_protector::end_collection(); }
		} collection_ender;

		// we'll hold a long-running lock on the objects database - it would be very bad if someone changed it at any point in this process
		std::lock_guard<std::mutex> obj_lock(objs.mutex);

		// -------------------------------------------------------------------

		#if GC_COLLECT_MSG
		std::size_t collect_count = 0; // number of objects that we scheduled for deletion
		#endif

		// -- recalculate root claims for each object -- //

		// this can happen because e.g. a ptr<T> was added to a std::vector<ptr<T>> and thus was not unrooted by GC::make() after construction

		// for each object in the gc database
		for (info *i = objs.first; i; i = i->next)
		{
			// clear its marked flag
			i->marked = false;

			// claim its children (see above comment)
			// we only need to do this for the mutable targets because the non-mutable targets are collected immediately upon the object entering gc control
			i->mutable_route(GC::router_unroot);
		}

		// -- mark and sweep -- //

		{
			// we're about to use the roots content set, so we need to lock its mutex
			std::lock_guard<std::mutex> root_lock(roots.content_mutex);

			// now we need to update the roots content set to apply the changes
			roots.__update_content();

			// for each root
			for (const std::atomic<info*> *i : roots.content)
			{
				info *raw = i->load();

				// perform a mark sweep from this root (if it points to something)
				if (raw) __mark_sweep(raw);
			}
		}

		// -- clean anything not marked -- //

		// for each item in the gc database
		for (info *i = objs.first, *_next; i; i = _next)
		{
			// record next node
			_next = i->next;

			// if it hasn't been marked and isn't currently being deleted
			if (!i->marked && !i->destroying.test_and_set())
			{
				// add it to the delete list
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

	// -- make sure the obj mutex is unlocked before starting next step (could halt) -- //

	// handle the delete list
	GC::del_list.handle_all();
}

// ------------------------------ //

// -- utility router functions -- //

// ------------------------------ //

void GC::router_unroot(const std::atomic<info*> &arc)
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
