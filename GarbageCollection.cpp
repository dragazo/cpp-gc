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

// -------------------------------- //

// -- info handle implementation -- //

// -------------------------------- //

GC::smart_handle::smart_handle(std::nullptr_t) : handle(new info*)
{
	collection_synchronizer::schedule_handle_create_null(*handle);
}
GC::smart_handle::smart_handle(info *init, bind_new_obj_t) : handle(new info*)
{
	collection_synchronizer::schedule_handle_create_bind_new_obj(*handle, init);
}

GC::smart_handle::~smart_handle()
{
	collection_synchronizer::schedule_handle_destroy(*handle);
}

GC::smart_handle::smart_handle(const smart_handle &other) : handle(new info*)
{
	collection_synchronizer::schedule_handle_create_alias(*handle, *other.handle);
}
GC::smart_handle &GC::smart_handle::operator=(const smart_handle &other)
{
	reset(other);
	return *this;
}

void GC::smart_handle::reset(const smart_handle &new_value)
{
	collection_synchronizer::schedule_handle_repoint(*handle, new_value.handle);
}
void GC::smart_handle::reset()
{
	collection_synchronizer::schedule_handle_repoint(*handle, nullptr);
}

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
	}

	// empty other
	other.first = other.last = nullptr;
}

// ----------------------------- //

// -- collection synchronizer -- //

// ----------------------------- //

GC::obj_list GC::collection_synchronizer::objs;

std::unordered_set<GC::info *const*> GC::collection_synchronizer::roots;

std::vector<GC::info*> GC::collection_synchronizer::del_list;

// ---------------------------------------------------------------------

std::mutex GC::collection_synchronizer::internal_mutex;

std::thread::id GC::collection_synchronizer::collector_thread;

GC::obj_list GC::collection_synchronizer::objs_add_cache;

std::unordered_set<GC::info *const*> GC::collection_synchronizer::roots_add_cache;
std::unordered_set<GC::info *const*> GC::collection_synchronizer::roots_remove_cache;

std::unordered_map<GC::info**, GC::info*> GC::collection_synchronizer::handle_repoint_cache;

std::vector<GC::info**> GC::collection_synchronizer::handle_dealloc_list;

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
		for (info *handle : del_list)
		{
			#if GC_SHOW_DELMSG
			std::cerr << "\ngc deleting " << handle->obj << '\n';
			#endif

			handle->destroy();
		}

		// deallocate memory
		// done after calling deleters so that the deletion func can access the handles (but not objects) safely
		for (info *handle : del_list)
		{
			handle->dealloc();
		}

		// clear the del list
		del_list.clear();

		// end the collection action
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

void GC::collection_synchronizer::schedule_handle_create_null(info *&raw_handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// root the handle
	__schedule_handle_root(raw_handle);

	// since we're creating the handle, it's not already in the handle database
	// thus we can assign the value immediately

	// repoint it to null
	raw_handle = nullptr;
}
void GC::collection_synchronizer::schedule_handle_create_bind_new_obj(info *&raw_handle, info *new_obj)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// root the handle
	__schedule_handle_root(raw_handle);

	// since we're creating the handle, it's not already in the handle database
	// thus we can assign the value immediately

	// repoint it to the new obj
	raw_handle = new_obj;

	// add the new obj to the obj add cache
	objs_add_cache.add(new_obj);
}
void GC::collection_synchronizer::schedule_handle_create_alias(info *&raw_handle, info *&src_handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// root the handle
	__schedule_handle_root(raw_handle);

	// since we're creating the handle, it's not already in the handle database
	// thus we can assign the value immediately

	// repoint it to the current target of src_handle
	raw_handle = __get_current_target(&src_handle);
}

void GC::collection_synchronizer::schedule_handle_destroy(info *&raw_handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// unroot it
	__schedule_handle_unroot(raw_handle);

	// purge it from the repoint cache
	handle_repoint_cache.erase(&raw_handle);

	// add it to the dynamic handle deletion list
	handle_dealloc_list.push_back(&raw_handle);
}

void GC::collection_synchronizer::schedule_handle_unroot(info *const &raw_handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);
	__schedule_handle_unroot(raw_handle);
}

void GC::collection_synchronizer::__schedule_handle_root(info *&raw_handle)
{
	// add it to the root add cache
	roots_add_cache.insert(&raw_handle);
	// remove it from the root remove cache
	roots_remove_cache.erase(&raw_handle);
}
void GC::collection_synchronizer::__schedule_handle_unroot(info *const &raw_handle)
{
	// add it to the roots remove cache
	roots_remove_cache.insert(&raw_handle);
	// remove it from the roots add cache
	roots_add_cache.erase(&raw_handle);
}

void GC::collection_synchronizer::schedule_handle_repoint(info *&raw_handle, info *const *new_value)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);
	__schedule_handle_repoint(raw_handle, new_value);
}
void GC::collection_synchronizer::__schedule_handle_repoint(info *&raw_handle, info *const *new_value)
{
	// assign this as raw_handle's repoint target in the cache
	handle_repoint_cache[&raw_handle] = __get_current_target(new_value);
}

GC::info *GC::collection_synchronizer::get_current_target(info *const *new_value)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);
	return __get_current_target(new_value);
}
GC::info *GC::collection_synchronizer::__get_current_target(info *const *new_value)
{
	// if new_value is non-null, we need to look it up
	if (new_value)
	{
		// find new_value's repoint target from the cache
		auto new_value_iter = handle_repoint_cache.find(const_cast<info**>(new_value)); // the const cast is safe because we won't be modifying it - just for compiler's sake

		// get the target (if it's in the repoint cache, get the repoint target, otherwise use it raw)
		return new_value_iter != handle_repoint_cache.end() ? new_value_iter->second : *new_value;
	}
	// otherwise the target is null
	else return nullptr;
}

// ---------------- //

// -- collection -- //

// ---------------- //

void GC::__mark_sweep(info *handle)
{
	// mark this handle
	handle->marked = true;

	std::cerr << "sweeping " << handle << '\n';

	// for each outgoing arc
	handle->route(+[](const smart_handle &arc)
	{
		// get the current arc value - this is only safe because we're in a gc cycle
		info *raw = arc.raw_handle();

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
			i->mutable_route(GC::router_unroot);
		}

		// -- mark and sweep -- //

		std::cerr << "beginning sweep\n";

		// for each root
		for (info *const *i : sentry.get_roots())
		{
			std::cerr << "root sweep: " << i << '\n';

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
