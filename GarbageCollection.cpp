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

// ------------------------------------------------------------- //

// -- dev build settings - you probably want all of these off -- //

// ------------------------------------------------------------- //

// iff nonzero, prints log messages for disjunction handle activity
#define DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_LOGGING 0

// iff nonzero, does extra und safety checks for atomic disjuction handle internals
#define DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_DATA_UND_SAFETY 1

// if nonzero, displays a message on cerr that an object was added to gc database (+ its address)
#define DRAGAZO_GARBAGE_COLLECT_SHOW_CREATMSG 0

// if nonzero, displays a message on cerr that an object was deleted (+ its address)
#define DRAGAZO_GARBAGE_COLLECT_SHOW_DELMSG 0

// if nonzero, displays info messages on cerr during GC::collect()
#define DRAGAZO_GARBAGE_COLLECT_MSG 0

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

bool GC::obj_list::contains(info *obj) const noexcept
{
	for (info *i = first; i; i = i->next)
		if (i == obj) return true;

	return false;
}

// --------------- //

// -- gc module -- //

// --------------- //

GC::disjoint_module::~disjoint_module()
{
	// getting here means there's no longer any owning handles for this disjoint module.
	// that means we're about to release control of all our stuff - the only thing that can leak is objects.
	// however we can't perform a collection because this thread no longer as living handle to this module.
	// so all we can do is enforce the fact that there should not be any memory leaks.

	// if we still have objects, bad news - the user probably violated a disjunction barrier
	if (!objs.empty())
	{
		std::cerr << "\n\nYOU MADE A USAGE VIOLATION!!\ndestruction of a disjoint gc module had leftover objects\n\n";
		std::cerr << objs.front() << ' ' << objs.front()->next << '\n' << roots.size() << '\n';
		std::abort();
	}
	// same thing for roots - less important cause this can't leak, but we don't want dangling pointers floating around out there.
	if (!roots.empty())
	{
		std::cerr << "\n\nYOU MADE A USAGE VIOLATION!!\ndestruction of a disjoint gc module had leftover roots\n\n";
		std::cerr << roots.size() << '\n' << *roots.begin() << '\n';
		std::abort();
	}
}

void GC::info::mark_sweep()
{
	// mark this handle
	this->marked = true;

	// for each outgoing arc
	this->route(+[](const smart_handle &arc)
	{
		// get the current arc value - this is only safe because we're in a collect action
		info *raw = arc.raw_handle();

		// if it hasn't been marked, recurse to it (only if non-null)
		if (raw && !raw->marked) raw->mark_sweep();
	});
}

bool GC::disjoint_module::collect()
{
	// -- begin the collection action -- //

	{
		std::lock_guard<std::mutex> internal_lock(internal_mutex);

		// if there are 1 or more ignore sentries acting on this module, do nothing and return true.
		// true is to prevent a deadlock case where we do a blocking collection is made to an ignoring module.
		if (ignore_collect_count > 0) return true;

		// if there's already a collection in progress for this module, we do nothing.
		// if the collector is us, return true - this is to prevent a deadlock case where we do a blocking collection from a router/destructor.
		// otherwise return false - someone else is doing something.
		if (collector_thread != std::thread::id()) return collector_thread == std::this_thread::get_id();

		// otherwise mark the calling thread as the collector thread
		collector_thread = std::this_thread::get_id();

		// begin caching ref count deletion events
		cache_ref_count_del_actions = true;

		// since we just came out of no-collect phase, all the caches should be empty
		assert(objs_add_cache.empty());

		assert(roots_add_cache.empty());
		assert(roots_remove_cache.empty());

		assert(handle_repoint_cache.empty());

		// ref count del cache should also be empty
		assert(ref_count_del_cache.empty());

		// the del list should also be empty
		assert(del_list.empty());
	}

	// -- initialize the collection data -- //

	// we've now started the collection action, so we have lock-free access to collector-only resources.

	// to ensure all unused objects are deleted in one pass, we need to unroot all mutables arcs.
	// this requires going through all the obj list entities, so we might as well clear their marks.

	// for each object we'll examine
	for (info *i = objs.front(); i; i = i->next)
	{
		// clear the marked flag
		i->marked = false;

		// route to mutable arcs and directly unroot from the collector-only root set.
		// this is only safe because we're guaranteed to be the (only) collector at this point.
		i->mutable_route(+[](const smart_handle &arc) { local()->roots.erase(&arc.raw_handle()); });
	}

	// clear the root objs set
	root_objs.clear();

	{
		std::lock_guard<std::mutex> lock(internal_mutex);

		// at this point we've directly unrooted all the mutables in the obj list.
		// however, other threads could have destroyed rooted handles.
		// this means the roots set at this point may contain dangling pointers.
		// thus, we need to apply the cached root/unroot actions.
		// this will ensure there are no dangling pointers that are represented.
		
		// the reason we can't just do the cached unroot actions is because it could exclude live objects.
		// for instance, suppose we have a pre-existing dynamic rooted handle A.
		// now, say a new dynamic rooted handle B is created and initialized to A, then A is destroyed.
		// thus, A was unrooted, but the object it refered to is still reachable through B.
		// this could happen e.g. on a std::vector<GC::ptr<T>> reallocation.
		// this of course could only happen for live objects, so we want to make sure we keep them that way.
		// thus we account for both root and unroot cached actions.

		// for much the same reasons, we need to apply the handle repoint cache.
		// and, because we now have roots that may point to objects in the obj add cache, we need to add those as well.
		// however, we need to clear their marks first.
		// we can't perform the routing step for them because we need the mutex to be locked during this process.
		// but that's not a problem as collect() only guarantees it will collect all objects unreachable prior to invocation.
		// the routing logic is just to ensure this happens in 1 pass and not 2 (uncommon but otherwise possible without this step).

		// so long story short we need to apply all the caches (aside from obj deletion), plus a tiny bit of extra logic.
		// we know this is safe because it's as if we took the graph snapshot later on and just routed to a subset of it for unrooting.

		// apply the obj add cache - also clear their marks (the ones in the obj list are already cleared)
		for (info *i : objs_add_cache)
		{
			i->marked = false;
			objs.add(i);
		}
		objs_add_cache.clear();

		// apply cached root actions
		for (auto i : roots_add_cache) roots.insert(i);
		roots_add_cache.clear();

		// apply cached unroot actions
		for (auto i : roots_remove_cache) roots.erase(i);
		roots_remove_cache.clear();

		// apply handle repoint actions
		for (auto i : handle_repoint_cache) *i.first = i.second;
		handle_repoint_cache.clear();

		// now that that's all done...

		// add the pointed-at objects of all remaining (valid) roots to a set of root objects.
		// we only include the non-null targets for convenience.
		for (auto root : roots)
			if (*root) root_objs.insert(*root);
	}

	// -----------------------------------------------------------

	#if DRAGAZO_GARBAGE_COLLECT_MSG
	std::size_t collect_count = 0; // number of objects that we scheduled for deletion
	#endif

	// -- mark and sweep -- //

	// perform a mark sweep from each root object
	for (info *i : root_objs) i->mark_sweep();

	// -- clean anything not marked -- //

	// for each item in the gc database
	for (info *i = objs.front(), *next; i; i = next)
	{
		next = i->next;

		// if it hasn't been marked, mark it for deletion
		if (!i->marked)
		{
			// mark it for deletion
			objs.remove(i);
			del_list.add(i);

			#if DRAGAZO_GARBAGE_COLLECT_MSG
			++collect_count;
			#endif
		}
	}

	#if DRAGAZO_GARBAGE_COLLECT_MSG
	std::cerr << "collecting - deleting: " << collect_count << '\n';
	#endif

	// -----------------------------------------------------------

	// we've now divided the old obj list into two partitions:
	// the reachable objects are still in the obj list.
	// the unreachable objects are now in the del list.
	// ref count deletion caching is still in effect.

	// destroy unreachable objects
	for (info *i = del_list.front(); i; i = i->next) i->destroy();

	// now we've destroyed the unreachable objects but there may be cached deletions from ref count logic.
	// we'll now resume immediate ref count deletions.
	// we can't resume immediate ref count deletions prior to destroying the unreachable objs because it could double delete.
	// e.g. could drop an unreachable obj ref count to 0 and insta-delete on its own before we get to it.
	// even if we made a check for double delete, it would still deallocate the info object as well and would cause even more headache.
	// we know this usage is safe because there's no way a reachable object could ref count delete an unreachable object.
		
	// resume immediate ref count deletions.
	{
		std::lock_guard<std::mutex> internal_lock(internal_mutex);

		// stop caching ref count deletion actions (i.e. resume immediate ref count deletions)
		cache_ref_count_del_actions = false;

		// if an unreachable object is in the ref count del cache purge it (to avoid the double delete issue for unreachable objs).
		// we know this'll work because the unreachable objects are unreachable from reachable objects (hence the name).
		// thus, since we already called unreachable destructors, there will be no further ref count logic for unreachables.

		// purge unreachable objects from the ref count del cache (to avoid double deletions - see above).
		for (info *i = del_list.front(); i; i = i->next) ref_count_del_cache.erase(i);

		// after the double-deletion purge, remove remaining ref count del cache objects from the obj list.
		// we do this now because enabling immediate ref count del logic means the obj list can be modified by any holder of the mutex.
		for (auto i : ref_count_del_cache) objs.remove(i);
	}

	// we now have lock-free exclusive ownership of the ref count del cache.

	// deallocate memory
	// done after calling ALL unreachable dtors so that the dtors can access the info objects safely.
	// this is because we might be deleting objects whose reference count is not zero.
	// which means they could potentially hold live gc references to other objects in del list and try to refer to their info objects.
	for (info *i = del_list.front(), *next; i; i = next)
	{
		next = i->next; // dealloc() will deallocate the info object as well, so grab the next object now
		i->dealloc();
	}

	// clear the del list (we already deallocated the resources above)
	del_list.unsafe_clear();
	assert(del_list.empty()); // just to make sure

	// remember, we still have lock-free exclusive ownership of the ref count del cache from above.
	// process the cached ref count deletions.
	for (auto i : ref_count_del_cache)
	{
		// we don't need to do all destructors before all deallocators like we did above.
		// this is because we know the ref count for these is zero (because they were cached ref count deletion).
		// this means we don't have the same risks as above (i.e. live references being forcibly severed).

		i->destroy();
		i->dealloc();
	}
	ref_count_del_cache.clear();

	// end the collection action
	// must be after dtors/deallocs to ensure that if they call collect() it'll no-op (otherwise very slow).
	// additionally, must be after those to ensure the caches are fully emptied as the last atomic step.
	// also, if this came before dtors, the reference count system could fall to 0 and result in double dtor.
	{
		std::lock_guard<std::mutex> internal_lock(internal_mutex);

		// mark that there is no longer a collector thread
		collector_thread = std::thread::id();

		// apply all the cached obj add actions that occurred during the collection action
		for (auto i : objs_add_cache) objs.add(i);
		objs_add_cache.clear();

		// apply cached root actions
		for (auto i : roots_add_cache) roots.insert(i);
		roots_add_cache.clear();

		// apply cached unroot actions
		for (auto i : roots_remove_cache) roots.erase(i);
		roots_remove_cache.clear();

		// apply all the cached handle repoint actions
		for (auto i : handle_repoint_cache) *i.first = i.second;
		handle_repoint_cache.clear();
	}

	// return that we did the collection
	return true;
}
void GC::disjoint_module::blocking_collect()
{
	while (!collect());
}

bool GC::disjoint_module::this_is_collector_thread()
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);
	return collector_thread == std::this_thread::get_id();
}

void GC::disjoint_module::schedule_handle_create_null(smart_handle &handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// point it at null
	handle.raw = nullptr;
	
	// root it
	__schedule_handle_root(handle);
}
void GC::disjoint_module::schedule_handle_create_bind_new_obj(smart_handle &handle, info *new_obj)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// point it at the object
	handle.raw = new_obj;

	// root it
	__schedule_handle_root(handle);

	// -- add the object -- //

	// set its reference count to 1
	new_obj->ref_count = 1;

	// if there's no collector thread, we MUST apply the change immediately
	if (collector_thread == std::thread::id())
	{
		// if this branch was selected, the caches should be empty
		assert(objs_add_cache.empty());

		objs.add(new_obj);
	}
	// otherwise we need to cache the request
	else objs_add_cache.insert(new_obj);
}
void GC::disjoint_module::schedule_handle_create_alias(smart_handle &handle, const smart_handle &src_handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// get the target
	info *target = __get_current_target(src_handle);

	#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_SAFETY_CHECKS

	// if we're going to repoint outside the disjunction of the handle, that's a disjunction violation
	if (target && handle.disjunction != target->disjunction)
	{
		throw GC::disjunction_error("attempt to repoint GC::ptr outside of the current disjunction");
	}

	#endif

	// point it at the source handle's current target
	handle.raw = target;

	// increment the target reference count
	if (handle.raw) ++handle.raw->ref_count;

	// root it
	__schedule_handle_root(handle);
}

void GC::disjoint_module::schedule_handle_destroy(const smart_handle &handle)
{
	std::unique_lock<std::mutex> internal_lock(internal_mutex);

	// get the old target
	info *old_target = __get_current_target(handle);

	// unroot the handle
	__schedule_handle_unroot(handle);

	// purge the handle from the repoint cache so we don't dereference undefined memory.
	// the const cast is ok because we won't be modifying it - just for lookup.
	handle_repoint_cache.erase(const_cast<info**>(&handle.raw));

	// dec the reference count
	__MUST_BE_LAST_ref_count_dec(old_target, std::move(internal_lock));
}

void GC::disjoint_module::schedule_handle_unroot(const smart_handle &handle)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);
	
	// unroot it
	__schedule_handle_unroot(handle);
}

void GC::disjoint_module::schedule_handle_repoint_null(smart_handle &handle)
{
	std::unique_lock<std::mutex> internal_lock(internal_mutex);

	// get the old target
	info *old_target = __get_current_target(handle);

	// repoint handle to null
	__raw_schedule_handle_repoint(handle, nullptr);

	// decrement old target reference count
	__MUST_BE_LAST_ref_count_dec(old_target, std::move(internal_lock));
}
void GC::disjoint_module::schedule_handle_repoint(smart_handle &handle, const smart_handle &new_value)
{
	std::unique_lock<std::mutex> internal_lock(internal_mutex);
	
	// get the old/new targets
	info *old_target = __get_current_target(handle);
	info *new_target = __get_current_target(new_value);

	#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_SAFETY_CHECKS

	// if we're going to repoint outside the disjunction of the handle, that's a disjunction violation
	if (new_target && handle.disjunction != new_target->disjunction)
	{
		throw GC::disjunction_error("attempt to repoint GC::ptr outside of the current disjunction");
	}

	#endif

	// only do the remaining logic if it's an actual repoint
	if (old_target != new_target)
	{
		// repoint handle to the new target
		__raw_schedule_handle_repoint(handle, new_target);

		// increment new target reference count
		if (new_target) ++new_target->ref_count;

		// decrement old target reference count
		__MUST_BE_LAST_ref_count_dec(old_target, std::move(internal_lock));
	}
}
void GC::disjoint_module::schedule_handle_repoint_swap(smart_handle &handle_a, smart_handle &handle_b)
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);

	// get their current repoint targets
	info *target_a = __get_current_target(handle_a);
	info *target_b = __get_current_target(handle_b);

	#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_SAFETY_CHECKS

	// if we're going to repoint outside the disjunction of either handle, that's a disjunction violation
	if (target_b && handle_a.disjunction != target_b->disjunction || target_a && handle_b.disjunction != target_a->disjunction)
	{
		throw GC::disjunction_error("attempt to repoint GC::ptr outside of the current disjunction");
	}

	#endif

	// only perform the swap if they point to different things
	if (target_a != target_b)
	{
		// schedule repoint actions to swap them
		__raw_schedule_handle_repoint(handle_a, target_b);
		__raw_schedule_handle_repoint(handle_b, target_a);

		// there's no need for reference counting logic in a swap operation
	}
}

std::size_t GC::disjoint_module::begin_ignore_collect()
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);
	return ignore_collect_count++;
}
void GC::disjoint_module::end_ignore_collect()
{
	std::lock_guard<std::mutex> internal_lock(internal_mutex);
	assert(ignore_collect_count != 0);
	--ignore_collect_count;
}

void GC::disjoint_module::__schedule_handle_root(const smart_handle &handle)
{
	// if there's no collector thread, we MUST apply the change immediately
	if (collector_thread == std::thread::id())
	{
		// if this branch was selected, the caches should be empty
		assert(roots_add_cache.empty());
		assert(roots_remove_cache.empty());

		roots.insert(&handle.raw_handle());
	}
	// otherwise we need to cache the request
	else
	{
		roots_add_cache.insert(&handle.raw);
		roots_remove_cache.erase(&handle.raw); // ensure the sets are disjoint
	}
}
void GC::disjoint_module::__schedule_handle_unroot(const smart_handle &handle)
{
	// if there's no collector thread, we MUST apply the change immediately
	if (collector_thread == std::thread::id())
	{
		// if this branch was selected, the caches should be empty
		assert(roots_add_cache.empty());
		assert(roots_remove_cache.empty());

		roots.erase(&handle.raw);
	}
	// otherwise we need to cache the request
	else
	{
		roots_remove_cache.insert(&handle.raw);
		roots_add_cache.erase(&handle.raw); // ensure the sets are disjoint
	}
}

void GC::disjoint_module::__raw_schedule_handle_repoint(smart_handle &handle, info *target)
{
	// if there's no collector thread, we MUST apply the change immediately
	if (collector_thread == std::thread::id())
	{
		// if this branch was selected, the caches should be empty
		assert(handle_repoint_cache.empty());

		// immediately repoint handle to target
		handle.raw = target;
	}
	// otherwise we need to cache the request
	else handle_repoint_cache[&handle.raw] = target;
}

GC::info *GC::disjoint_module::__get_current_target(const smart_handle &handle)
{
	// find new_value's repoint target from the cache.
	// const cast is safe because we won't be modifying it (just for lookup in the repoint cache).
	auto new_value_iter = handle_repoint_cache.find(const_cast<info**>(&handle.raw));

	// get the target - if it's in the repoint cache, get the repoint target, otherwise use it raw.
	// this works regardless of if we're in a collect action or not (if we're in a collect action the cache is empty).
	return new_value_iter != handle_repoint_cache.end() ? new_value_iter->second : handle.raw;
}

void GC::disjoint_module::__MUST_BE_LAST_ref_count_dec(info *target, std::unique_lock<std::mutex> internal_lock)
{
	// decrement the reference count
	// if it falls to zero we need to perform ref count deletion logic
	if (target && --target->ref_count == 0)
	{
		// if it's in the obj add cache we can delete it immediately regardless of what's going on.
		// this is because it being in the obj add cache means it's not in the obj list, and is thus not under gc consideration.
		if (objs_add_cache.find(target) != objs_add_cache.end())
		{
			// remove it from the obj add cache
			objs_add_cache.erase(target);

			// unlock the mutex so we can call arbitrary code
			internal_lock.unlock();

			target->destroy();
			target->dealloc();
		}
		// otherwise we know it exists and isn't in the add cache, therefore it's in the obj list.
		// if we're not suppoed to cache ref count deletions, handle it immediately
		else if (!cache_ref_count_del_actions)
		{
			// remove it from the obj list
			objs.remove(target);
			
			// unlock the mutex so we can call arbitrary code
			internal_lock.unlock();

			target->destroy();
			target->dealloc();
		}
		// otherwise we're supposed to cache the ref count deletion action.
		// this also implies we're in a collection action.
		else
		{
			assert(collector_thread != std::thread::id());

			ref_count_del_cache.insert(target);
		}
	}
}

// ------------------------------- //

// -- special disjunction stuff -- //

// ------------------------------- //

GC::primary_disjunction_t GC::primary_disjunction;
GC::inherit_disjunction_t GC::inherit_disjunction;
GC::new_disjunction_t GC::new_disjunction;

GC::disjoint_module *GC::disjoint_module::local_detour = nullptr;

const GC::shared_disjoint_handle &GC::disjoint_module::primary_handle()
{
	// not thread_local because the primary disjunction must exist for the entire program runtime.
	// the default value creates that actual collection module.
	static struct primary_handle_t
	{
		shared_disjoint_handle m;

		#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_LOGGING
		struct _
		{
			_() { std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ctor primary handle\n"; }
			~_() { std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! dtor primary handle\n"; }
		} __;
		#endif

		primary_handle_t()
		{
			disjoint_module_container::get().create_new_disjunction(m);
		}

		~primary_handle_t()
		{
			// because this happens at static dtor time, all thread_local objects have been destroyed already - including the local handle.
			// thus accesses to the local handle will result in und memory accesses.
			// set up the local detour to bypass the local handle and instead go to the primary module - which is still alive.
			local_detour = m.get();

			#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_LOGGING
			std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! primary mid dtor - roots: " << m->roots.size() << '\n';
			#endif
		}
	} primary_handle;

	#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_LOGGING
	std::cerr << "                                !!!! primary handle access\n";
	#endif

	return primary_handle.m;
}
GC::shared_disjoint_handle &GC::disjoint_module::local_handle()
{
	// thread_local because this is a thread-specific owning handle.
	// the default value points the current thread to use the primary disjunction.
	thread_local struct local_handle_t
	{
		shared_disjoint_handle m = primary_handle();

		#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_LOGGING
		struct _
		{
			_() { std::cerr << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ ctor local handle " << std::this_thread::get_id() << '\n'; }
			~_() { std::cerr << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ dtor local handle " << std::this_thread::get_id() << '\n'; }
		} __;
		#endif
	} local_handle;

	#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_LOGGING
	std::cerr << "                                ~~~~ local handle access " << std::this_thread::get_id() << '\n';
	#endif

	// if local detour is non-null it means we're in the static dtors (from primary() dtor handler), which means the thread_local handle has already been destroyed.
	// this would be und access of a destroyed object, but would theoretically only happen if a static dtor tried to make a thread for some reason.
	assert(local_detour == nullptr);

	return local_handle.m;
}

GC::disjoint_module *GC::disjoint_module::primary()
{
	return primary_handle().get();
}
GC::disjoint_module *GC::disjoint_module::local()
{
	// get the local detour
	disjoint_module *detour = local_detour;

	// if we're taking a detour, use that, otherwise the handle is alive and we should read that instead
	return detour ? detour : local_handle().get();
}

void GC::disjoint_module_container::create_new_disjunction(shared_disjoint_handle &dest)
{
	// create a new disjoint module handle data block
	auto m = std::make_unique<handle_data>();
	// and construct its module in-place
	new (m->get()) disjoint_module;

	// repoint dest to the new disjunction
	dest.reset(m.release());

	// add it to the disjunction database
	{
		std::lock_guard<std::mutex> internal_lock(internal_mutex);

		// if we're not collecting, we need to put it immediately in the disjunction list
		if (!collecting) disjunctions.emplace_back(dest);
		// otherwise we need to cache the add action
		else disjunction_add_cache.emplace_back(dest);
	}
}

void GC::disjoint_module_container::BACKGROUND_COLLECTOR_ONLY___collect(bool collect)
{
	{
		std::lock_guard<std::mutex> internal_lock(internal_mutex);

		// we should not already be collecting (this is background collector only)
		assert(!collecting);

		// enter collecting mode
		collecting = true;

		// the add cache should be empty (just came out of a non-collecting phase)
		assert(disjunction_add_cache.empty());
	}

	// if performing a real collection
	if (collect)
	{
		// for each stored disjunction:
		// (the EXPLICIT std::list::iterator ensures it is indeed a LIST).
		// (otherwise we need to constantly update the end iterator after each erasure).
		for (auto i = disjunctions.begin(), end = disjunctions.end(); i != end; )
		{
			// lock and use this disjunction as the local disjunction - then get the raw handle
			disjoint_module *const raw_handle = (disjoint_module::local_handle() = *i).get();

			// if it's still allive, perform a collection on it
			if (raw_handle)
			{
				// perform the collection
				raw_handle->collect();
				++i;

				// afterwards unlink the handle - we don't want to keep them alive longer than they need to be
				disjoint_module::local_handle() = nullptr;
			}
			// otherwise it's invalid (dangling) - erase it
			else i = disjunctions.erase(i);
		}
	}
	// otherwise just performing a cull
	else
	{
		// for each stored disjunction:
		// (the EXPLICIT std::list::iterator ensures it is indeed a LIST).
		// (otherwise we need to constantly update the end iterator after each erasure).
		for (auto i = disjunctions.begin(), end = disjunctions.end(); i != end; )
		{
			// if this is a dangling pointer, erase it
			if (i->expired()) i = disjunctions.erase(i);
			else ++i;
		}
	}

	{
		std::lock_guard<std::mutex> internal_lock(internal_mutex);

		// exit collecting mode
		collecting = false;

		// apply all the cached disjunction insertions
		disjunctions.splice(disjunctions.begin(), disjunction_add_cache);
		disjunction_add_cache.clear(); // just to be sure
	}
}

// ----------------------------------- //

// -- disjunction handle data stuff -- //

// ----------------------------------- //

GC::handle_data::tag_t GC::handle_data::tag_add(tag_t v, std::memory_order order)
{
	const auto prev = tag.fetch_add(v, order);

	#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_DATA_UND_SAFETY

	const auto cur = prev + v; // compute current value from previous

	// make sure we didn't overflow any of the fields
	assert((cur & strong_mask) >= (prev & strong_mask));
	assert((cur & weak_mask) >= (prev & weak_mask));
	assert((cur & lock_mask) >= (prev & lock_mask));

	#endif

	return prev;
}
GC::handle_data::tag_t GC::handle_data::tag_sub(tag_t v, std::memory_order order)
{
	const auto prev = tag.fetch_sub(v, order);

	#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_DATA_UND_SAFETY

	const auto cur = prev - v; // compute current value from previous

	// make sure we didn't overflow any of the fields
	assert((cur & strong_mask) <= (prev & strong_mask));
	assert((cur & weak_mask) <= (prev & weak_mask));
	assert((cur & lock_mask) <= (prev & lock_mask));

	#endif

	return prev;
}

// ------------------------------- //

// -- shared disjunction handle -- //

// ------------------------------- //

void GC::shared_disjoint_handle::reset(handle_data *other)
{
	// handle redundant assignment as no-op
	if (data == other) return;

	// if we pointed at something
	if (data)
	{
		// drop a strong reference and get the previous tag
		auto prev = data->tag_sub(handle_data::strong_1, std::memory_order_acq_rel);

		// if we were the last strong reference, there are no longer any strong references - destroy the object
		// we include the lock strong refs because those locks succeeded - i.e. our very existence as a non-lock strong owner proves those locks succeeded.
		if ((prev & handle_data::strong_mask) == handle_data::strong_1)
		{
			__module->blocking_collect(); // perform one final collection to make sure everything's collected
			__module->~disjoint_module(); // then destroy the module itself - its dtor asserts that all objects were collected

			// if there were also no more weak references, delete the handle
			if ((prev & handle_data::weak_mask) == 0) delete data;
			// otherwise we're not deleting it but the weak ref count might have fallen to zero in the meantime and could be waiting - alert that we're done destroying
			else data->destroyed_flag.store(true, std::memory_order_release);
		}
	}

	// repoint and test for null - must come after destruction logic to ensure the collection can potentially alias this handle
	data = other;
	if (data)
	{
		__module = data->get(); // cache the module pointer
		data->tag_add(handle_data::strong_1, std::memory_order_acq_rel); // bump up the strong ref count
	}
	else __module = nullptr; // cache the module pointer (in this case null)
}

void GC::shared_disjoint_handle::lock(handle_data *other)
{
	// we need to start out null
	reset();

	// if the relock target is non-null
	if (other)
	{
		// bump up the strong and lock counts
		auto prev = other->tag_add(handle_data::lock_1 | handle_data::strong_1, std::memory_order_acq_rel);

		// if there was at least 1 non-lock strong reference, the lock is successful and the object is still alive.
		// we exclude lock strong refs because otherwise 2 locks back-to-back from 2 threads could trick the latter into thinking it succeeded.
		if (handle_data::non_lock_strongs(prev) != 0)
		{
			// unmark the lock ref (success keeps only the strong ref because we're a non-lock strong owner type)
			other->tag_sub(handle_data::lock_1, std::memory_order_acq_rel);

			// we now own a reference - do the raw repoint
			data = other;
			__module = other->get();
		}
		// otherwise the object is expired
		else
		{
			// unmark the lock ref and strong ref (failure to lock a strong ref)
			other->tag_sub(handle_data::lock_1 | handle_data::strong_1, std::memory_order_acq_rel);
		}
	}
}

GC::shared_disjoint_handle::shared_disjoint_handle(std::nullptr_t) noexcept : __module(nullptr), data(nullptr) {}
GC::shared_disjoint_handle::~shared_disjoint_handle() { reset(); }

GC::shared_disjoint_handle::shared_disjoint_handle(const shared_disjoint_handle &other)
{
	// alias the same disjunction
	data = other.data;
	__module = other.__module;

	// bump up the strong ref count to account for our aliasing
	if (data) data->tag_add(handle_data::strong_1, std::memory_order_acq_rel);
}
GC::shared_disjoint_handle::shared_disjoint_handle(shared_disjoint_handle &&other) noexcept
{
	// steal disjunction ownership - ref count change is net zero
	data = std::exchange(other.data, nullptr);
	__module = std::exchange(other.__module, nullptr);
}

GC::shared_disjoint_handle &GC::shared_disjoint_handle::operator=(const shared_disjoint_handle &other)
{
	reset(other.data); // internally performs self-assignment safety stuff
	return *this;
}
GC::shared_disjoint_handle &GC::shared_disjoint_handle::operator=(shared_disjoint_handle &&other)
{
	if (&other != this)
	{
		// repoint to null with the ref count logic
		reset();

		// then steal disjunction ownership - ref count change is net zero
		data = std::exchange(other.data, nullptr);
		__module = std::exchange(other.__module, nullptr);
	}

	return *this;
}

GC::shared_disjoint_handle &GC::shared_disjoint_handle::operator=(std::nullptr_t)
{
	reset();
	return *this;
}

GC::shared_disjoint_handle &GC::shared_disjoint_handle::operator=(const weak_disjoint_handle &other)
{
	lock(other.data);
	return *this;
}

// ----------------------------- //

// -- weak disjunction handle -- //

// ----------------------------- //

void GC::weak_disjoint_handle::reset(handle_data *other)
{
	// handle redundant assignment as no-op
	if (data == other) return;

	// if we pointed at something
	if (data)
	{
		// drop a weak ref and get the previous tag
		auto prev = data->tag_sub(handle_data::weak_1, std::memory_order_acq_rel);

		// if we were the last weak ref and there were no strong refs, the object is already destroyed and needs to be deleted.
		// being the last weak ref implies there are no locks at the moment because without unsynchronized read/write to the same variable from several threads that's impossible.
		// therefore we don't need to bother about strong vs. non-lock strong references in this context because strong == non-lock strong
		if ((prev & handle_data::weak_mask) == handle_data::weak_1 && (prev & handle_data::strong_mask) == 0)
		{
			// in this case the strong count fell to zero, so the object is potentially being destroyed by the strong ref count logic.
			// however, that strong ref count logic won't delete the data block because our existence proves there was a weak reference prior to the strong ref dec logic.
			// therefore we just need to wait until the object is destroyed so we can delete the data block
			while (!data->destroyed_flag.load(std::memory_order_acquire));
			delete data;
		}
	}

	// repoint - if non-null bump up weak ref count
	data = other;
	if (data) data->tag_add(handle_data::weak_1, std::memory_order_acq_rel);
}

GC::weak_disjoint_handle::weak_disjoint_handle(std::nullptr_t) noexcept : data(nullptr) {}
GC::weak_disjoint_handle::~weak_disjoint_handle() { reset(); }

GC::weak_disjoint_handle::weak_disjoint_handle(const weak_disjoint_handle &other)
{
	data = other.data; // alias the same disjunction
	if (data) data->tag_add(handle_data::weak_1, std::memory_order_acq_rel); // bump up the weak ref count to account for our aliasing
}
GC::weak_disjoint_handle::weak_disjoint_handle(weak_disjoint_handle &&other) noexcept
{
	data = std::exchange(other.data, nullptr); // steal disjunction ownership - ref count change is net zero
}

GC::weak_disjoint_handle &GC::weak_disjoint_handle::operator=(const weak_disjoint_handle &other)
{
	reset(other.data); // internally performs self-assignment safety
	return *this;
}
GC::weak_disjoint_handle &GC::weak_disjoint_handle::operator=(weak_disjoint_handle &&other)
{
	if (&other != this)
	{
		reset(); // repoint to null with the ref count logic
		data = std::exchange(other.data, nullptr); // then unsafe repoint to other and disconnect other - the ref count change is net zero
	}
	return *this;
}

GC::weak_disjoint_handle &GC::weak_disjoint_handle::operator=(std::nullptr_t)
{
	reset(); 
	return *this;
}

GC::weak_disjoint_handle::weak_disjoint_handle(const shared_disjoint_handle &other)
{
	data = other.data; // alias the same disjunction
	if (data) data->tag_add(handle_data::weak_1, std::memory_order_acq_rel); // bump up the weak ref count to account for our aliasing
}
GC::weak_disjoint_handle &GC::weak_disjoint_handle::operator=(const shared_disjoint_handle &other)
{
	reset(other.data); // perform the repoint action, accounting for ref counts
	return *this;
}

bool GC::weak_disjoint_handle::expired() const noexcept
{
	// get the current tag
	auto tag = data ? data->tag.load(std::memory_order_acq_rel) : 0;

	// this weak ptr is expired if there are no strong refs remaining.
	// we include lock strong refs because otherwise it's possible for a series of atomic steps to result in a successful lock but intermediately no non-lock strong refs.
	// due to locking, however, this approach could report false negatives (but never false positives).
	return (tag & handle_data::strong_mask) == 0;
}

// ---------------- //

// -- collection -- //

// ---------------- //

void GC::collect()
{
	disjoint_module::local()->collect();
}

// ------------------------------ //

// -- utility router functions -- //

// ------------------------------ //

void GC::router_unroot(const smart_handle &arc)
{
	arc.disjunction->schedule_handle_unroot(arc);
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
	static struct _
	{
		_()
		{
			std::thread([]
			{
				#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_HANDLE_LOGGING
				std::cerr << "start timed collect thread: " << std::this_thread::get_id() << '\n';
				#endif

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
							// run a collect pass for all the dynamic disjunctions
							disjoint_module_container::get().BACKGROUND_COLLECTOR_ONLY___collect(true);
						}
						// otherwise perform any other relevant logic
						else
						{
							// even if we don't perform a dynamic disjunction collection, we need to cull dangling references
							disjoint_module_container::get().BACKGROUND_COLLECTOR_ONLY___collect(false);
						}
					}
				}
				// if we ever hit an error, something terrible happened
				catch (...)
				{
					// print error message and terminate with a nonzero code
					std::cerr << "CRITICAL ERROR: garbage collection threw an exception\n";
					std::abort();
				}
			}).detach();
		}
	} __;
}
