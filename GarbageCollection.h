#ifndef DRAGAZO_GARBAGE_COLLECT_H
#define DRAGAZO_GARBAGE_COLLECT_H

#include <iostream>
#include <utility>
#include <mutex>
#include <memory>
#include <vector>
#include <list>
#include <type_traits>
#include <unordered_set>
#include <atomic>
#include <thread>
#include <chrono>

// ------------------------ //

// -- Garbage Collection -- //

// ------------------------ //

class GC
{
private: struct info; // forward decl

public: // -- outgoing arcs -- //

	// type used for router event actions. USERS SHOULD NOT USE THIS DIRECTLY.
	typedef void(*router_fn)(info*&);

	// for all data elements "elem" OWNED by obj that either ARE or OWN (directly or indirectly) a GC::ptr value, calls GC::route(elem, func) exactly once.
	// obj must be the SOLE owner of elem, and elem must be the SOLE owner of its (direct or indirect) GC::ptr values.
	// obj shall be safely-convertibly to T* via reinterpret cast.
	// it is undefined behavior to use any gc utilities (directly or indirectly) during this function's invocation.
	// this default implementation is sufficient for any type that does not contain GC::ptr by value (directly or indirectly).
	template<typename T>
	struct router { static void route(void *obj, router_fn func) {} };

	// convenience function - equivalent to router<T>::route(&obj, func) where T is deduced from the obj argument.
	template<typename T>
	static void route(T &obj, router_fn func) { router<T>::route(&obj, func); }

private: // -- private types -- //

	// represents a single garbage-collected object's allocation info.
	// this is used internally by the garbage collector's logic - DO NOT MANUALLY MODIFY THIS.
	// ANY POINTER OF THIS TYPE UNDER GC MUST AT ALL TIMES POINT TO A VALID OBJECT OR NULL.
	struct info
	{
		void *const obj;             // pointer to the managed object
		void(*const deleter)(void*); // a deleter function to eventually delete obj

		void(*const router)(void*, router_fn); // router function to use for this object

		std::size_t ref_count; // the reference count for this allocation

		bool marked; // only used for GC::collect() - otherwise undefined

		bool destroying = false; // marks if the object is currently in the process of being destroyed (multi-delete safety flag)

		info *prev; // the std::list iterator contract isn't quite what we want
		info *next; // so we need to manage a linked list on our own

		// populates info
		info(void *_obj, void(*_deleter)(void*), void(*_router)(void*, router_fn), std::size_t _ref_count, info *_prev, info *_next)
			: obj(_obj), deleter(_deleter), router(_router), ref_count(_ref_count), prev(_prev), next(_next)
		{}
	};

	// used to select a GC::ptr constructor that does not perform a GC::root operation
	struct no_rooting_t {};

public: // -- public interface -- //
	
	// a self-managed garbage-collected pointer
	template<typename T>
	struct ptr
	{
	private: // -- data -- //

		GC::info *handle; // the handle to use for gc management functions.

		friend class GC;

	private: // -- helpers -- //

		// changes what handle we should use, properly unlinking ourselves from the old one and linking to the new one.
		// the new handle must come from a pre-existing ptr object of the same type.
		void reset(GC::info *_handle = nullptr)
		{
			// we only need to do anything if we refer to different gc allocations
			if (handle != _handle)
			{
				bool call_handle_del_list = false;

				{
					std::lock_guard<std::mutex> lock(GC::mutex);

					// drop our object
					if (handle) call_handle_del_list = GC::__delref(handle);

					// take on other's object
					handle = _handle;
					if (handle) GC::__addref(handle);
				}

				// after a call to __delref we must call handle_del_list()
				if (call_handle_del_list) GC::handle_del_list();
			}
		}

		// creates an empty ptr but DOES NOT ROOT THE INTERNAL HANDLE
		explicit ptr(GC::no_rooting_t) : handle(nullptr) {}
		// must be used after the no_rooting_t ctor - ROOTS INTERNAL HANDLE AND SETS HANDLE
		void __init(GC::info *_handle)
		{
			GC::__root(handle);
			handle = _handle;
		}

	public: // -- ctor / dtor / asgn -- //

		// creates an empty ptr (null)
		ptr() : handle(nullptr)
		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			// register handle as a root
			GC::__root(handle);
		}

		~ptr()
		{
			bool call_handle_del_list = false;

			{
				std::lock_guard<std::mutex> lock(GC::mutex);

				// if we have a handle, dec reference count
				if (handle) call_handle_del_list = GC::__delref(handle);

				// set handle to null - we must always point to a valid object or null
				handle = nullptr;

				// unregister handle as a root
				GC::__unroot(handle);
			}

			// after a call to __delref we must call handle_del_list()
			if (call_handle_del_list) GC::handle_del_list();
		}

		ptr(const ptr &other) : handle(other.handle)
		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			// register handle as a root
			GC::__root(handle);

			// we're new - inc ref count
			if (handle) GC::__addref(handle);
		}

		ptr &operator=(const ptr &other) { reset(other.handle);	return *this; }

		ptr &operator=(std::nullptr_t) { reset(nullptr); return *this; }

	public: // -- obj access -- //

		T &operator*() const { return *(T*)handle->obj; }
		T *operator->() const { return (T*)handle->obj; }

		// returns the number of references to the current object.
		// if this object is not pointing at any object, returns 0.
		std::size_t use_count() const { return handle ? handle->ref_count : 0; }

		// gets a pointer to the managed object.
		// if this ptr does not point at a managed object, returns null.
		T *get() const { return handle ? (T*)handle->obj : nullptr; }

		// returns true iff this ptr points to a managed object (non-null)
		explicit operator bool() const { return handle != nullptr; }

	public: // -- comparison -- //

		friend bool operator==(const ptr &a, const ptr &b) { return a.handle == b.handle; }
		friend bool operator!=(const ptr &a, const ptr &b) { return a.handle != b.handle; }
		friend bool operator<(const ptr &a, const ptr &b) { return a.get() < b.get(); }
		friend bool operator<=(const ptr &a, const ptr &b) { return a.get() <= b.get(); }
		friend bool operator>(const ptr &a, const ptr &b) { return a.get() > b.get(); }
		friend bool operator>=(const ptr &a, const ptr &b) { return a.get() >= b.get(); }

		friend bool operator==(const ptr &a, std::nullptr_t b) { return a.handle == b; }
		friend bool operator!=(const ptr &a, std::nullptr_t b) { return a.handle != b; }
		friend bool operator<(const ptr &a, std::nullptr_t b) { return a.get() < b; }
		friend bool operator<=(const ptr &a, std::nullptr_t b) { return a.get() <= b; }
		friend bool operator>(const ptr &a, std::nullptr_t b) { return a.get() > b; }
		friend bool operator>=(const ptr &a, std::nullptr_t b) { return a.get() >= b; }

		friend bool operator==(std::nullptr_t a, const ptr &b) { return a == b.handle; }
		friend bool operator!=(std::nullptr_t a, const ptr &b) { return a != b.handle; }
		friend bool operator<(std::nullptr_t a, const ptr &b) { return a < b.get(); }
		friend bool operator<=(std::nullptr_t a, const ptr &b) { return a <= b.get(); }
		friend bool operator>(std::nullptr_t a, const ptr &b) { return a > b.get(); }
		friend bool operator>=(std::nullptr_t a, const ptr &b) { return a >= b.get(); }
	};

	// specialization of router for ptr<T> (i.e. ptr<T> can be thought of as a struct containing a ptr<T>)
	template<typename T>
	struct router<ptr<T>>
	{
		static void route(void *obj, router_fn func) { func(((ptr<T>*)obj)->handle); }
	};
	
	// creates a new dynamic instance of T that is bound to a ptr.
	// throws any exception resulting from T's constructor but does not leak resources if this occurs.
	template<typename T, typename ...Args>
	static ptr<T> make(Args &&...args)
	{
		// allocate the object and get a raw pointer to it
		std::unique_ptr<T> obj = std::make_unique<T>(std::forward<Args>(args)...);
		void *raw = reinterpret_cast<void*>(obj.get());

		// create a ptr ahead of time (make sure to use the no_rooting_t ctor)
		ptr<T> res(GC::no_rooting_t{});

		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			// create a handle for it
			GC::info *handle = GC::__create(raw, [](void *ptr) { delete (T*)ptr; }, router<T>::route);

			// initialize ptr with handle
			res.__init(handle);

			// for each outgoing arc from obj
			handle->router(handle->obj, [](info *&arc)
			{
				// mark this arc as not being a root (because obj owns it by value)
				GC::__unroot(arc);
			});
		}

		// unlink obj from the smart pointer (all the dangerous stuff is done)
		obj.release();

		// return the created ptr
		return res;
	}

	// triggers a full garbage collection pass.
	// objects that are not in use will be deleted.
	// objects that are in use will not be moved (i.e. pointers will still be valid).
	static void collect();

public: // -- auto collection -- //

	// represents the type of automatic garbage collection to perform.
	enum class strategy
	{
		manual = 0, // no automatic garbage collection

		timed = 1, // garbage collect on a timed basis
	};

	// type to use for measuring timed strategy sleep times.
	typedef std::chrono::milliseconds sleep_time_t;

	// gets the current automatic garbage collection strategy
	static strategy get_strategy();
	// sets the automatic garbage collection strategy we should use from now on.
	// note: this only applies to cycle resolution; non-cyclic references are always handled immediately.
	static void set_strategy(strategy new_strategy);

	// gets the current timed strategy sleep time.
	static sleep_time_t get_sleep_time();
	// sets the sleep time for timed automatic garbage collection strategy.
	// note: only used if the timed strategy flag is set.
	static void set_sleep_time(sleep_time_t new_sleep_time);

private: // -- data -- //

	static std::mutex mutex; // used to support thread-safety of gc operations

	static info *first; // pointer to the first gc allocation
	static info *last;  // pointer to the last gc allocation (not the same as the end iterator)

	static std::unordered_set<info**> roots; // a database of all gc root handles - (references - do not delete)

	static std::unordered_set<info*> del_list; // list of handles that are scheduled for deletion (from __delref async calls)

	// -----------------------------------------------

	static strategy strat; // the auto collect tactics currently in place

	static sleep_time_t sleep_time; // the amount of time to sleep after an automatic timed collection cycle

	static std::thread auto_collect_thread; // thread that does the automatic collection work

private: // -- misc -- //

	GC() = delete; // not instantiatable

private: // -- private interface -- //

	// -----------------------------------------------------------------
	// functions in this block that start with "__" are not thread safe.
	// all other functions in this block are thread safe.
	// -----------------------------------------------------------------

	// registers/unregisters a gc_info* as a root.
	// safe to root if already rooted. safe to unroot if not rooted.
	static void __root(info *&handle);
	static void __unroot(info *&handle);

	// adds a pre-existing (non-garbage-collected) object to the garbage-collection database.
	// returns a handle that must be used to control the gc allocation - DO NOT LOSE THIS - DO NOT MODIFY THIS IN ANY WAY.
	// the aliased object begins with a reference count of 1 - YOU SHOULD NOT CALL ADDREF ~BECAUSE~ OF CREATE.
	// it is undefined behavior to call this function on an object that already exists in the gc database.
	// returns a non-null pointer. if the memory allocation fails, throws an exception but does not leak resources.
	// <obj> is the address of the actual object that was allocated dynamically that should now be managed.
	// <deleter> is a function that will be used to deallocate <obj>.
	// <outgoing> is a function that returns the begin/end range of outgoing gc-qualified pointer offsets from <obj>.
	static info *__create(void *obj, void(*deleter)(void*), void(*router)(void*, router_fn));

	// unlinks handle from the gc database.
	// it is undefined behavior if handle is not currently in the gc database.
	static void __unlink(info *handle);

	// adds a reference count to a garbage-collected object.
	static void __addref(info *handle);
	// removes a reference count from a garbage-collected object.
	// instead of destroying the object immediately when the ref count reaches zero, adds it to del_list.
	// returns true iff the object was scheduled for destruction in del_list.
	static bool __delref(info *handle);

	// handles actual deletion of any objects scheduled for deletion if del_list.
	static void handle_del_list();

	// performs a mark sweep operation from the given handle.
	static void __mark_sweep(info *handle);

private: // -- functions you should never ever call ever. did i mention YOU SHOULD NOT CALL THESE?? -- //

	// this is the function that will be run by auto_collect_thread.
	// never under any circumstance should you ever call this.
	static void __auto_collect_thread_func();
};

// -------------------- //

// -- misc functions -- //

// -------------------- //

// if T is a polymorphic type, returns a pointer to the most-derived object pointed by <ptr>; otherwise, returns <ptr>.
template<typename T, std::enable_if_t<std::is_polymorphic<T>::value, int> = 0>
void *get_polymorphic_root(T *ptr) { return dynamic_cast<void*>(ptr); }
template<typename T, std::enable_if_t<!std::is_polymorphic<T>::value, int> = 0>
void *get_polymorphic_root(T *ptr) { return ptr; }

// outputs the stored pointer to the stream - equivalent to ostr << ptr.get()
template<typename T, typename U, typename V>
std::basic_ostream<U, V> &operator<<(std::basic_ostream<U, V> &ostr, const GC::ptr<T> &ptr)
{
	ostr << ptr.get();
	return ostr;
}

#endif
