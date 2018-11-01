#include <iostream>
#include <cstdlib>
#include <utility>
#include <memory>
#include <mutex>
#include <list>
#include <vector>
#include <algorithm>

#include "GarbageCollection.h"

// ---------- //

// -- data -- //

// ---------- //

std::mutex gc_mutex; // used to support thread-safety of gc operations

gc_info *gc_first = nullptr; // pointer to the first gc allocation
gc_info *gc_last = nullptr;  // pointer to the last gc allocation (not the same as the end iterator)

std::vector<gc_info**> gc_roots; // a database of all gc root handles - sorted - (references - do not delete)

// ------------- //

// -- helpers -- //

// ------------- //

void __gc_register(gc_info *&handle)
{
	// find where it should be in the roots database
	auto iter = std::lower_bound(gc_roots.begin(), gc_roots.end(), &handle);

	// if it's not in there, add it
	if (iter == gc_roots.end() || *iter != &handle) gc_roots.insert(iter, &handle);
}
void __gc_unregister(gc_info *&handle)
{
	// find where it should be in the roots database
	auto iter = std::lower_bound(gc_roots.begin(), gc_roots.end(), &handle);

	// if it's in there, remove it
	if (iter != gc_roots.end() && *iter == &handle) gc_roots.erase(iter);
}

gc_info *__gc_create(void *obj, void(*deleter)(void*), gc_outgoing_t(*outgoing)())
{
	// create a gc entry for it
	gc_info *info = new gc_info(obj, deleter, outgoing, 1, gc_last, nullptr);

	// put it in the database
	if (gc_last) gc_last = gc_last->next = info;
	else gc_first = gc_last = info;

	// return the gc alloc entry
	return info;
}

void __gc_delete(gc_info *handle)
{
	// remove it from the gc database
	if (handle == gc_first && handle == gc_last) gc_first = gc_last = nullptr;
	else if (handle == gc_first) (gc_first = gc_first->next)->prev = nullptr;
	else if (handle == gc_last) (gc_last = gc_last->prev)->next = nullptr;
	else
	{
		handle->prev->next = handle->next;
		handle->next->prev = handle->prev;
	}

	std::cerr << "\ngc deleting " << handle->obj << '\n';

	// call its deleter for the stored object
	handle->deleter(handle->obj);

	// delete the info object itself
	delete handle;
}

void __gc_addref(gc_info *handle)
{
	// inc its reference count
	++handle->ref_count;
}
void __gc_delref(gc_info *handle)
{
	// dec its ref count - if result is now zero, delete it
	if (--handle->ref_count == 0) __gc_delete(handle);
}

void __gc_collect()
{
	// don't do anything for now - we need basic stuff working first
}

// --------------- //

// -- interface -- //

// --------------- //

void gc_register(gc_info *&handle)
{
	std::lock_guard<std::mutex> lock(gc_mutex);
	__gc_register(handle);
}
void gc_unregister(gc_info *&handle)
{
	std::lock_guard<std::mutex> lock(gc_mutex);
	__gc_unregister(handle);
}

gc_info *gc_create(void *obj, void(*deleter)(void*), gc_outgoing_t(*outgoing)())
{
	std::lock_guard<std::mutex> lock(gc_mutex);
	return __gc_create(obj, deleter, outgoing);
}

void gc_addref(gc_info *handle)
{
	std::lock_guard<std::mutex> lock(gc_mutex);
	__gc_addref(handle);
}
void gc_delref(gc_info *handle)
{
	std::lock_guard<std::mutex> lock(gc_mutex);
	__gc_delref(handle);
}

void gc_collect()
{
	std::lock_guard<std::mutex> lock(gc_mutex);
	__gc_collect();
}
