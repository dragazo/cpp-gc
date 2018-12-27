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

GC::no_addref_t GC::no_addref;

GC::bind_new_obj_t GC::bind_new_obj;

// ------------------------------------ //

// -- object database implementation -- //

// ------------------------------------ //

GC::obj_list::obj_list() : first(nullptr), last(nullptr) {}

void GC::obj_list::add(info *obj)
{
	// put it at the end of the list
	obj->prev = last;
	obj->next = nullptr;

	// link the other way as well - account for edge cases
	if (last) last = last->next = obj;
	else first = last = obj;
}
void GC::obj_list::remove(info *obj)
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

void GC::obj_list::merge(obj_list &&other)
{
	// don't do anything if other is us
	if (&other == this) return;

	// if we're empty
	if (!first)
	{
		// just take other's stuff
		first = other.first;
		last = other.last;
	}
	// otherwise if other isn't empty
	else if (other.first)
	{
		// do an actual splice
		last->next = other.first;
		other.first->prev = last;

		// repoint the last pointer
		last = other.last;
	}

	// empty other
	other.first = other.last = nullptr;
}

// ----------------------------- //

// -- collection synchronizer -- //

// ----------------------------- //

GC::obj_list GC::collection_synchronizer::objs;

std::unordered_set<GC::raw_handle_t> GC::collection_synchronizer::roots;

GC::obj_list GC::collection_synchronizer::del_list;

// ---------------------------------------------------------------------

std::mutex GC::collection_synchronizer::internal_mutex;

std::thread::id GC::collection_synchronizer::collector_thread;

GC::obj_list GC::collection_synchronizer::objs_add_cache;

std::unordered_set<GC::raw_handle_t> GC::collection_synchronizer::roots_add_cache;
std::unordered_set<GC::raw_handle_t> GC::collection_synchronizer::roots_remove_cache;

std::unordered_map<GC::raw_handle_t, GC::info*> GC::collection_synchronizer::handle_repoint_cache;

std::vector<GC::raw_handle_t> GC::collection_synchronizer::handle_dealloc_list;

// ---------------------------------------------------------------------

GC::collection_synchronizer::collection_sentry::collection_sentry()
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// if there's already a collector thread, fail
	if (collector_thread != std::thread::id()) { success = false; return; }

	// otherwise mark the calling thread as the collector thread
	collector_thread = std::this_thread::get_id();

	// apply all the obj add actions
	objs.merge(std::move(objs_add_cache));

	// apply all the scheduled root add actions
	for (auto i : roots_add_cache)
	{
		roots.insert(i);
	}
	roots_add_cache.clear();

	// apply all the scheduled root remove actions
	for (auto i : roots_remove_cache)
	{
		roots.erase(i);
	}
	roots_remove_cache.clear();

	// apply all the scheduled handle repoint actions from the cache
	for (const auto &entry : handle_repoint_cache)
	{
		*entry.first = entry.second;
	}
	handle_repoint_cache.clear();

	// delete everything in the handle dealloc list
	for (info **i : handle_dealloc_list)
	{
		delete i;
	}
	handle_dealloc_list.clear();

	success = true;
}
GC::collection_synchronizer::collection_sentry::~collection_sentry()
{
	// only do this if the construction was successful
	if (success)
	{
		// destroy objects
		for (info *handle = del_list.front(); handle; handle = handle->next)
		{
			#if GC_SHOW_DELMSG
			std::cerr << "\ngc deleting " << handle->obj << '\n';
			#endif

			handle->destroy();
		}

		// deallocate memory
		// done after calling deleters so that the deletion func can access the handles (but not objects) safely
		for (info *handle = del_list.front(), *next; handle; handle = next)
		{
			next = handle->next;
			handle->dealloc();
		}

		// clear the del list (we already deallocated the resources above)
		del_list.unsafe_clear();

		// end the collection action.
		// must be after dtors/deallocs to ensure that if they call collect() it'll no-op (otherwise very slow).
		{
			std::lock_guard<std::mutex> internal_lock(internal_mutex);

			// mark that there is no longer a collector thread
			collector_thread = std::thread::id();
		}
	}
}

bool GC::collection_synchronizer::this_is_collector_thread()
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// return true iff the calling thread is the collector thread
	return collector_thread == std::this_thread::get_id();
}

void GC::collection_synchronizer::schedule_handle_create_null(raw_handle_t &handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// create the handle
	handle = new info*(nullptr);

	// root it
	__schedule_handle_root(handle);
}
void GC::collection_synchronizer::schedule_handle_create_bind_new_obj(raw_handle_t &handle, info *new_obj)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// create the handle
	handle = new info*(new_obj);

	// root it
	__schedule_handle_root(handle);

	// add the new obj to the obj add cache
	objs_add_cache.add(new_obj);
}
void GC::collection_synchronizer::schedule_handle_create_alias(raw_handle_t &handle, raw_handle_t src_handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// create the handle
	handle = new info*(__get_current_target(src_handle));

	// root it
	__schedule_handle_root(handle);
}

void GC::collection_synchronizer::schedule_handle_destroy(raw_handle_t handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// unroot it
	__schedule_handle_unroot(handle);

	// purge it from the repoint cache
	handle_repoint_cache.erase(handle);

	// add it to the dynamic handle deletion list
	handle_dealloc_list.push_back(handle);
}

void GC::collection_synchronizer::schedule_handle_unroot(raw_handle_t handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);
	__schedule_handle_unroot(handle);
}
void GC::collection_synchronizer::unsafe_immediate_handle_unroot(raw_handle_t handle)
{
	// no need to lock because we're modifying collector-only resources and assuming we're the collector
	roots.erase(handle);
}

void GC::collection_synchronizer::__schedule_handle_root(raw_handle_t handle)
{
	// if there's no collector thread, we can apply the change immediately
	if (collector_thread == std::thread::id())
	{
		roots.insert(handle);
	}
	// otherwise we need to cache the request
	else
	{
		// add it to the root add cache
		roots_add_cache.insert(handle);
		// remove it from the root remove cache
		roots_remove_cache.erase(handle);
	}
}
void GC::collection_synchronizer::__schedule_handle_unroot(raw_handle_t handle)
{
	// if there's no collector thread, we can apply the change immediately
	if (collector_thread == std::thread::id())
	{
		roots.erase(handle);
	}
	// otherwise we need to cache the request
	else
	{
		// add it to the roots remove cache
		roots_remove_cache.insert(handle);
		// remove it from the roots add cache
		roots_add_cache.erase(handle);
	}
}

void GC::collection_synchronizer::schedule_handle_repoint(raw_handle_t handle, raw_handle_t new_value)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);
	
	// repoint handle to the target
	__raw_schedule_handle_repoint(handle, __get_current_target(new_value));
}
void GC::collection_synchronizer::schedule_handle_repoint_null(raw_handle_t handle)
{
	std::lock_guard<std::mutex> lock(internal_mutex);

	// repoint handle to null
	__raw_schedule_handle_repoint(handle, nullptr);
}
void GC::collection_synchronizer::schedule_handle_repoint_swap(raw_handle_t handle_a, raw_handle_t handle_b)
{
	std::lock_guard<std::mutex> lock(internal_mutex);

	// get their current repoint targets
	info *target_a = __get_current_target(handle_a);
	info *target_b = __get_current_target(handle_b);

	// schedule repoint actions to swap them
	__raw_schedule_handle_repoint(handle_a, target_b);
	__raw_schedule_handle_repoint(handle_b, target_a);
}

void GC::collection_synchronizer::__raw_schedule_handle_repoint(raw_handle_t handle, info *target)
{
	// if there's no collector thread, we can apply the change immediately
	if (collector_thread == std::thread::id())
	{
		// immediately repoint handle to target
		*handle = target;
	}
	// otherwise we need to cache the request
	else
	{
		// assign handle's repoint target
		handle_repoint_cache[handle] = target;
	}
}

GC::info *GC::collection_synchronizer::__get_current_target(raw_handle_t handle)
{
	// find new_value's repoint target from the cache
	auto new_value_iter = handle_repoint_cache.find(handle);

	// get the target (if it's in the repoint cache, get the repoint target, otherwise use it raw)
	return new_value_iter != handle_repoint_cache.end() ? new_value_iter->second : *handle;
}

// ---------------- //

// -- collection -- //

// ---------------- //

void GC::__mark_sweep(info *handle)
{
	// mark this handle
	handle->marked = true;

	// for each outgoing arc
	handle->route(+[](const smart_handle &arc)
	{
		// get the current arc value - this is only safe because we're in a gc cycle
		info *raw = *arc.raw_handle();

		// if it hasn't been marked, recurse to it (only if non-null)
		if (raw && !raw->marked) __mark_sweep(raw);
	});
}

void GC::collect()
{
	// begin a collection action - if that fails early exit.
	// this is in an outer scope because all mutexes must be unlocked prior to this object's destructor.
	collection_synchronizer::collection_sentry sentry;
	if (!sentry) return;

	{
		#if GC_COLLECT_MSG
		std::size_t collect_count = 0; // number of objects that we scheduled for deletion
		#endif

		// -- recalculate root claims for each object -- //

		// this can happen because e.g. a ptr<T> was added to a std::vector<ptr<T>> and thus was not unrooted by GC::make() after construction

		// for each object in the gc database
		for (info *i = sentry.get_objs().front(); i; i = i->next)
		{
			// clear its marked flag
			i->marked = false;

			// claim its children (see above comment)
			// we only need to do this for the mutable targets because the non-mutable targets are collected immediately upon the object entering gc control
			i->mutable_route(GC::router_unsafe_immediate_unroot);
		}

		// -- mark and sweep -- //

		// for each root
		for (info *const *i : sentry.get_roots())
		{
			// perform a mark sweep from this root (if it points to something)
			if (*i) __mark_sweep(*i);
		}

		// -- clean anything not marked -- //

		// for each item in the gc database
		for (info *i = sentry.get_objs().front(), *_next; i; i = _next)
		{
			// record next node
			_next = i->next;

			// if it hasn't been marked and isn't currently being deleted
			if (!i->marked && !i->destroying.test_and_set())
			{
				// mark it for deletion (internally unlinks it from obj database)
				sentry.mark_delete(i);

				#if GC_COLLECT_MSG
				++collect_count;
				#endif
			}
		}

		#if GC_COLLECT_MSG
		std::cerr << "collecting - deleting: " << collect_count << '\n';
		#endif
	}
}

// ------------------------------ //

// -- utility router functions -- //

// ------------------------------ //

void GC::router_unroot(const smart_handle &arc)
{
	collection_synchronizer::schedule_handle_unroot(arc.raw_handle());
}
void GC::router_unsafe_immediate_unroot(const smart_handle &arc)
{
	collection_synchronizer::unsafe_immediate_handle_unroot(arc.raw_handle());
}

// --------------------- //

// -- auto collection -- //

// --------------------- //

GC::strategies GC::strategy() { return _strategy; }
void GC::strategy(strategies new_strategy) { _strategy = new_strategy; }

GC::sleep_time_t GC::sleep_time() { return _sleep_time; }
void GC::sleep_time(sleep_time_t new_sleep_time) { _sleep_time = new_sleep_time; }

void GC::start_timed_collect()
{
	static std::thread *thread = new std::thread(__timed_collect_func);
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
