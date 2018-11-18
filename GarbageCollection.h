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
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <new>

// ------------------------ //

// -- Garbage Collection -- //

// ------------------------ //

class GC
{
private: struct info; // forward decl

public: // -- outgoing arcs -- //

	// type used for router event actions. USERS SHOULD NOT USE THIS DIRECTLY.
	struct router_fn
	{
	private: // -- contents hidden for security -- //

		void(*const func)(info *const &); // raw function pointer to call

		router_fn(void(*_func)(info *const &)) : func(_func) {}

		void operator()(info *const &arg) { func(arg); }
		void operator()(info *&&arg) = delete; // for safety - ensures we can't call with an rvalue

		friend class GC;
	};

	// for all data elements "elem" OWNED by obj that either ARE or OWN (directly or indirectly) a GC::ptr value, calls GC::route(elem, func) exactly once.
	// obj must be the SOLE owner of elem, and elem must be the SOLE owner of its (direct or indirect) GC::ptr values.
	// it is undefined behavior to use any gc utilities (directly or indirectly) during this function's invocation.
	// this default implementation is sufficient for any type that does not contain GC::ptr by value (directly or indirectly).
	// T should be non-const and the function should take const &T.
	// THIS IS USED FOR ROUTER DEFINITION - YOU SHOULD NOT USE IT DIRECTLY FOR ROUTING (E.G. INSIDE YOUR router<T>::route() DEFINITION).
	template<typename T>
	struct router { static void route(const T &obj, router_fn func) {} };

	// routes obj into func recursively.
	// you should use this function for routing in router<T>::route() definitions (rather than direct use of router<T>::route()).
	template<typename T>
	static void route(const T &obj, router_fn func) { router<std::remove_const_t<T>>::route(obj, func); }

	// routes each element in an iterator range into func recursively.
	// like route(), this function is safe to use directly - DO NOT USE router<T>::route() DIRECTLY
	template<typename IterBegin, typename IterEnd>
	static void route_range(IterBegin begin, IterEnd end, router_fn func) { for (; begin != end; ++begin) GC::route(*begin, func); }

private: // -- private types -- //

	// the raw version of router<T> that uses void* instead of T& (we have to do this for generality).
	// equivalent to router<T>::route() where obj is reinterpret cast to T* and dereferenced.
	template<typename T>
	struct __router
	{
		static void __route(void *obj, router_fn func) { router<T>::route(*reinterpret_cast<T*>(obj), func); }
	};

	// represents a single garbage-collected object's allocation info.
	// this is used internally by the garbage collector's logic - DO NOT MANUALLY MODIFY THIS.
	// ANY POINTER OF THIS TYPE UNDER GC MUST AT ALL TIMES POINT TO A VALID OBJECT OR NULL.
	struct info
	{
		void *const obj; // pointer to the managed object

		void(*const dtor)(void *_obj);    // a function to destroy obj
		void(*const dealloc)(void *_obj); // a function to deallocate memory - called after dtor()

		void(*const router)(void *_obj, router_fn); // a router function to use for this object

		std::size_t ref_count = 1;      // the reference count for this allocation
		bool        destroying = false; // marks if the object is currently in the process of being destroyed (multi-delete safety flag)

		bool marked; // only used for GC::collect() - otherwise undefined

		info *prev; // the std::list iterator contract isn't quite what we want
		info *next; // so we need to manage a linked list on our own

		// populates info - ref count starts at 1
		info(void *_obj, void(*_dtor)(void*), void(*_dealloc)(void*), void(*_router)(void*, router_fn))
			: obj(_obj), dtor(_dtor), dealloc(_dealloc), router(_router)
		{}
	};

	// used to select a GC::ptr constructor that does not perform a GC::root operation
	struct no_rooting_t {};

private: // -- construction / destruction -- //

	// constructs or destructs T in place
	template<typename T>
	struct __ctor_dtor
	{
		template<typename ...Args>
		static void ctor(T *pos, Args &&...args) { new (pos) T{std::forward<Args>(args)...}; }

		static void dtor(T *pos) { pos->~T(); }
		static void __dtor(void *pos) { dtor((T*)pos); }
	};

	// handles the construction/destruction of C-style arrays in place
	template<typename T, std::size_t N>
	struct __ctor_dtor<T[N]>
	{
		static void ctor(T(*pos)[N]) { for (std::size_t i = 0; i < N; ++i) __ctor_dtor<T>::ctor(*pos + i); }

		static void dtor(T(*pos)[N]) { for (std::size_t i = 0; i < N; ++i) __ctor_dtor<T>::dtor(*pos + i); }
		static void __dtor(void *pos) { dtor((T(*)[N])pos); }
	};

private: // -- extent extensions -- //

	// gets the total extent of a multi-dimensional array. 1 for scalar types.
	template<typename T>
	struct __full_extent : std::integral_constant<std::size_t, 1> {};

	template<typename T, std::size_t N>
	struct __full_extent<T[N]> : std::integral_constant<std::size_t, N * __full_extent<T>::value> {};

public: // -- ptr -- //
	
	// a self-managed garbage-collected pointer
	template<typename T>
	struct ptr
	{
	private: // -- data -- //

		T *obj; // pointer to the object (not the same as handle->obj because of type conversions)

		GC::info *handle; // the handle to use for gc management functions.

		friend class GC;

	private: // -- helpers -- //

		// changes what handle we should use, properly unlinking ourselves from the old one and linking to the new one.
		// the new handle must come from a pre-existing ptr object of a compatible type (i.e. static or dynamic cast).
		// _obj must be properly sourced from _handle->obj and non-null if _handle is non-null.
		// if _handle (and _obj) is null, the resulting state is empty.
		void reset(T *_obj, GC::info *_handle)
		{
			// repoint to the proper object
			obj = _obj;

			// we only need to do anything else if we refer to different gc allocations
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
		explicit ptr(GC::no_rooting_t) : obj(nullptr), handle(nullptr) {}
		// must be used after the no_rooting_t ctor - ROOTS INTERNAL HANDLE AND SETS HANDLE.
		// _obj must be properly sourced from _handle->obj and non-null if _handle is non-null.
		// if _handle (and _obj) is null, creates a valid, but empty ptr.
		// GC::mutex must be locked prior to invocation.
		void __init(T *_obj, GC::info *_handle)
		{
			GC::__root(handle);

			obj = _obj;
			handle = _handle;
		}

	public: // -- ctor / dtor / asgn -- //

		// creates an empty ptr (null)
		ptr() : obj(nullptr), handle(nullptr)
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

				// set obj to null (better to get nullptr exceptions than segfaults)
				obj = nullptr;

				// unregister handle as a root
				GC::__unroot(handle);
			}

			// after a call to __delref we must call handle_del_list()
			if (call_handle_del_list) GC::handle_del_list();
		}

		// constructs a new gc pointer from a pre-existing one. allows any conversion that can be statically-checked.
		ptr(const ptr &other) : obj(other.obj), handle(other.handle)
		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			GC::__root(handle); // register handle as a root
			if (handle) GC::__addref(handle); // we're new - inc ref count
		}
		template<typename J, std::enable_if_t<std::is_convertible<J*, T*>::value, int> = 0>
		ptr(const ptr<J> &other) : obj(static_cast<T*>(other.obj)), handle(other.handle)
		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			GC::__root(handle); // register handle as a root
			if (handle) GC::__addref(handle); // we're new - inc ref count
		}

		// assigns a pre-existing gc pointer a new object. allows any conversion that can be statically-checked.
		ptr &operator=(const ptr &other) { reset(other.obj, other.handle);	return *this; }
		template<typename J, std::enable_if_t<std::is_convertible<J*, T*>::value, int> = 0>
		ptr &operator=(const ptr<J> &other) { reset(static_cast<T*>(other.obj), other.handle); return *this; }

		ptr &operator=(std::nullptr_t) { reset(nullptr, nullptr); return *this; }

	public: // -- obj access -- //

		T &operator*() const { return *obj; }
		T *operator->() const { return obj; }

		// returns the number of references to the current object.
		// if this object is not pointing at any object, returns 0.
		std::size_t use_count() const
		{
			std::lock_guard<std::mutex> lock(GC::mutex);
			return handle ? handle->ref_count : 0;
		}

		// gets a pointer to the managed object. if this ptr does not point at a managed object, returns null.
		T *get() const { return obj; }

		// returns true iff this ptr points to a managed object (non-null)
		explicit operator bool() const { return get() != nullptr; }

	public: // -- comparison -- //

		friend bool operator==(const ptr &a, const ptr &b) { return a.get() == b.get(); }
		friend bool operator!=(const ptr &a, const ptr &b) { return a.get() != b.get(); }
		friend bool operator<(const ptr &a, const ptr &b) { return a.get() < b.get(); }
		friend bool operator<=(const ptr &a, const ptr &b) { return a.get() <= b.get(); }
		friend bool operator>(const ptr &a, const ptr &b) { return a.get() > b.get(); }
		friend bool operator>=(const ptr &a, const ptr &b) { return a.get() >= b.get(); }

		friend bool operator==(const ptr &a, std::nullptr_t b) { return a.get() == b; }
		friend bool operator!=(const ptr &a, std::nullptr_t b) { return a.get() != b; }
		friend bool operator<(const ptr &a, std::nullptr_t b) { return a.get() < b; }
		friend bool operator<=(const ptr &a, std::nullptr_t b) { return a.get() <= b; }
		friend bool operator>(const ptr &a, std::nullptr_t b) { return a.get() > b; }
		friend bool operator>=(const ptr &a, std::nullptr_t b) { return a.get() >= b; }

		friend bool operator==(std::nullptr_t a, const ptr &b) { return a == b.get(); }
		friend bool operator!=(std::nullptr_t a, const ptr &b) { return a != b.get(); }
		friend bool operator<(std::nullptr_t a, const ptr &b) { return a < b.get(); }
		friend bool operator<=(std::nullptr_t a, const ptr &b) { return a <= b.get(); }
		friend bool operator>(std::nullptr_t a, const ptr &b) { return a > b.get(); }
		friend bool operator>=(std::nullptr_t a, const ptr &b) { return a >= b.get(); }
	};

public: // -- core router specializations -- //

	// specialization of router for ptr<T> (i.e. ptr<T> can be thought of as a struct containing a ptr<T>).
	// this is required, as all GC::route() calls must eventually decay to calling ptr<T> routers.
	template<typename T>
	struct router<ptr<T>>
	{
		static void route(const ptr<T> &obj, router_fn func) { func(obj.handle); }
	};

	// routes a message directed at a C-style array to each element in said array
	template<typename T, std::size_t N>
	struct router<T[N]>
	{
		static void route(const T(&objs)[N], router_fn func) { for (std::size_t i = 0; i < N; ++i) GC::route(objs[i], func); }
	};

public: // -- stdlib router specializations -- //

	template<typename T, typename Allocator>
	struct router<std::vector<T, Allocator>>
	{
		static void route(const std::vector<T, Allocator> &vec, router_fn func) { route_range(vec.begin(), vec.end(), func); }
	};

public: // -- ptr allocation -- //

	// creates a new dynamic instance of T that is bound to a ptr.
	// throws any exception resulting from T's constructor but does not leak resources if this occurs.
	template<typename T, typename ...Args>
	static ptr<T> make(Args &&...args)
	{
		// -- normalize T -- //

		// typedef the non-const type
		typedef std::remove_const_t<T> NCT;

		// get the element type and the full extent
		typedef std::remove_const_t<std::remove_all_extents_t<NCT>> ElemT;
		constexpr std::size_t full_extent = __full_extent<NCT>::value;

		// -- create the buffer for both NCT and its info object -- //

		// allocate aligned space for NCT and info
		void *buf = GC::aligned_malloc(sizeof(NCT) + sizeof(info), std::max(alignof(NCT), alignof(info)));

		// if that failed but we have allocfail collect mode enabled
		if (!buf && (int)strategy() & (int)strategies::allocfail)
		{
			// collect and retry the allocation
			GC::collect();
			buf = GC::aligned_malloc(sizeof(NCT) + sizeof(info), std::max(alignof(NCT), alignof(info)));
		}

		// if that failed, throw bad alloc
		if (!buf) throw std::bad_alloc();

		// alias the buffer partitions (pt == buf always)
		NCT  *obj = reinterpret_cast<NCT*>(buf);
		info *handle = reinterpret_cast<info*>((char*)buf + sizeof(NCT));

		// -- construct the objects -- //

		// try to construct the NCT object
		try { __ctor_dtor<NCT>::ctor(obj, std::forward<Args>(args)...); }
		// if that fails, deallocate buf and rethrow
		catch (...) { GC::aligned_free(buf); throw; }

		// construct the info object
		new (handle) info(
			obj,                      // object to manage
			__ctor_dtor<NCT>::__dtor, // dtor function
			GC::aligned_free,         // dealloc function
			__router<NCT>::__route);  // router function

		// -- do the garbage collection aspects -- //

		// create a ptr ahead of time - uses correct T - (make sure to use the no_rooting_t ctor)
		ptr<T> res(GC::no_rooting_t{});

		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			// link the info object
			__link(handle);

			// initialize ptr with handle
			res.__init(obj, handle);

			// claim this object's children
			handle->router(handle->obj, GC::__unroot);

			// begin timed collect
			__start_timed_collect();
		}

		// return the created ptr
		return res;
	}

	// triggers a full garbage collection pass.
	// objects that are not in use will be deleted.
	// objects that are in use will not be moved (i.e. pointers will still be valid).
	static void collect();

public: // -- auto collection -- //

	// represents the type of automatic garbage collection to perform.
	enum class strategies
	{
		manual = 0, // no automatic garbage collection

		timed = 1,     // garbage collect on a timed basis
		allocfail = 2, // garbage collect each time a call to GC::make has an allocation failure
	};
	
	friend strategies operator|(strategies a, strategies b) { return (strategies)((int)a | (int)b); }
	friend strategies operator&(strategies a, strategies b) { return (strategies)((int)a & (int)b); }

	// type to use for measuring timed strategy sleep times.
	typedef std::chrono::milliseconds sleep_time_t;

	// gets/sets the current automatic garbage collection strategy.
	// note: this only applies to cycle resolution - non-cyclic references are always handled immediately.
	static strategies strategy();
	static void strategy(strategies new_strategy);

	// gets/sets the sleep time for timed automatic garbage collection strategy.
	// note: only used if the timed strategy flag is set.
	static sleep_time_t sleep_time();
	static void sleep_time(sleep_time_t new_sleep_time);

private: // -- data -- //

	static std::mutex mutex; // used to support thread-safety of gc operations

	static info *first; // pointer to the first gc allocation
	static info *last;  // pointer to the last gc allocation (not the same as the end iterator)

	static std::unordered_set<info *const *> roots; // a database of all gc root handles - (references - do not delete)

	static std::vector<info*> del_list; // list of handles that are scheduled for deletion (from __delref async calls)

	// -----------------------------------------------

	static std::atomic<strategies> _strategy; // the auto collect tactics currently in place

	static std::atomic<sleep_time_t> _sleep_time; // the amount of time to sleep after an automatic timed collection cycle

private: // -- misc -- //

	GC() = delete; // not instantiatable

	// if T is a polymorphic type, returns a pointer to the most-derived object pointed by <ptr>; otherwise, returns <ptr>.
	template<typename T, std::enable_if_t<std::is_polymorphic<T>::value, int> = 0>
	void *get_polymorphic_root(T *ptr) { return dynamic_cast<void*>(ptr); }
	template<typename T, std::enable_if_t<!std::is_polymorphic<T>::value, int> = 0>
	void *get_polymorphic_root(T *ptr) { return ptr; }

	// allocates space and returns a pointer to at least <size> bytes which is aligned to <align>.
	// it is undefined behavior if align is not a power of 2.
	// allocating zero bytes returns null.
	// on failure, returns null.
	// must be deallocated via aligned_free().
	static void *aligned_malloc(std::size_t size, std::size_t align);
	// deallocates a block of memory allocated by aligned_malloc().
	// if <ptr> is null, does nothing.
	static void aligned_free(void *ptr);

private: // -- private interface -- //

	// -----------------------------------------------------------------
	// from here on out, function names follow these conventions:
	// starts with "__" = GC::mutex must be locked prior to invocation.
	// otherwise = GC::mutex must not be locked prior to invocation.
	// -----------------------------------------------------------------

	// registers/unregisters a gc_info* as a root.
	// safe to root if already rooted. safe to unroot if not rooted.
	static void __root(info *const &handle);
	static void __unroot(info *const &handle);

	// for safety - ensures we can't pass root/unroot an rvalue
	static void __root(info *&&handle) = delete;
	static void __unroot(info *&&handle) = delete;

	// links handle into the gc database.
	// if is undefined behavior if handle is currently in the gc database.
	static void __link(info *handle);
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

	// the first invocation of this function begins a new thread to perform timed garbage collection.
	// all subsequent invocations do nothing.
	static void __start_timed_collect();

private: // -- functions you should never ever call ever. did i mention YOU SHOULD NOT CALL THESE?? -- //

	// the function to be executed by the timed collector thread. DO NOT CALL THIS.
	static void __timed_collect_func();
};

// -------------------- //

// -- misc functions -- //

// -------------------- //

// outputs the stored pointer to the stream - equivalent to ostr << ptr.get()
template<typename T, typename U, typename V>
std::basic_ostream<U, V> &operator<<(std::basic_ostream<U, V> &ostr, const GC::ptr<T> &ptr)
{
	ostr << ptr.get();
	return ostr;
}

#endif
