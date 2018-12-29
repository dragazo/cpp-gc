#ifndef DRAGAZO_GARBAGE_COLLECT_H
#define DRAGAZO_GARBAGE_COLLECT_H

#include <iostream>
#include <utility>
#include <mutex>
#include <atomic>
#include <new>
#include <type_traits>
#include <thread>
#include <chrono>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <cassert>
#include <cstddef>

#include <memory>
#include <tuple>

#include <array>
#include <vector>
#include <deque>
#include <forward_list>
#include <list>

#include <set>
#include <map>

#include <unordered_set>
#include <unordered_map>

#include <stack>
#include <queue>

// --------------------- //

// -- cpp-gc settings -- //

// --------------------- //

// the C++ version id - this is used to selectively enable various C++ version-specific features.
// increasing numbers imply newer versions of C++ standard.
// C++11 = 11, C++14 = 14, C++17 = 17, C++20 = 20
#define DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID 14

// ------------------- //

// -- utility types -- //

// ------------------- //

// this is equivalent to std::scoped_lock - included here because it's really handy and is otherwise C++17 only.
// you shouldn't use this type by name - you should refer to it by is alias in the GC class i.e. GC::scoped_lock.
template<typename ...Lockable>
class __gc_scoped_lock
{
private: // -- helpers -- //

	// performs the unlocking step for our custom scoped_lock impl.
	// Len is the length of the current T... slice (initially sizeof...(T)).
	template<std::size_t Len, typename ...T>
	struct unlocker
	{
		static void unlock(const std::tuple<T&...> &t)
		{
			std::get<Len - 1>(t).unlock();
			unlocker<Len - 1, T...>::unlock(t);
		}
	};
	template<typename ...T>
	struct unlocker<0, T...>
	{
		static void unlock(const std::tuple<T&...> &t) {}
	};

private: // -- data -- //

	std::tuple<Lockable&...> locks;

public: // -- ctor / dtor / asgn -- //

	explicit __gc_scoped_lock(Lockable &...lockable) : locks(lockable...)
	{
		std::lock(lockable...);
	}
	__gc_scoped_lock(std::adopt_lock_t, Lockable &...lockable) : locks(lockable...)
	{}

	~__gc_scoped_lock()
	{
		unlocker<sizeof...(Lockable), Lockable...>::unlock(locks);
	}

	__gc_scoped_lock(const __gc_scoped_lock&) = delete;
	__gc_scoped_lock &operator=(const __gc_scoped_lock&) = delete;
};
template<typename Lockable>
class __gc_scoped_lock<Lockable>
{
private: // -- data -- //

	Lockable &lock; // the (single) lockable object

public: // -- ctor / dtor / asgn -- //

	explicit __gc_scoped_lock(Lockable &lockable) : lock(lockable)
	{
		lockable.lock();
	}
	__gc_scoped_lock(std::adopt_lock_t, Lockable &lockable) : lock(lockable)
	{}

	~__gc_scoped_lock()
	{
		lock.unlock();
	}

	__gc_scoped_lock(const __gc_scoped_lock&) = delete;
	__gc_scoped_lock &operator=(const __gc_scoped_lock&) = delete;
};
template<>
class __gc_scoped_lock<>
{
public: // -- ctor / dtor / asgn -- //

	explicit __gc_scoped_lock() = default;
	__gc_scoped_lock(std::adopt_lock_t) {}

	~__gc_scoped_lock() = default;

	__gc_scoped_lock(const __gc_scoped_lock&) = delete;
	__gc_scoped_lock &operator=(const __gc_scoped_lock&) = delete;
};

// ------------------------ //

// -- garbage collection -- //

// ------------------------ //

class GC
{
public: // -- router function types -- //

	struct router_fn;
	struct mutable_router_fn;

private: // -- router function usage safety -- //

	// gets if T is any of several types - equivalent to (std::is_same<T, Types>::value || ...) but doesn't require C++17 fold expressions
	template<typename T, typename First, typename ...Rest>
	struct is_same_any : std::integral_constant<bool, std::is_same<T, First>::value || GC::is_same_any<T, Rest...>::value> {};
	template<typename T, typename First>
	struct is_same_any<T, First> : std::integral_constant<bool, std::is_same<T, First>::value> {};

	// defines if T is a router function object type - facilitates a type safety mechanism for GC::route().
	template<typename T>
	struct is_router_function_object : GC::is_same_any<T, GC::router_fn, GC::mutable_router_fn> {};

	// gets if T is a well-formed router function overload
	template<typename T, typename F>
	struct is_well_formed_router_function : GC::is_router_function_object<F>
	{
	// was used to make sure router funcs used the correct types - commented out because gcc doesn't like it
	//private: typedef decltype(static_cast<void(*)(const T&, F)>(GC::router<std::remove_cv_t<T>>::route)) __type_check; // just used to make sure it exists
	};

public: // -- router functions -- //

	// THE FOLLOWING INFORMATION IS CRITICAL FOR ANY USAGE OF THIS LIBRARY.

	// a type T is defined to be "gc" if it owns an object that is itself considered to be gc.
	// by definition, GC::ptr, GC::atomic_ptr, and std::atomic<GC::ptr> are gc types.
	// ownership means said "owned" object's lifetime is entirely controlled by the "owner".
	// an object may only have one owner at any point in time - shared ownership is considered non-owning.
	// thus global variables and static member variables are never considered to be owned objects.
	// at any point in time, the owned object is considered to be part of its owner (i.e. as if it were a by-value member).

	// the simplest form of ownership is a by-value member.
	// another common category of owned object is a by-value container (e.g. the contents of std::vector, std::list, std::set, etc.).
	// another case is a uniquely-owning pointer or reference - e.g. pointed-to object of std::unique_ptr, or any other (potentially-smart) pointer/reference to an object you know you own uniquely.
	// of course, these cases can be mixed - e.g. a by-value member std::unique_ptr which is a uniquely-owning pointer to a by-value container std::vector of a gc type.
	
	// it is important to remember that a container type like std::vector<T> is a gc type if T is a gc type (because it thus contains or "owns" gc objects).

	// object reachability traversals are performed by router functions.
	// for a gc type T, its router functions route a function-like object to its owned gc objects recursively.
	// because object ownership cannot be cyclic, this will never degrade into infinite recursion.

	// routing to anything you don't own is undefined behavior.
	// thus, although it is legal to route to the "contents" of an owned object (i.e. owned object of an owned object), it is typically dangerous to do so.
	// in general, you should just route to all your by-value members - it is their responsibility to properly route the message the rest of the way.
	// thus, if you own a std::vector<T> where T is a gc type, you should just route to the vector itself, which has its own router functions to route the message to its contents.

	// if you are the owner of a gc type object, it is undefined behavior not to route to it (except in the special case of a mutable router function - see below).

	// for a gc type T, a "mutable" owned object is defined to be an owned object for which you could legally route to its contents but said contents can be changed after construction.
	// e.g. std::vector, std::list, std::set, and std::unique_ptr are all examples of "mutable" gc types because you own them (and their contents), but their contents can change.
	// an "immutable" or "normal" owned object is defined as any owned object that is not "mutable".

	// more formally, consider an owned object x:
	// suppose you examine the set of all objects routed-to through x recursively.
	// x is a "mutable" owned gc object iff for any two such router invocation sets taken over the lifetime of x the two sets are different. 

	// it should be noted that re-pointing a GC::ptr, GC::atomic_ptr, or std::atomic<GC::ptr> is not a mutating action for the purposes of classifying a "mutable" owned gc object.
	// this is due to the fact that you would never route to its pointed-to contents due to it being a shared resource.

	// the following is critical and easily forgotten:
	// all mutating actions in a mutable gc object (e.g. adding items to a std::vector<GC::ptr<T>>) must occurr in a manner mutually exclusive with the object's router function.
	// this is because any thread may at any point make routing requests to any number of objects under gc management in any order and for any purpose.
	// thus, if you have e.g. a std::vector<GC::ptr<T>>, you should also have a mutex to guard it on insertion/deletion/reordering of an element and for the router function.
	// this would likely need to be encapsulated by methods of your class to ensure external code can't violate this requirement (it is undefined behavior to violate this).

	// on the bright side, cpp-gc has wrappers for all standard containers that internally apply all of this logic without the need to remember it.
	// so, you could use a GC::vector<GC::ptr<T>> instead of a std::vector<GC::ptr<T>> and avoid the need to be careful or encapsulate anything.

	// the following requirements pertain to router functions:
	// for a normal router function (i.e. GC::router_fn) this must at least route to all owned gc objects.
	// for a mutable router function (i.e. GC::mutable_router_fn) this must at least route to all owned "mutable" gc objects.
	// the mutable router function system is entirely a method for optimization and can safely be ignored if desired.

	// this type represents the router function set for objects of type T.
	// router functions must be static, named "route", return void, and take two args: const T& and a router function object by value (i.e. GC::router_fn or GC::mutable_router_fn).
	// if you don't care about the efficiency mechanism of mutable router functions, you can defined the function type as a template type paramter, but it must be deducible.
	// the default implementation is no-op, which is suitable for any non-gc type.
	// this should not be used directly for routing to owned objects - see the helper functions below.
	template<typename T>
	struct router
	{
		// make sure we don't accidentally select a cv-qualified default implementation
		static_assert(std::is_same<T, std::remove_cv_t<T>>::value, "router type T must not be cv-qualified");

		// if this overload is selected, it implies T is a non-gc type - thus we don't need to route to anything
		template<typename F> static void route(const T &obj, F func) {}
	};

	// recursively routes to obj - should only be used inside router functions
	template<typename T, typename F, std::enable_if_t<GC::is_router_function_object<F>::value, int> = 0>
	static void route(const T &obj, F func)
	{
		// make sure the underlying router function is valid
		static_assert(GC::is_well_formed_router_function<T, F>::value, "underlying router function was ill-formed");

		// call the underlying router function
		GC::router<std::remove_cv_t<T>>::route(obj, func);
	}

	// recursively routes to each object in an iteration range - should only be used inside router functions
	template<typename IterBegin, typename IterEnd, typename F, std::enable_if_t<GC::is_router_function_object<F>::value, int> = 0>
	static void route_range(IterBegin begin, IterEnd end, F func) { for (; begin != end; ++begin) GC::route(*begin, func); }

private: // -- private types -- //

	struct info;

	// the virtual function table type for info objects.
	struct info_vtable
	{
		void(*const destroy)(info&); // a function to destroy the object - allowed to deallocate obj's memory but must not deallocate the info object's memory.
		void(*const dealloc)(info&); // a function to deallocate memory - called after destroy - must not call anything but deallocation functions (e.g. no destructors).

		void(*const route)(info&, router_fn);                 // a router function to use for this object
		void(*const mutable_route)(info&, mutable_router_fn); // a mutable router function to use for this object

		info_vtable(void(*_destroy)(info&), void(*_dealloc)(info&), void(*_route)(info&, router_fn), void(*_mutable_route)(info&, mutable_router_fn))
			: destroy(_destroy), dealloc(_dealloc), route(_route), mutable_route(_mutable_route)
		{}
	};

	// represents a single garbage-collected object's allocation info.
	// this is used internally by the garbage collector's logic - DO NOT MANUALLY MODIFY THIS.
	// ANY POINTER OF THIS TYPE UNDER GC CONTROL MUST AT ALL TIMES POINT TO A VALID OBJECT OR NULL.
	struct info
	{
		void *const       obj;   // pointer to the managed object
		const std::size_t count; // the number of elements in obj (meaning varies by implementer)

		const info_vtable *const vtable; // virtual function table to use

		std::atomic<std::size_t> ref_count; // the reference count for this allocation
		
		std::atomic_flag destroying = ATOMIC_FLAG_INIT; // marks if the object is currently in the process of being destroyed (multi-delete safety flag)
		bool             destroy_completed = false;     // marks if destruction completed

		bool marked; // only used for GC::collect() - otherwise undefined

		info *prev; // the std::list iterator contract isn't quite what we want
		info *next; // so we need to manage a linked list on our own

		// populates info - ref count starts at 1 - prev/next are undefined
		info(void *_obj, std::size_t _count, const info_vtable *_vtable)
			: obj(_obj), count(_count), vtable(_vtable), ref_count(1)
		{}

		// -- helpers -- //

		void destroy() { vtable->destroy(*this); }
		void dealloc() { vtable->dealloc(*this); }

		void route(router_fn func) { vtable->route(*this, func); }
		void mutable_route(mutable_router_fn func) { vtable->mutable_route(*this, func); }
	};

	// used to select constructor paths that lack the addref step
	static struct no_addref_t {} no_addref;

	// used to select constructor paths that bind a new object
	static struct bind_new_obj_t {} bind_new_obj;

	// represents a raw_handle_t value with encapsulated syncronization logic.
	// you should not use raw_handle_t directly - use this instead.
	class smart_handle
	{
	private: // -- data -- //

		// the raw handle to manage.
		// after construction, this must never be modified directly.
		// all modification actions should be delegated to one of the collection_synchronizer functions.
		info *handle;

	public: // -- ctor / dtor / asgn -- //

		// initializes the info handle to null and marks it as a root.
		smart_handle(std::nullptr_t = nullptr)
		{
			collection_synchronizer::schedule_handle_create_null(handle);
		}

		// initializes the info handle with the specified value and marks it as a root.
		// the init object is added to the objects database in the same atomic step as the handle initialization.
		// init must be the correct value of a current object - thus the return value of raw_handle() cannot be used.
		smart_handle(info *init, bind_new_obj_t)
		{
			collection_synchronizer::schedule_handle_create_bind_new_obj(handle, init);
		}
		
		// constructs a new smart handle to alias another
		smart_handle(const smart_handle &other)
		{
			collection_synchronizer::schedule_handle_create_alias(handle, other.handle);
		}

		// unroots the internal handle.
		~smart_handle()
		{
			collection_synchronizer::schedule_handle_destroy(handle);
		}

		// repoints this smart_handle to other - equivalent to this->reset(other)
		smart_handle &operator=(const smart_handle &other) { reset(other); return *this; }

	public: // -- interface -- //

		// gets the raw handle - guaranteed to outlive the object during a gc cycle.
		// the value of the referenced pointer does not reflect the true current value.
		// said value is only meant as a snapshot of the gc graph structure at an instance for the garbage collector.
		// thus, using the value of the info* (and not just the pointer to the pointer) is undefined behavior.
		// therefore this should never be used to get an argument for a smart_handle constructor.
		info *const &raw_handle() const noexcept { return handle; }

		// safely repoints the underlying raw handle at the new handle's object.
		void reset(const smart_handle &new_value)
		{
			collection_synchronizer::schedule_handle_repoint(handle, new_value.handle);
		}
		// safely repoints the underlying raw handle at no object (null).
		void reset()
		{
			collection_synchronizer::schedule_handle_repoint_null(handle);
		}

		// safely swaps the underlying raw handles.
		void swap(smart_handle &other)
		{
			collection_synchronizer::schedule_handle_repoint_swap(handle, other.handle);
		}
		friend void swap(smart_handle &a, smart_handle &b) { a.swap(b); }
	};

private: // -- base router function -- //

	// the base type for all router functions.
	// this class should not be used directly.
	// it is undefined behavior to delete this type polymorphically.
	struct __base_router_fn
	{
	protected: // -- contents hidden for security -- //

		void(*const func)(const smart_handle&); // raw function pointer to call

		__base_router_fn(void(*_func)(const smart_handle&)) : func(_func) {}
		__base_router_fn(std::nullptr_t) = delete;

		~__base_router_fn() = default;

		void operator()(const smart_handle &arg) { func(arg); }
		void operator()(smart_handle&&) = delete; // for safety - ensures we can't call with an rvalue
	};

public: // -- specific router function type definitions -- //

	// type used for a normal router event
	struct router_fn : __base_router_fn { using __base_router_fn::__base_router_fn; friend class GC; };
	// type used for mutable router event
	struct mutable_router_fn : __base_router_fn { using __base_router_fn::__base_router_fn; friend class GC; };

private: // -- extent extensions -- //

	// gets the full (total) extent of a potentially multi-dimensional array.
	// for scalar types this is 1.
	// for array of unknown bound, returns the full extent of the known bounds.
	template<typename T>
	struct full_extent : std::integral_constant<std::size_t, 1> {};

	template<typename T, std::size_t N>
	struct full_extent<T[N]> : std::integral_constant<std::size_t, N * full_extent<T>::value> {};

	template<typename T>
	struct full_extent<T[]> : std::integral_constant<std::size_t, full_extent<T>::value> {};

private: // -- array typing helpers -- //

	// true if T is an array of unknown bound, false otherwise
	template<typename T>
	struct is_unbound_array : std::false_type {};
	template<typename T>
	struct is_unbound_array<T[]> : std::true_type {};

	// true if T is an array of known bound, false otherwise
	template<typename T>
	struct is_bound_array : std::false_type {};
	template<typename T, std::size_t N>
	struct is_bound_array<T[N]> : std::true_type {};

	// stips off the top level of unbound array (bound is not stripped)
	template<typename T>
	struct remove_unbound_extent { typedef T type; };
	template<typename T>
	struct remove_unbound_extent<T[]> { typedef T type; };

	// strips off the top level of bound array (unbound is not stripped)
	template<typename T>
	struct remove_bound_extent { typedef T type; };
	template<typename T, std::size_t N>
	struct remove_bound_extent<T[N]> { typedef T type; };

	template<typename T> using remove_unbound_extent_t = typename remove_unbound_extent<T>::type;
	template<typename T> using remove_bound_extent_t = typename remove_bound_extent<T>::type;

public: // -- mutex helpers -- //

	// equivalent to std::scoped_lock - included here because it's handy and is otherwise only available in C++17 and up
	template<typename ...T>
	using scoped_lock = __gc_scoped_lock<T...>;

public: // -- ptr -- //

	// a self-managed garbage-collected pointer to type T.
	// if T is an unbound array, it is considered the "array" form, otherwise it is the "scalar" form.
	// scalar and array forms may offer different interfaces.
	// this type is not internally synchronized (i.e. not thread safe).
	// thus read/writes from several threads to the same ptr are undefined behavior (see atomic_ptr).
	template<typename T>
	struct ptr
	{
	public: // -- types -- //

		// type of element stored
		typedef GC::remove_unbound_extent_t<T> element_type;

	private: // -- data -- //

		// pointer to the object - this is only used for object access and is entirely unimportant as far a gc is concerned
		element_type *obj;

		// the raw handle wraper - this is where all the important gc logic takes place
		smart_handle handle;

		friend class GC;

	private: // -- helpers -- //

		// changes what handle we should use, properly unlinking ourselves from the old one and linking to the new one.
		// the new handle must come from a pre-existing ptr object.
		// _obj must be properly sourced from _handle->obj and non-null if _handle is non-null.
		// if _handle (and _obj) is null, the resulting state is empty.
		void reset(element_type *new_obj, const smart_handle &new_handle)
		{
			obj = new_obj;
			handle.reset(new_handle);
		}

		// constructs a new ptr instance with the specified obj and pre-existing handle.
		// this is equivalent to reset() but done at construction time for efficiency.
		ptr(element_type *new_obj, const smart_handle &new_handle) : obj(new_obj), handle(new_handle) {}

		// constructs a new ptr instance with the specified obj and handle.
		// for obj and handle: both must either be null or non-null - mixing null/non-null is undefined behavior.
		// _handle is automatically added to the gc database.
		// _handle must not have been sourced via handle.raw_handle().
		ptr(element_type *_obj, info *_handle, bind_new_obj_t) : obj(_obj), handle(_handle, GC::bind_new_obj) {}

	public: // -- ctor / dtor / asgn -- //

		// creates an empty ptr (null)
		ptr(std::nullptr_t = nullptr) : obj(nullptr), handle(nullptr) {}

		~ptr()
		{
			// set obj to null (better to get nullptr exceptions than segfaults)
			obj = nullptr;
		}

		// constructs a new gc pointer from a pre-existing one. allows any conversion that can be statically-checked.
		ptr(const ptr &other) : obj(other.obj), handle(other.handle)
		{}
		template<typename J, std::enable_if_t<std::is_convertible<J*, T*>::value, int> = 0>
		ptr(const ptr<J> &other) : obj(static_cast<element_type*>(other.obj)), handle(other.handle)
		{}

		// assigns a pre-existing gc pointer a new object. allows any conversion that can be statically-checked.
		ptr &operator=(const ptr &other)
		{
			reset(other.obj, other.handle);
			return *this;
		}
		template<typename J, std::enable_if_t<std::is_convertible<J*, T*>::value, int> = 0>
		ptr &operator=(const ptr<J> &other)
		{
			reset(static_cast<element_type*>(other.obj), other.handle);
			return *this;
		}

		ptr &operator=(std::nullptr_t) { reset(nullptr, nullptr); return *this; }

	public: // -- obj access -- //

		// gets a pointer to the managed object. if this ptr does not point at a managed object, returns null.
		element_type *get() const { return obj; }

		element_type &operator*() const { return *get(); }
		element_type *operator->() const { return get(); }

		// returns true iff this ptr points to a managed object (non-null)
		explicit operator bool() const { return get() != nullptr; }

	public: // -- array obj access -- //

		// accesses an item in an array. only defined if T is an array type.
		// undefined behavior if index is out of bounds.
		template<typename J = T, std::enable_if_t<std::is_same<T, J>::value && GC::is_unbound_array<J>::value, int> = 0>
		element_type &operator[](std::ptrdiff_t index) const { return get()[index]; }

		// returns a ptr to an item in an array. only defined if T is an array type.
		// the returned ptr is an owner of the entire array, so the array will not be deallocated while it exists.
		// undefined behavior if index is out of bounds.
		template<typename J = T, std::enable_if_t<std::is_same<T, J>::value && GC::is_unbound_array<J>::value, int> = 0>
		ptr<element_type> get(std::ptrdiff_t index) const { return {get() + index, handle}; }

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

	public: // -- swap -- //

		void swap(ptr &other)
		{
			using std::swap;

			swap(obj, other.obj);
			swap(handle, other.handle);
		}
		friend void swap(ptr &a, ptr &b) { a.swap(b); }
	};

	// defines an atomic gc ptr.
	// as ptr, but read/writes are synchronized and thus thread safe.
	template<typename T>
	struct atomic_ptr
	{
	private: // -- data -- //

		GC::ptr<T> value;

		mutable std::mutex mutex;

		friend struct GC::router<atomic_ptr<T>>;

	public: // -- ctor / dtor / asgn -- //

		atomic_ptr() = default;

		~atomic_ptr() = default;

		atomic_ptr(const atomic_ptr&) = delete;
		atomic_ptr &operator=(const atomic_ptr&) = delete;

	public: // -- store / load -- //

		atomic_ptr(const GC::ptr<T> &desired) : value(desired) {}
		atomic_ptr &operator=(const GC::ptr<T> &desired)
		{
			store(desired);
			return *this;
		}

		void store(const GC::ptr<T> &desired)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			value = desired;
		}
		GC::ptr<T> load() const
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			GC::ptr<T> ret = value;
			return ret;
		}

		operator GC::ptr<T>() const
		{
			return load();
		}

	public: // -- exchange -- //

		GC::ptr<T> exchange(const GC::ptr<T> &desired)
		{
			std::lock_guard<std::mutex> lock(this->mutex);

			ptr<T> old = value;
			value = desired;
			return old;
		}

		// !! add compare exchange stuff !! //

	public: // -- lock info -- //

		static constexpr bool is_always_lock_free = false;

		bool is_lock_free() const noexcept { return is_always_lock_free; }

	public: // -- swap -- //

		void swap(atomic_ptr &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			value.swap(other.value);
		}
		friend void swap(atomic_ptr &a, atomic_ptr &b) { a.swap(b); }
	};

public: // -- ptr casting -- //

	template<typename To, typename From, std::enable_if_t<std::is_convertible<From, To>::value || std::is_same<std::remove_cv_t<To>, std::remove_cv_t<From>>::value, int> = 0>
	static ptr<To> staticCast(const GC::ptr<From> &p)
	{
		return p.obj ? ptr<To>(static_cast<typename ptr<To>::element_type*>(p.obj), p.handle) : ptr<To>();
	}

	template<typename To, typename From, std::enable_if_t<std::is_polymorphic<From>::value && !std::is_array<To>::value, int> = 0>
	static ptr<To> dynamicCast(const GC::ptr<From> &p)
	{
		auto obj = dynamic_cast<typename ptr<To>::element_type*>(p.obj);
		return obj ? ptr<To>(obj, p.handle) : ptr<To>();
	}

	template<typename To, typename From, std::enable_if_t<std::is_same<std::remove_cv_t<To>, std::remove_cv_t<From>>::value, int> = 0>
	static ptr<To> constCast(const GC::ptr<From> &p)
	{
		return ptr<To>(const_cast<typename ptr<To>::element_type*>(p.obj), p.handle);
	}

	template<typename To, typename From>
	static ptr<To> reinterpretCast(const GC::ptr<From> &p)
	{
		return ptr<To>(reinterpret_cast<typename ptr<To>::element_type*>(p.obj), p.handle);
	}

public: // -- core router specializations -- //

	// base case for router - this one actually does something directly
	template<typename T>
	struct router<ptr<T>>
	{
		template<typename F> static void route(const ptr<T> &obj, F func) { func(obj.handle); }
	};

	// an appropriate specialization for atomic_ptr (does not use any calls to gc functions - see generic std::atomic<T> ill-formed construction)
	template<typename T>
	struct router<atomic_ptr<T>>
	{
		template<typename F> static void route(const atomic_ptr<T> &atomic, F func)
		{
			// we avoid calling any gc functions by not actually using the load function - we just use the object directly.
			// locking is safe because all ptr operations are non-blocking and thus this can never result in a deadlock.

			std::lock_guard<std::mutex> lock(atomic.mutex);

			GC::route(atomic.value, func);
		}
	};

public: // -- C-style array router specializations -- //

	// routes a message directed at a C-style bounded array to each element in said array
	template<typename T, std::size_t N>
	struct router<T[N]>
	{
		template<typename F> static void route(const T(&objs)[N], F func) { for (std::size_t i = 0; i < N; ++i) GC::route(objs[i], func); }
	};

	// ill-formed variant for unbounded arrays
	template<typename T>
	struct router<T[]>
	{
		// intentionally left blank - we don't know the extent, so we can't route to its contents
	};

private: // -- tuple routing helpers -- //

	// recursively routes to all elements of a tuple with ordinal indicies [0, Len)
	template<std::size_t Len, typename ...Types>
	struct __tuple_router
	{
		template<typename F> static void __route(const std::tuple<Types...> &tuple, F func)
		{
			GC::route(std::get<Len - 1>(tuple), func);
			__tuple_router<Len - 1, Types...>::__route(tuple, func);
		}
	};

	// base case - does nothing
	template<typename ...Types>
	struct __tuple_router<0, Types...>
	{
		template<typename F> static void __route(const std::tuple<Types...> &tuple, F func) {}
	};

public: // -- stdlib misc router specializations -- //

	template<typename T1, typename T2>
	struct router<std::pair<T1, T2>>
	{
		template<typename F> static void route(const std::pair<T1, T2> &pair, F func) { GC::route(pair.first, func); GC::route(pair.second, func); }
	};

	template<typename ...Types>
	struct router<std::tuple<Types...>>
	{
		template<typename F> static void route(const std::tuple<Types...> &tuple, F func) { GC::__tuple_router<sizeof...(Types), Types...>::__route(tuple, func); }
	};

	template<typename T, typename Deleter>
	struct router<std::unique_ptr<T, Deleter>>
	{
		template<typename F> static void route(const std::unique_ptr<T, Deleter> &obj, F func) { if (obj) GC::route(*obj, func); }
	};

	template<typename T>
	struct router<std::atomic<T>>
	{
		// ill-formed - we would need to read the atomic's value and if the T in atomic<T> uses a gc function on fetch, we could deadlock.
		// you can avoid this problem if you can avoid calling any gc functions, in which case feel free to make a valid specialization.
	};

public: // -- stdlib container router specializations -- //

	// source https://en.cppreference.com/w/cpp/container

	template<typename T, std::size_t N>
	struct router<std::array<T, N>>
	{
		template<typename F> static void route(const std::array<T, N> &arr, F func) { GC::route_range(arr.begin(), arr.end(), func); }
	};

	template<typename T, typename Allocator>
	struct router<std::vector<T, Allocator>>
	{
		template<typename F> static void route(const std::vector<T, Allocator> &vec, F func) { GC::route_range(vec.begin(), vec.end(), func); }
	};

	template<typename T, typename Allocator>
	struct router<std::deque<T, Allocator>>
	{
		template<typename F> static void route(const std::deque<T, Allocator> &deque, F func) { route_range(deque.begin(), deque.end(), func); }
	};

	template<typename T, typename Allocator>
	struct router<std::forward_list<T, Allocator>>
	{
		template<typename F> static void route(const std::forward_list<T, Allocator> &list, F func) { route_range(list.begin(), list.end(), func); }
	};

	template<typename T, typename Allocator>
	struct router<std::list<T, Allocator>>
	{
		template<typename F> static void route(const std::list<T, Allocator> &list, F func) { route_range(list.begin(), list.end(), func); }
	};
	
	template<typename Key, typename Compare, typename Allocator>
	struct router<std::set<Key, Compare, Allocator>>
	{
		template<typename F> static void route(const std::set<Key, Compare, Allocator> &set, F func) { route_range(set.begin(), set.end(), func); }
	};

	template<typename Key, typename Compare, typename Allocator>
	struct router<std::multiset<Key, Compare, Allocator>>
	{
		template<typename F> static void route(const std::multiset<Key, Compare, Allocator> &set, F func) { route_range(set.begin(), set.end(), func); }
	};

	template<typename Key, typename T, typename Compare, typename Allocator>
	struct router<std::map<Key, T, Compare, Allocator>>
	{
		template<typename F> static void route(const std::map<Key, T, Compare, Allocator> &map, F func) { route_range(map.begin(), map.end(), func); }
	};

	template<typename Key, typename T, typename Compare, typename Allocator>
	struct router<std::multimap<Key, T, Compare, Allocator>>
	{
		template<typename F> static void route(const std::multimap<Key, T, Compare, Allocator> &map, F func) { route_range(map.begin(), map.end(), func); }
	};

	template<typename Key, typename Hash, typename KeyEqual, typename Allocator>
	struct router<std::unordered_set<Key, Hash, KeyEqual, Allocator>>
	{
		template<typename F> static void route(const std::unordered_set<Key, Hash, KeyEqual, Allocator> &set, F func) { route_range(set.begin(), set.end(), func); }
	};

	template<typename Key, typename Hash, typename KeyEqual, typename Allocator>
	struct router<std::unordered_multiset<Key, Hash, KeyEqual, Allocator>>
	{
		template<typename F> static void route(const std::unordered_multiset<Key, Hash, KeyEqual, Allocator> &set, F func) { route_range(set.begin(), set.end(), func); }
	};

	template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
	struct router<std::unordered_map<Key, T, Hash, KeyEqual, Allocator>>
	{
		template<typename F> static void route(const std::unordered_map<Key, T, Hash, KeyEqual, Allocator> &map, F func) { route_range(map.begin(), map.end(), func); }
	};

	template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
	struct router<std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator>>
	{
		template<typename F> static void route(const std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator> &map, F func) { route_range(map.begin(), map.end(), func); }
	};

private: // -- std adapter internal container access functions -- //

	// given a std adapter type (stack, queue, priority_queue), gets the underlying container (by reference)
	template<typename Adapter>
	static const auto &__get_adapter_container(const Adapter &adapter)
	{
		// we need access to Adapter's protected data, so create a derived type
		struct HaxAdapter : Adapter
		{
			// given a std adapter type (stack, queue, priority_queue), gets the underlying container (by reference)
			// this construction implicitly performs a static_cast on &HaxAdapter::c to convert it into &Adapter::c.
			// this is safe because HaxAdapter doesn't define anything on its own, so if c exists, it came from Adapter.
			static const auto &__get_adapter_container(const Adapter &adapter) { return adapter.*&HaxAdapter::c; }
		};
		
		return HaxAdapter::__get_adapter_container(adapter);
	}

public: // -- std adapter routers -- //

	template<typename T, typename Container>
	struct router<std::stack<T, Container>>
	{
		template<typename F> static void route(const std::stack<T, Container> &stack, F func)
		{
			const auto &container = __get_adapter_container(stack);
			route_range(container.begin(), container.end(), func);
		}
	};

	template<typename T, typename Container>
	struct router<std::queue<T, Container>>
	{
		template<typename F> static void route(const std::queue<T, Container> &queue, F func)
		{
			const auto &container = __get_adapter_container(queue);
			route_range(container.begin(), container.end(), func);
		}
	};

	template<typename T, typename Container, typename Compare>
	struct router<std::priority_queue<T, Container, Compare>>
	{
		template<typename F> static void route(const std::priority_queue<T, Container, Compare> &pqueue, F func)
		{
			const auto &container = __get_adapter_container(pqueue);
			route_range(container.begin(), container.end(), func);
		}
	};

private: // -- aligned raw memory allocators -- //

	// uses whatever default alignment the stdlib implementation uses (i.e. std::max_align_t).
	// this is more space-efficient than active alignment, but can only be used for certain types.
	struct __passive_aligned_allocator
	{
		static void *alloc(std::size_t size) { return std::malloc(size); }
		static void dealloc(void *p) { std::free(p); }
	};

	// actively aligns blocks of memory to the given alignment.
	// this is less space-efficient than passive alignment, but must be used for over-aligned types.
	template<std::size_t align>
	struct __active_aligned_allocator
	{
		static void *alloc(std::size_t size) { return GC::aligned_malloc(size, align); }
		static void dealloc(void *p) { GC::aligned_free(p); }
	};

	// wrapper for an allocator that additionally performs gc-specific logic
	template<typename allocator_t>
	struct __checked_allocator
	{
		static void *alloc(std::size_t size)
		{
			// allocate the space
			void *buf = allocator_t::alloc(size);

			// if that failed but we have allocfail collect mode enabled
			if (!buf && (int)strategy() & (int)strategies::allocfail)
			{
				// collect and retry the allocation
				GC::collect();
				buf = allocator_t::alloc(size);
			}

			// if that failed, throw bad alloc
			if (!buf) throw std::bad_alloc();

			return buf;
		}
		static void dealloc(void *p) { allocator_t::dealloc(p); }
	};

	// given a type T, returns the type of an aligned allocator that will give the most efficient proper alignment.
	// these functions do not have definitions - they are meant to be used in decltype context.
	template<std::size_t align, std::enable_if_t<(align <= alignof(std::max_align_t)), int> = 0>
	static __checked_allocator<__passive_aligned_allocator> __returns_checked_aligned_allocator_type();

	template<std::size_t align, std::enable_if_t<(align > alignof(std::max_align_t)), int> = 0>
	static __checked_allocator<__active_aligned_allocator<align>> __returns_checked_aligned_allocator_type();

public: // -- ptr allocation -- //

	// creates a new dynamic instance of T that is bound to a ptr.
	// throws any exception resulting from T's constructor but does not leak resources if this occurs.
	template<typename T, typename ...Args, std::enable_if_t<!std::is_array<T>::value, int> = 0>
	static ptr<T> make(Args &&...args)
	{
		// -- normalize T -- //

		// strip cv qualifiers
		typedef std::remove_cv_t<T> element_type;
		
		// get the allocator
		typedef decltype(__returns_checked_aligned_allocator_type<std::max(alignof(element_type), alignof(info))>()) allocator_t;

		// -- create the vtable -- //

		auto router_set = [](info &handle, auto func) { GC::route(*reinterpret_cast<element_type*>(handle.obj), func); };

		static const info_vtable _vtable(
			[](info &handle) { reinterpret_cast<element_type*>(handle.obj)->~element_type(); },
			[](info &handle) { allocator_t::dealloc(handle.obj); },
			router_set,
			router_set
		);

		// -- create the buffer for both the object and its info object -- //

		// allocate the buffer space
		void *const buf = allocator_t::alloc(sizeof(element_type) + sizeof(info));

		// alias the buffer partitions
		element_type *obj = reinterpret_cast<element_type*>(buf);
		info         *handle = reinterpret_cast<info*>(reinterpret_cast<char*>(buf) + sizeof(element_type));

		// -- construct the object -- //

		// try to construct the object
		try { new (obj) element_type(std::forward<Args>(args)...); }
		// if that fails, deallocate buf and rethrow
		catch (...) { allocator_t::dealloc(buf); throw; }

		// construct the info object
		new (handle) info(obj, 1, &_vtable);

		// -- do the garbage collection aspects -- //

		// claim its children
		handle->route(GC::router_unroot);

		// create the ptr first - this roots obj
		ptr<T> res(obj, handle, GC::bind_new_obj);

		// begin timed collection (if it's not already)
		GC::start_timed_collect();

		// return the now-initialized ptr
		return res;
	}

	// creates a new dynamic array of T that is bound to a ptr.
	// throws any exception resulting from any T object's construction, but does not leak resources if this occurs.
	template<typename T, std::enable_if_t<GC::is_unbound_array<T>::value, int> = 0>
	static ptr<T> make(std::size_t count)
	{
		// -- normalize T -- //

		// strip cv qualifiers and first extent to get element type
		typedef std::remove_cv_t<std::remove_extent_t<T>> element_type;

		// strip cv qualifiers and all extents to get scalar type
		typedef std::remove_cv_t<std::remove_all_extents_t<T>> scalar_type;

		// get the total number of scalar objects - we'll build the array in terms of scalar entities
		const std::size_t scalar_count = count * GC::full_extent<T>::value;

		// get the allocator
		typedef decltype(__returns_checked_aligned_allocator_type<std::max(alignof(scalar_type), alignof(info))>()) allocator_t;

		// -- create the vtable -- //

		auto router_set = [](info &handle, auto func) { for (std::size_t i = 0; i < handle.count; ++i) GC::route(reinterpret_cast<scalar_type*>(handle.obj)[i], func); };

		static const info_vtable _vtable(
			[](info &handle) { for (std::size_t i = 0; i < handle.count; ++i) reinterpret_cast<scalar_type*>(handle.obj)[i].~scalar_type(); },
			[](info &handle) { allocator_t::dealloc(handle.obj); },
			router_set,
			router_set
		);

		// -- create the buffer for the objects and their info object -- //

		// allocate the buffer space
		void *const buf = allocator_t::alloc(scalar_count * sizeof(scalar_type) + sizeof(info));

		// alias the buffer partitions
		scalar_type *obj = reinterpret_cast<scalar_type*>(buf);
		info        *handle = reinterpret_cast<info*>(reinterpret_cast<char*>(buf) + scalar_count * sizeof(scalar_type));

		// -- construct the objects -- //

		std::size_t constructed_count = 0; // number of successfully constructed objects

		// try to construct the objects
		try
		{
			for (std::size_t i = 0; i < scalar_count; ++i)
			{
				new (obj + i) scalar_type();
				++constructed_count; // inc constructed_count after each success
			}
		}
		// if that fails
		catch (...)
		{
			// destroy anything we successfully constructed
			for (std::size_t i = 0; i < constructed_count; ++i) (obj + i)->~scalar_type();

			// deallocate the buffer and rethrow whatever killed us
			allocator_t::dealloc(buf);
			throw;
		}

		// construct the info object
		// the cast is safe because element_type is either scalar_type or bound array of scalar_type
		new (handle) info(obj, scalar_count, &_vtable);
		
		// -- do the garbage collection aspects -- //

		// claim its children
		handle->route(GC::router_unroot);

		// create the ptr first - this roots obj
		ptr<T> res(reinterpret_cast<element_type*>(obj), handle, GC::bind_new_obj);

		// begin timed collection (if it's not already)
		GC::start_timed_collect();

		// return the now-initialized ptr
		return res;
	}

	// creates a new dynamic instance of T that is bound to a ptr.
	// throws any exception resulting from any T's constructor but does not leak resources if this occurs.
	template<typename T, std::enable_if_t<GC::is_bound_array<T>::value, int> = 0>
	static ptr<T> make()
	{
		// create it with the dynamic array form and reinterpret it to normal array form
		return reinterpretCast<T>(make<std::remove_extent_t<T>[]>(std::extent<T>::value));
	}

	// adopts a pre-existing scalar instance of T that is, after this call, bound to a ptr.
	// throws any exception resulting from failed memory allocation - in this case the object is deleted.
	// if obj is null, does nothing and returns a null ptr object.
	template<typename T, typename Deleter = std::default_delete<T>>
	static ptr<T> adopt(T *obj)
	{
		// -- verification -- //

		// if obj is null, return an null ptr
		if (!obj) return {};

		// -- normalize T -- //

		// get the allocator
		typedef decltype(__returns_checked_aligned_allocator_type<alignof(info)>()) allocator_t;

		// -- create the vtable -- //

		auto router_set = [](info &handle, auto func) { GC::route(*reinterpret_cast<T*>(handle.obj), func); };

		static const info_vtable _vtable(
			[](info &handle) { Deleter()(reinterpret_cast<T*>(handle.obj)); },
			[](info &handle) { allocator_t::dealloc(&handle); },
			router_set,
			router_set
		);

		// -- create the info object for management -- //

		info *handle;

		// allocate the buffer space for the info object
		try { handle = reinterpret_cast<info*>(allocator_t::alloc(sizeof(info))); }
		// on failure, delete the object and rethrow whatever killed us
		catch (...) { Deleter()(obj); throw; }

		// construct the info object
		new (handle) info(obj, 1, &_vtable);

		// -- do the garbage collection aspects -- //

		// claim its children
		handle->route(GC::router_unroot);

		// create the ptr first - this roots obj
		ptr<T> res(obj, handle, GC::bind_new_obj);

		// begin timed collection (if it's not already)
		GC::start_timed_collect();

		// return the now-initialized ptr
		return res;
	}

	// adopts a pre-existing dynamic array of T that is, after this call, bount to a ptr.
	// throws any expection resulting from failed memory allocation - in this case the objects are deleted.
	// if obj is null, does nothing and returns a null ptr object.
	template<typename T, typename Deleter = std::default_delete<T[]>>
	static ptr<T[]> adopt(T *obj, std::size_t count)
	{
		// -- verification -- //

		// if obj is null, return an null ptr
		if (!obj) return {};

		// -- normalize T -- //

		// get the allocator
		typedef decltype(__returns_checked_aligned_allocator_type<alignof(info)>()) allocator_t;

		// -- create the vtable -- //

		auto router_set = [](info &handle, auto func) { for (std::size_t i = 0; i < handle.count; ++i) GC::route(reinterpret_cast<T*>(handle.obj)[i], func); };

		static const info_vtable _vtable(
			[](info &handle) { Deleter()(reinterpret_cast<T*>(handle.obj)); },
			[](info &handle) { allocator_t::dealloc(&handle); },
			router_set,
			router_set
		);

		// -- create the info object for management -- //

		info *handle;

		// allocate the buffer space for the info object
		try { handle = reinterpret_cast<info*>(allocator_t::alloc(sizeof(info))); }
		// on failure, delete the object and rethrow whatever killed us
		catch (...) { Deleter()(obj); throw; }

		// construct the info object
		new (handle) info(obj, count, &_vtable);

		// -- do the garbage collection aspects -- //

		// claim its children
		handle->route(GC::router_unroot);

		// create the ptr first - this roots obj
		ptr<T[]> res(obj, handle, GC::bind_new_obj);

		// begin timed collection (if it's not already)
		GC::start_timed_collect();

		// return the now-initialized ptr
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

public: // -- mutable std wrappers -- //

	// a wrapper for std::unique_ptr that has an internally-synchronized router function (i.e. ready-to-go for gc).
	// note - this type is not thread safe - only locks enough to satisfy the requirements of a router function for a mutable gc type (see intro comments).
	template<typename T, typename Deleter = std::default_delete<T>>
	class unique_ptr
	{
	private: // -- data -- //

		typedef std::unique_ptr<T, Deleter> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<unique_ptr>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- types -- //

		typedef typename wrapped_t::pointer pointer;

		typedef typename wrapped_t::element_type element_type;

		typedef typename wrapped_t::deleter_type deleter_type;

	public: // -- ctor / dtor -- //

		constexpr unique_ptr() noexcept
		{
			new (buffer) wrapped_t();
		}
		constexpr unique_ptr(std::nullptr_t) noexcept
		{
			new (buffer) wrapped_t(nullptr);
		}

		explicit unique_ptr(pointer p) noexcept
		{
			new (buffer) wrapped_t(p);
		}

		// !! ADD DELETER OBJ CTORS (3-4) https://en.cppreference.com/w/cpp/memory/unique_ptr/unique_ptr

		unique_ptr(unique_ptr &&other) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		unique_ptr(wrapped_t &&other) noexcept
		{
			new (buffer) wrapped_t(std::move(other));
		}

		template<typename U, typename E>
		unique_ptr(unique_ptr<U, E> &&other) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		template<typename U, typename E>
		unique_ptr(std::unique_ptr<U, E> &&other) noexcept
		{
			new (buffer) wrapped_t(std::move(other));
		}

		~unique_ptr()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		unique_ptr &operator=(unique_ptr &&other) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		unique_ptr &operator=(wrapped_t &&other) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		template<typename U, typename E>
		unique_ptr &operator=(unique_ptr<U, E> &&other) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		template<typename U, typename E>
		unique_ptr &operator=(std::unique_ptr<U, E> &&other) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		unique_ptr &operator=(std::nullptr_t) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = nullptr;
			return *this;
		}

	public: // -- management -- //

		pointer release() noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().release();
		}

		void reset(pointer ptr = pointer()) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reset(ptr);
		}

		template<typename U, typename Z = T, std::enable_if_t<std::is_same<T, Z>::value && GC::is_unbound_array<T>::value, int> = 0>
		void reset(U other) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reset(other); // std::unique_ptr doesn't know about cpp-gc stuff, so there's no way this can go wrong mutex-wise
		}

		template<typename Z = T, std::enable_if_t<std::is_same<T, Z>::value && GC::is_unbound_array<T>::value, int> = 0>
		void reset(std::nullptr_t = nullptr) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reset(nullptr);
		}

		void swap(unique_ptr &other) noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(unique_ptr &a, unique_ptr &b) { a.swap(b); }

	public: // -- obj access -- //

		pointer get() const noexcept { return wrapped().get(); }

		decltype(auto) get_deleter() noexcept { return wrapped().get_deleter(); }
		decltype(auto) get_deleter() const noexcept { return wrapped().get_deleter(); }

		explicit operator bool() const noexcept { return static_cast<bool>(wrapped()); }

		decltype(auto) operator*() const { return *wrapped(); }
		decltype(auto) operator->() const noexcept { return wrapped().operator->(); }

		template<typename Z = T, std::enable_if_t<std::is_same<T, Z>::value && GC::is_unbound_array<T>::value, int> = 0>
		decltype(auto) operator[](std::size_t i) const { return wrapped()[i]; }
	};
	template<typename T, typename Deleter>
	struct router<GC::unique_ptr<T, Deleter>>
	{
		template<typename F>
		static void route(const GC::unique_ptr<T, Deleter> &obj, F func)
		{
			std::lock_guard<std::mutex> lock(obj.mutex);
			GC::route(obj.wrapped(), func);
		}
	};

	// -- unique_ptr cmp -- //

	template<typename T1, typename D1, typename T2, typename D2>
	friend bool operator==(const GC::unique_ptr<T1, D1> &a, const GC::unique_ptr<T2, D2> &b) { return a.get() == b.get(); }
	template<typename T1, typename D1, typename T2, typename D2>
	friend bool operator!=(const GC::unique_ptr<T1, D1> &a, const GC::unique_ptr<T2, D2> &b) { return a.get() != b.get(); }
	template<typename T1, typename D1, typename T2, typename D2>
	friend bool operator<(const GC::unique_ptr<T1, D1> &a, const GC::unique_ptr<T2, D2> &b) { return a.get() < b.get(); }
	template<typename T1, typename D1, typename T2, typename D2>
	friend bool operator<=(const GC::unique_ptr<T1, D1> &a, const GC::unique_ptr<T2, D2> &b) { return a.get() <= b.get(); }
	template<typename T1, typename D1, typename T2, typename D2>
	friend bool operator>(const GC::unique_ptr<T1, D1> &a, const GC::unique_ptr<T2, D2> &b) { return a.get() > b.get(); }
	template<typename T1, typename D1, typename T2, typename D2>
	friend bool operator>=(const GC::unique_ptr<T1, D1> &a, const GC::unique_ptr<T2, D2> &b) { return a.get() >= b.get(); }

	template<typename T1, typename D1>
	friend bool operator==(const GC::unique_ptr<T1, D1> &x, std::nullptr_t) { return x.get() == nullptr; }
	template<typename T1, typename D1>
	friend bool operator==(std::nullptr_t, const GC::unique_ptr<T1, D1> &x) { return nullptr == x.get(); }

	template<typename T1, typename D1>
	friend bool operator!=(const GC::unique_ptr<T1, D1> &x, std::nullptr_t) { return x.get() != nullptr; }
	template<typename T1, typename D1>
	friend bool operator!=(std::nullptr_t, const GC::unique_ptr<T1, D1> &x) { return nullptr != x.get(); }

	template<typename T1, typename D1>
	friend bool operator<(const GC::unique_ptr<T1, D1> &x, std::nullptr_t) { return x.get() < nullptr; }
	template<typename T1, typename D1>
	friend bool operator<(std::nullptr_t, const GC::unique_ptr<T1, D1> &x) { return nullptr < x.get(); }

	template<typename T1, typename D1>
	friend bool operator<=(const GC::unique_ptr<T1, D1> &x, std::nullptr_t) { return x.get() <= nullptr; }
	template<typename T1, typename D1>
	friend bool operator<=(std::nullptr_t, const GC::unique_ptr<T1, D1> &x) { return nullptr <= x.get(); }

	template<typename T1, typename D1>
	friend bool operator>(const GC::unique_ptr<T1, D1> &x, std::nullptr_t) { return x.get() > nullptr; }
	template<typename T1, typename D1>
	friend bool operator>(std::nullptr_t, const GC::unique_ptr<T1, D1> &x) { return nullptr > x.get(); }

	template<typename T1, typename D1>
	friend bool operator>=(const GC::unique_ptr<T1, D1> &x, std::nullptr_t) { return x.get() >= nullptr; }
	template<typename T1, typename D1>
	friend bool operator>=(std::nullptr_t, const GC::unique_ptr<T1, D1> &x) { return nullptr >= x.get(); }

	// --------------------------------------------------------------

	template<typename T, typename Allocator = std::allocator<T>>
	class vector
	{
	private: // -- data -- //

		typedef std::vector<T, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<vector>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::value_type value_type;
		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::reverse_iterator reverse_iterator;
		typedef typename wrapped_t::const_reverse_iterator const_reverse_iterator;

	public: // -- ctor / dtor -- //

		vector()
		{
			new (buffer) wrapped_t();
		}
		explicit vector(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		vector(size_type count, const T& value = T(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(count, value, alloc);
		}

		explicit vector(size_type count, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(count, alloc);
		}

		template<typename InputIt>
		vector(InputIt first, InputIt last, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, alloc);
		}

		vector(const vector &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		vector(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		vector(const vector &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		vector(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		vector(vector &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		vector(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		vector(vector &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		vector(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		vector(std::initializer_list<T> init, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, alloc);
		}

		~vector()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		vector &operator=(const vector &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		vector &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		vector &operator=(vector &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		vector &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		vector &operator=(std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

		void assign(size_type count, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(count, value);
		}

		template<typename InputIt>
		void assign(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(first, last);
		}

		void assign(std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(ilist);
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- obj access -- //

		reference at(size_type pos) { return wrapped().at(pos); }
		const_reference at(size_type pos) const { return wrapped().at(pos); }

		reference operator[](size_type pos) { return wrapped()[pos]; }
		const_reference operator[](size_type pos) const { return wrapped()[pos]; }

		reference front() { return wrapped().front(); }
		const_reference front() const { return wrapped().front(); }

		reference back() { return wrapped().back(); }
		const_reference back() const { return wrapped().back(); }

		T *data() noexcept { return wrapped().data(); }
		const T *data() const noexcept { return wrapped().data(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

		reverse_iterator rbegin() noexcept { return wrapped().rbegin(); }
		const_reverse_iterator rbegin() const noexcept { return wrapped().rbegin(); }
		const_reverse_iterator crbegin() const noexcept { return wrapped().crbegin(); }

		reverse_iterator rend() noexcept { return wrapped().rend(); }
		const_reverse_iterator rend() const noexcept { return wrapped().rend(); }
		const_reverse_iterator crend() const noexcept { return wrapped().crend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void reserve(size_type new_cap)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reserve(new_cap);
		}
		size_type capacity() const noexcept { return wrapped().capacity(); }

		void shrink_to_fit()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().shrink_to_fit();
		}

		void clear() noexcept(noexcept(std::declval<std::mutex>().lock()))
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		iterator insert(const_iterator pos, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, value);
		}
		iterator insert(const_iterator pos, T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, std::move(value));
		}

		iterator insert(const_iterator pos, size_type count, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, count, value);
		}

		template<typename InputIt>
		iterator insert(const_iterator pos, InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, first, last);
		}

		iterator insert(const_iterator pos, std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, ilist);
		}

		template<typename ...Args>
		iterator emplace(const_iterator pos, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

	public: // -- push / pop -- //

		void push_back(const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_back(value);
		}
		void push_back(T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_back(std::move(value));
		}

		template<typename ...Args>
		decltype(auto) emplace_back(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_back(std::forward<Args>(args)...);
		}

		void pop_back()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().pop_back();
		}

	public: // -- resize -- //

		void resize(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().resize(count);
		}
		void resize(size_type count, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().resize(count, value);
		}
		
	public: // -- swap -- //

		void swap(vector &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(vector &a, vector &b) { a.swap(b); }

	public: // -- cmp -- //

		friend bool operator==(const vector &a, const vector &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const vector &a, const vector &b) { return a.wrapped() != b.wrapped(); }
		friend bool operator<(const vector &a, const vector &b) { return a.wrapped() < b.wrapped(); }
		friend bool operator<=(const vector &a, const vector &b) { return a.wrapped() <= b.wrapped(); }
		friend bool operator>(const vector &a, const vector &b) { return a.wrapped() > b.wrapped(); }
		friend bool operator>=(const vector &a, const vector &b) { return a.wrapped() >= b.wrapped(); }
	};
	template<typename T, typename Allocator>
	struct router<GC::vector<T, Allocator>>
	{
		template<typename F>
		static void route(const GC::vector<T, Allocator> &vec, F func)
		{
			std::lock_guard<std::mutex> lock(vec.mutex);
			GC::route(vec.wrapped(), func);
		}
	};

	template<typename T, typename Allocator = std::allocator<T>>
	class deque
	{
	private: // -- data -- //

		typedef std::deque<T, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // the router synchronizer

		friend struct GC::router<deque>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::value_type value_type;
		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::reverse_iterator reverse_iterator;
		typedef typename wrapped_t::const_reverse_iterator const_reverse_iterator;

	public: // -- ctor / dtor -- //

		deque()
		{
			new (buffer) wrapped_t();
		}
		explicit deque(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		deque(size_type count, const T &value, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(count, value, alloc);
		}

		explicit deque(size_type count, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(count, alloc);
		}

		template<typename InputIt>
		deque(InputIt first, InputIt last, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, alloc);
		}

		deque(const deque &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		deque(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		deque(const deque &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		deque(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		deque(deque &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		deque(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		deque(deque &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		deque(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		deque(std::initializer_list<T> init, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, alloc);
		}

		~deque()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //
		
		deque &operator=(const deque &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		deque &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		deque &operator=(deque &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		deque &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		deque &operator=(std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

		void assign(size_type count, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(count, value);
		}

		template<typename InputIt>
		void assign(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(first, last);
		}

		void assign(std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(ilist);
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- obj access -- //

		reference at(size_type pos) { return wrapped().at(pos); }
		const_reference at(size_type pos) const { return wrapped().at(pos); }

		reference operator[](size_type pos) { return wrapped()[pos]; }
		const_reference operator[](size_type pos) const { return wrapped()[pos]; }

		reference front() { return wrapped().front(); }
		const_reference front() const { return wrapped().front(); }

		reference back() { return wrapped().back(); }
		const_reference back() const { return wrapped().back(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

		reverse_iterator rbegin() noexcept { return wrapped().rbegin(); }
		const_reverse_iterator rbegin() const noexcept { return wrapped().rbegin(); }
		const_reverse_iterator crbegin() const noexcept { return wrapped().crbegin(); }

		reverse_iterator rend() noexcept { return wrapped().rend(); }
		const_reverse_iterator rend() const noexcept { return wrapped().rend(); }
		const_reverse_iterator crend() const noexcept { return wrapped().crend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void reserve(size_type new_cap)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reserve(new_cap);
		}
		size_type capacity() const noexcept { return wrapped().capacity(); }

		void shrink_to_fit()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().shrink_to_fit();
		}

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		iterator insert(const_iterator pos, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, value);
		}
		iterator insert(const_iterator pos, T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, std::move(value));
		}

		iterator insert(const_iterator pos, size_type count, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, count, value);
		}

		template<typename InputIt>
		iterator insert(const_iterator pos, InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, first, last);
		}

		iterator insert(const_iterator pos, std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, ilist);
		}

		template<typename ...Args>
		iterator emplace(const_iterator pos, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

	public: // -- push / pop -- //

		void push_back(const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_back(value);
		}
		void push_back(T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_back(std::move(value));
		}

		template<typename ...Args>
		decltype(auto) emplace_back(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_back(std::forward<Args>(args)...);
		}

		void pop_back()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().pop_back();
		}

		void push_front(const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_front(value);
		}
		void push_front(T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_front(std::move(value));
		}

		template<typename ...Args>
		decltype(auto) emplace_front(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_front(std::forward<Args>(args)...);
		}

		void pop_front()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().pop_front();
		}

	public: // -- resize -- //

		void resize(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().resize(count);
		}
		void resize(size_type count, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().resize(count, value);
		}

	public: // -- swap -- //

		void swap(deque &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(deque &a, deque &b) { a.swap(b); }

	public: // -- cmp -- //

		friend bool operator==(const deque &a, const deque &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const deque &a, const deque &b) { return a.wrapped() != b.wrapped(); }
		friend bool operator<(const deque &a, const deque &b) { return a.wrapped() < b.wrapped(); }
		friend bool operator<=(const deque &a, const deque &b) { return a.wrapped() <= b.wrapped(); }
		friend bool operator>(const deque &a, const deque &b) { return a.wrapped() > b.wrapped(); }
		friend bool operator>=(const deque &a, const deque &b) { return a.wrapped() >= b.wrapped(); }
	};
	template<typename T, typename Allocator>
	struct router<GC::deque<T, Allocator>>
	{
		template<typename F>
		static void route(const GC::deque<T, Allocator> &vec, F func)
		{
			std::lock_guard<std::mutex> lock(vec.mutex);
			GC::route(vec.wrapped(), func);
		}
	};

	template<typename T, typename Allocator = std::allocator<T>>
	class forward_list
	{
	private: // -- data -- //

		typedef std::forward_list<T, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<forward_list>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::value_type value_type;
		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

	public: // -- ctor / dtor -- //

		forward_list()
		{
			new (buffer) wrapped_t();
		}
		explicit forward_list(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		forward_list(size_type count, const T &value, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(count, value, alloc);
		}

		explicit forward_list(size_type count, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(count, alloc);
		}

		template<typename InputIt>
		forward_list(InputIt first, InputIt last, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, alloc);
		}

		forward_list(const forward_list &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		forward_list(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		forward_list(const forward_list &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		forward_list(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		forward_list(forward_list &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		forward_list(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		forward_list(forward_list &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		forward_list(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		forward_list(std::initializer_list<T> init, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, alloc);
		}

		~forward_list()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		forward_list &operator=(const forward_list &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		forward_list &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		forward_list &operator=(forward_list &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		forward_list &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		forward_list &operator=(std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

		void assign(size_type count, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(count, value);
		}

		template<typename InputIt>
		void assign(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(first, last);
		}

		void assign(std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(ilist);
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- obj access -- //

		reference front() { return wrapped().front(); }
		const_reference front() const { return wrapped().front(); }

	public: // -- iterators -- //

		iterator before_begin() noexcept { return wrapped().before_begin(); }
		const_iterator before_begin() const noexcept { return wrapped().before_begin(); }
		const_iterator cbefore_begin() const noexcept { return wrapped().cbefore_begin(); }

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		iterator insert_after(const_iterator pos, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_after(pos, value);
		}
		iterator insert_after(const_iterator pos, T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_after(pos, std::move(value));
		}

		iterator insert_after(const_iterator pos, size_type count, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_after(pos, count, value);
		}

		template<typename InputIt>
		iterator insert_after(const_iterator pos, InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_after(pos, first, last);
		}

		iterator insert_after(const_iterator pos, std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_after(pos, ilist);
		}

		template<typename ...Args>
		iterator emplace_after(const_iterator pos, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_after(pos, std::forward<Args>(args)...);
		}

		iterator erase_after(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase_after(pos);
		}
		iterator erase_after(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase_after(first, last);
		}

	public: // -- push / pop -- //

		void push_front(const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_front(value);
		}
		void push_front(T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_front(std::move(value));
		}

		template<typename ...Args>
		decltype(auto) emplace_front(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_front(std::forward<Args>(args)...);
		}

		void pop_front()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().pop_front();
		}

	public: // -- resize -- //

		void resize(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().resize(count);
		}
		void resize(size_type count, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().resize(count, value);
		}

	public: // -- swap -- //

		void swap(forward_list &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(forward_list &a, forward_list &b) { a.swap(b); }

	public: // -- cmp -- //

		friend bool operator==(const forward_list &a, const forward_list &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const forward_list &a, const forward_list &b) { return a.wrapped() != b.wrapped(); }
		friend bool operator<(const forward_list &a, const forward_list &b) { return a.wrapped() < b.wrapped(); }
		friend bool operator<=(const forward_list &a, const forward_list &b) { return a.wrapped() <= b.wrapped(); }
		friend bool operator>(const forward_list &a, const forward_list &b) { return a.wrapped() > b.wrapped(); }
		friend bool operator>=(const forward_list &a, const forward_list &b) { return a.wrapped() >= b.wrapped(); }

	public: // -- merge -- //

		void merge(forward_list &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().merge(other.wrapped());
		}
		void merge(forward_list &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().merge(std::move(other.wrapped()));
		}

		template<typename Compare>
		void merge(forward_list &other, Compare comp)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().merge(other.wrapped(), comp);
		}
		template<typename Compare>
		void merge(forward_list &&other, Compare comp)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().merge(std::move(other.wrapped()), comp);
		}

		// ------------------------------------------------------------

		void merge(wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().merge(other);
		}
		void merge(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().merge(std::move(other));
		}

		template<typename Compare>
		void merge(wrapped_t &other, Compare comp)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().merge(other, comp);
		}
		template<typename Compare>
		void merge(wrapped_t &&other, Compare comp)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().merge(std::move(other), comp);
		}

	public: // -- splice -- //

		void splice_after(const_iterator pos, forward_list &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped());
		}
		void splice_after(const_iterator pos, forward_list &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()));
		}

		void splice_after(const_iterator pos, forward_list &other, const_iterator it)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped(), it);
		}
		void splice_after(const_iterator pos, forward_list &&other, const_iterator it)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()), it);
		}

		void splice_after(const_iterator pos, forward_list &other, const_iterator first, const_iterator last)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped(), first, last);
		}
		void splice_after(const_iterator pos, forward_list &&other, const_iterator first, const_iterator last)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()), first, last);
		}

		// ------------------------------------------------------------

		void splice_after(const_iterator pos, wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, other);
		}
		void splice_after(const_iterator pos, wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, std::move(other));
		}

		void splice_after(const_iterator pos, wrapped_t &other, const_iterator it)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, other, it);
		}
		void splice_after(const_iterator pos, wrapped_t &&other, const_iterator it)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, std::move(other), it);
		}

		void splice_after(const_iterator pos, wrapped_t &other, const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, other, first, last);
		}
		void splice_after(const_iterator pos, wrapped_t &&other, const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, std::move(other), first, last);
		}

	public: // -- remove -- //

		decltype(auto) remove(const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().remove(value);
		}

		template<typename UnaryPredicate>
		decltype(auto) remove_if(UnaryPredicate p)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().remove_if(p);
		}

	public: // -- ordering -- //

		void reverse() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reverse();
		}

		decltype(auto) unique()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().unique();
		}
		template<typename BinaryPredicate>
		decltype(auto) unique(BinaryPredicate p)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().unique(p);
		}

		void sort()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().sort();
		}
		template<typename Compare>
		void sort(Compare comp)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().sort(comp);
		}
	};
	template<typename T, typename Allocator>
	struct router<GC::forward_list<T, Allocator>>
	{
		template<typename F>
		static void route(const GC::forward_list<T, Allocator> &list, F func)
		{
			std::lock_guard<std::mutex> lock(list.mutex);
			GC::route(list.wrapped(), func);
		}
	};

	template<typename T, typename Allocator = std::allocator<T>>
	class list
	{
	private: // -- data -- //

		typedef std::list<T, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<list>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::value_type value_type;
		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;
		
		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::reverse_iterator reverse_iterator;
		typedef typename wrapped_t::const_reverse_iterator const_reverse_iterator;

	public: // -- ctor / dtor -- //

		list()
		{
			new (buffer) wrapped_t();
		}
		explicit list(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		list(size_type count, const T &value = T(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(count, value, alloc);
		}

		explicit list(size_type count, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(count, alloc);
		}

		template<typename InputIt>
		list(InputIt first, InputIt last, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, alloc);
		}

		list(const list &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		list(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		list(const list &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		list(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		list(list &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		list(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		list(list &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		list(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		list(std::initializer_list<T> init, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, alloc);
		}

		~list()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		list &operator=(const list &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		list &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		list &operator=(list &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		list &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		list &operator=(std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

		void assign(size_type count, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(count, value);
		}

		template<typename InputIt>
		void assign(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(first, last);
		}

		void assign(std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().assign(ilist);
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- obj access -- //

		reference front() { return wrapped().front(); }
		const_reference front() const { return wrapped().front(); }

		reference back() { return wrapped().back(); }
		const_reference back() const { return wrapped().back(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

		reverse_iterator rbegin() noexcept { return wrapped().rbegin(); }
		const_reverse_iterator rbegin() const noexcept { return wrapped().rbegin(); }
		const_reverse_iterator crbegin() const noexcept { return wrapped().crbegin(); }

		reverse_iterator rend() noexcept { return wrapped().rend(); }
		const_reverse_iterator rend() const noexcept { return wrapped().rend(); }
		const_reverse_iterator crend() const noexcept { return wrapped().crend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		iterator insert(const_iterator pos, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, value);
		}
		iterator insert(const_iterator pos, T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, std::move(value));
		}

		iterator insert(const_iterator pos, size_type count, const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, count, value);
		}

		template<typename InputIt>
		iterator insert(const_iterator pos, InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, first, last);
		}

		iterator insert(const_iterator pos, std::initializer_list<T> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(pos, ilist);
		}

		template<typename ...Args>
		iterator emplace(const_iterator pos, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(pos, std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

	public: // -- push / pop -- //

		void push_back(const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_back(value);
		}
		void push_back(T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_back(std::move(value));
		}

		template<typename ...Args>
		decltype(auto) emplace_back(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_back(std::forward<Args>(args)...);
		}

		void pop_back()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().pop_back();
		}

		void push_front(const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_front(value);
		}
		void push_front(T &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().push_front(std::move(value));
		}

		template<typename ...Args>
		decltype(auto) emplace_front(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_front(std::forward<Args>(args)...);
		}

		void pop_front()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().pop_front();
		}

	public: // -- resize -- //

		void resize(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().resize(count);
		}
		void resize(size_type count, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().resize(count, value);
		}

	public: // -- swap -- //

		void swap(list &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(list &a, list &b) { a.swap(b); }

	public: // -- cmp -- //

		friend bool operator==(const list &a, const list &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const list &a, const list &b) { return a.wrapped() != b.wrapped(); }
		friend bool operator<(const list &a, const list &b) { return a.wrapped() < b.wrapped(); }
		friend bool operator<=(const list &a, const list &b) { return a.wrapped() <= b.wrapped(); }
		friend bool operator>(const list &a, const list &b) { return a.wrapped() > b.wrapped(); }
		friend bool operator>=(const list &a, const list &b) { return a.wrapped() >= b.wrapped(); }

	public: // -- merge -- //

		void merge(list &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().merge(other.wrapped());
		}
		void merge(list &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().merge(std::move(other.wrapped()));
		}

		template<typename Compare>
		void merge(list &other, Compare comp)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().merge(other.wrapped(), comp);
		}
		template<typename Compare>
		void merge(list &&other, Compare comp)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().merge(std::move(other.wrapped()), comp);
		}

		// ----------------------------------------------------------

		void merge(wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().merge(other);
		}
		void merge(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().merge(std::move(other));
		}

		template<typename Compare>
		void merge(wrapped_t &other, Compare comp)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().merge(other, comp);
		}
		template<typename Compare>
		void merge(wrapped_t &&other, Compare comp)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().merge(std::move(other), comp);
		}

	public: // -- splice -- //

		void splice_after(const_iterator pos, list &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped());
		}
		void splice_after(const_iterator pos, list &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()));
		}

		void splice_after(const_iterator pos, list &other, const_iterator it)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped(), it);
		}
		void splice_after(const_iterator pos, list &&other, const_iterator it)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()), it);
		}

		void splice_after(const_iterator pos, list &other, const_iterator first, const_iterator last)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped(), first, last);
		}
		void splice_after(const_iterator pos, list &&other, const_iterator first, const_iterator last)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()), first, last);
		}

		// ----------------------------------------------------------

		void splice_after(const_iterator pos, wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, other);
		}
		void splice_after(const_iterator pos, wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, std::move(other));
		}

		void splice_after(const_iterator pos, wrapped_t &other, const_iterator it)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, other, it);
		}
		void splice_after(const_iterator pos, wrapped_t &&other, const_iterator it)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, std::move(other), it);
		}

		void splice_after(const_iterator pos, wrapped_t &other, const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, other, first, last);
		}
		void splice_after(const_iterator pos, wrapped_t &&other, const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().splice_after(pos, std::move(other), first, last);
		}

	public: // -- remove -- //

		decltype(auto) remove(const T &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().remove(value);
		}

		template<typename UnaryPredicate>
		decltype(auto) remove_if(UnaryPredicate p)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().remove_if(p);
		}

	public: // -- ordering -- //

		void reverse() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reverse();
		}

		decltype(auto) unique()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().unique();
		}
		template<typename BinaryPredicate>
		decltype(auto) unique(BinaryPredicate p)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().unique(p);
		}

		void sort()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().sort();
		}
		template<typename Compare>
		void sort(Compare comp)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().sort(comp);
		}
	};
	template<typename T, typename Allocator>
	struct router<GC::list<T, Allocator>>
	{
		template<typename F>
		static void route(const GC::list<T, Allocator> &list, F func)
		{
			std::lock_guard<std::mutex> lock(list.mutex);
			GC::route(list.wrapped(), func);
		}
	};

	template<typename Key, typename Compare = std::less<Key>, typename Allocator = std::allocator<Key>>
	class set
	{
	private: // -- data -- //

		typedef std::set<Key, Compare, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<set>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::key_type key_type;
		typedef typename wrapped_t::value_type value_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::key_compare key_compare;
		typedef typename wrapped_t::value_compare value_compare;

		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::reverse_iterator reverse_iterator;
		typedef typename wrapped_t::const_reverse_iterator const_reverse_iterator;

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		typedef typename wrapped_t::node_type node_type;
		typedef typename wrapped_t::insert_return_type insert_return_type;
		#endif

	public: // -- ctor / dtor -- //

		set()
		{
			new (buffer) wrapped_t();
		}
		explicit set(const Compare &comp, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(comp, alloc);
		}
		explicit set(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		template<typename InputIt>
		set(InputIt first, InputIt last, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, comp, alloc);
		}
		template<typename InputIt>
		set(InputIt first, InputIt last, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, alloc);
		}

		set(const set &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		set(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		set(const set &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		set(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		set(set &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		set(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		set(set &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		set(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		set(std::initializer_list<value_type> init, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, comp, alloc);
		}
		set(std::initializer_list<value_type> init, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, alloc);
		}

		~set()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		set &operator=(const set &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		set &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		set &operator=(set &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		set &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		set &operator=(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

		reverse_iterator rbegin() noexcept { return wrapped().rbegin(); }
		const_reverse_iterator rbegin() const noexcept { return wrapped().rbegin(); }
		const_reverse_iterator crbegin() const noexcept { return wrapped().crbegin(); }

		reverse_iterator rend() noexcept { return wrapped().rend(); }
		const_reverse_iterator rend() const noexcept { return wrapped().rend(); }
		const_reverse_iterator crend() const noexcept { return wrapped().crend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		std::pair<iterator, bool> insert(const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(value);
		}
		std::pair<iterator, bool> insert(value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(value));
		}

		iterator insert(const_iterator hint, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, value);
		}
		iterator insert(const_iterator hint, value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(value));
		}

		template<typename InputIt>
		void insert(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(first, last);
		}

		void insert(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(ilist);
		}

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		insert_return_type insert(node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(nh));
		}
		iterator insert(const_iterator hint, node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(nh));
		}

		#endif

		template<typename ...Args>
		std::pair<iterator, bool> emplace(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator emplace_hint(const_iterator hint, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

		size_type erase(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(key);
		}

	public: // -- swap -- //

		void swap(set &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(set &a, set &b) { a.swap(b); }

	public: // -- extract -- //

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		node_type extract(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(pos);
		}
		node_type extract(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(key);
		}

		#endif

		// !! ADD MERGE FUNCTIONS (C++17)

	public: // -- lookup -- //

		size_type count(const Key &key) const { return wrapped().count(key); }
		template<typename K>
		size_type count(const K &key) const { return wrapped().count(key); }

		iterator find(const Key &key) { return wrapped().find(key); }
		const_iterator find(const Key &key) const { return wrapped().find(key); }

		template<typename K>
		iterator find(const K &key) { return wrapped().find(key); }
		template<typename K>
		const_iterator find(const K &key) const { return wrapped().find(key); }

		bool contains(const Key &key) const { return wrapped().contains(key); }
		template<typename K>
		bool contains(const K &key) const { return wrapped().contains(key); }

		std::pair<iterator, iterator> equal_range(const Key &key) { return wrapped().equal_range(key); }
		std::pair<const_iterator, const_iterator> equal_range(const Key &key) const { return wrapped().equal_range(key); }

		template<typename K>
		std::pair<iterator, iterator> equal_range(const K &key) { return wrapped().equal_range(key); }
		template<typename K>
		std::pair<const_iterator, const_iterator> equal_range(const K &key) const { return wrapped().equal_range(key); }

		iterator lower_bound(const Key &key) { return wrapped().lower_bound(key); }
		const_iterator lower_bound(const Key &key) const { return wrapped().lower_bound(key); }

		template<typename K>
		iterator lower_bound(const K &key) { return wrapped().lower_bound(key); }
		template<typename K>
		const_iterator lower_bound(const K &key) const { return wrapped().lower_bound(key); }

		iterator upper_bound(const Key &key) { return wrapped().upper_bound(key); }
		const_iterator upper_bound(const Key &key) const { return wrapped().upper_bound(key); }

		template<typename K>
		iterator upper_bound(const K &key) { return wrapped().upper_bound(key); }
		template<typename K>
		const_iterator upper_bound(const K &key) const { return wrapped().upper_bound(key); }

	public: // -- cmp types -- //

		key_compare key_comp() const { return wrapped().key_comp(); }
		value_compare value_comp() const { return wrapped().value_comp(); }

	public: // -- cmp -- //

		friend bool operator==(const set &a, const set &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const set &a, const set &b) { return a.wrapped() != b.wrapped(); }
		friend bool operator<(const set &a, const set &b) { return a.wrapped() < b.wrapped(); }
		friend bool operator<=(const set &a, const set &b) { return a.wrapped() <= b.wrapped(); }
		friend bool operator>(const set &a, const set &b) { return a.wrapped() > b.wrapped(); }
		friend bool operator>=(const set &a, const set &b) { return a.wrapped() >= b.wrapped(); }
	};
	template<typename Key, typename Compare, typename Allocator>
	struct router<GC::set<Key, Compare, Allocator>>
	{
		template<typename F>
		static void route(const GC::set<Key, Compare, Allocator> &set, F func)
		{
			std::lock_guard<std::mutex> lock(set.mutex);
			GC::route(set.wrapped(), func);
		}
	};

	template<typename Key, typename Compare = std::less<Key>, typename Allocator = std::allocator<Key>>
	class multiset
	{
	private: // -- data -- //

		typedef std::multiset<Key, Compare, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<multiset>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::key_type key_type;
		typedef typename wrapped_t::value_type value_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::key_compare key_compare;
		typedef typename wrapped_t::value_compare value_compare;

		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::reverse_iterator reverse_iterator;
		typedef typename wrapped_t::const_reverse_iterator const_reverse_iterator;

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		typedef typename wrapped_t::node_type node_type;
		#endif

	public: // -- ctor / dtor -- //

		multiset()
		{
			new (buffer) wrapped_t();
		}
		explicit multiset(const Compare &comp, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(comp, alloc);
		}
		explicit multiset(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		template<typename InputIt>
		multiset(InputIt first, InputIt last, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, comp, alloc);
		}
		template<typename InputIt>
		multiset(InputIt first, InputIt last, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, alloc);
		}

		multiset(const multiset &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		multiset(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		multiset(const multiset &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		multiset(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		multiset(multiset &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		multiset(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		multiset(multiset &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		multiset(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		multiset(std::initializer_list<value_type> init, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, comp, alloc);
		}
		multiset(std::initializer_list<value_type> init, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, alloc);
		}

		~multiset()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		multiset &operator=(const multiset &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		multiset &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		multiset &operator=(multiset &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		multiset &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		multiset &operator=(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

		reverse_iterator rbegin() noexcept { return wrapped().rbegin(); }
		const_reverse_iterator rbegin() const noexcept { return wrapped().rbegin(); }
		const_reverse_iterator crbegin() const noexcept { return wrapped().crbegin(); }

		reverse_iterator rend() noexcept { return wrapped().rend(); }
		const_reverse_iterator rend() const noexcept { return wrapped().rend(); }
		const_reverse_iterator crend() const noexcept { return wrapped().crend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		iterator insert(const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(value);
		}
		iterator insert(value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(value));
		}

		iterator insert(const_iterator hint, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, value);
		}
		iterator insert(const_iterator hint, value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(value));
		}

		template<typename InputIt>
		void insert(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(first, last);
		}

		void insert(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(ilist);
		}

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		iterator insert(node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(nh));
		}
		iterator insert(const_iterator hint, node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(nh));
		}

		#endif

		template<typename ...Args>
		std::pair<iterator, bool> emplace(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator emplace_hint(const_iterator hint, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

		size_type erase(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(key);
		}

	public: // -- swap -- //

		void swap(multiset &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(multiset &a, multiset &b) { a.swap(b); }

	public: // -- extract -- //

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		node_type extract(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(pos);
		}
		node_type extract(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(key);
		}
		#endif

		// !! ADD MERGE FUNCTIONS (C++17)

	public: // -- lookup -- //

		size_type count(const Key &key) const { return wrapped().count(key); }
		template<typename K>
		size_type count(const K &key) const { return wrapped().count(key); }

		iterator find(const Key &key) { return wrapped().find(key); }
		const_iterator find(const Key &key) const { return wrapped().find(key); }

		template<typename K>
		iterator find(const K &key) { return wrapped().find(key); }
		template<typename K>
		const_iterator find(const K &key) const { return wrapped().find(key); }

		bool contains(const Key &key) const { return wrapped().contains(key); }
		template<typename K>
		bool contains(const K &key) const { return wrapped().contains(key); }

		std::pair<iterator, iterator> equal_range(const Key &key) { return wrapped().equal_range(key); }
		std::pair<const_iterator, const_iterator> equal_range(const Key &key) const { return wrapped().equal_range(key); }

		template<typename K>
		std::pair<iterator, iterator> equal_range(const K &key) { return wrapped().equal_range(key); }
		template<typename K>
		std::pair<const_iterator, const_iterator> equal_range(const K &key) const { return wrapped().equal_range(key); }

		iterator lower_bound(const Key &key) { return wrapped().lower_bound(key); }
		const_iterator lower_bound(const Key &key) const { return wrapped().lower_bound(key); }

		template<typename K>
		iterator lower_bound(const K &key) { return wrapped().lower_bound(key); }
		template<typename K>
		const_iterator lower_bound(const K &key) const { return wrapped().lower_bound(key); }

		iterator upper_bound(const Key &key) { return wrapped().upper_bound(key); }
		const_iterator upper_bound(const Key &key) const { return wrapped().upper_bound(key); }

		template<typename K>
		iterator upper_bound(const K &key) { return wrapped().upper_bound(key); }
		template<typename K>
		const_iterator upper_bound(const K &key) const { return wrapped().upper_bound(key); }

	public: // -- cmp types -- //

		key_compare key_comp() const { return wrapped().key_comp(); }
		value_compare value_comp() const { return wrapped().value_comp(); }

	public: // -- cmp -- //

		friend bool operator==(const multiset &a, const multiset &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const multiset &a, const multiset &b) { return a.wrapped() != b.wrapped(); }
		friend bool operator<(const multiset &a, const multiset &b) { return a.wrapped() < b.wrapped(); }
		friend bool operator<=(const multiset &a, const multiset &b) { return a.wrapped() <= b.wrapped(); }
		friend bool operator>(const multiset &a, const multiset &b) { return a.wrapped() > b.wrapped(); }
		friend bool operator>=(const multiset &a, const multiset &b) { return a.wrapped() >= b.wrapped(); }
	};
	template<typename Key, typename Compare, typename Allocator>
	struct router<GC::multiset<Key, Compare, Allocator>>
	{
		template<typename F>
		static void route(const GC::multiset<Key, Compare, Allocator> &set, F func)
		{
			std::lock_guard<std::mutex> lock(set.mutex);
			GC::route(set.wrapped(), func);
		}
	};

	template<typename Key, typename T, typename Compare = std::less<Key>, typename Allocator = std::allocator<std::pair<const Key, T>>>
	class map
	{
	private: // -- data -- //

		typedef std::map<Key, T, Compare, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<map>;
		
	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::key_type key_type;
		typedef typename wrapped_t::mapped_type mapped_type;

		typedef typename wrapped_t::value_type value_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::key_compare key_compare;

		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::reverse_iterator reverse_iterator;
		typedef typename wrapped_t::const_reverse_iterator const_reverse_iterator;

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		typedef typename wrapped_t::node_type node_type;
		typedef typename wrapped_t::insert_return_type insert_return_type;
		#endif

		typedef typename wrapped_t::value_compare value_compare; // alias map's nested value_compare class

	public: // -- ctor / dtor -- //

		map()
		{
			new (buffer) wrapped_t();
		}
		explicit map(const Compare &comp, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(comp, alloc);
		}
		explicit map(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		template<typename InputIt>
		map(InputIt first, InputIt last, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, comp, alloc);
		}
		template<typename InputIt>
		map(InputIt first, InputIt last, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, alloc);
		}

		map(const map &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		map(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		map(const map &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		map(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		map(map &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		map(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		map(map &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		map(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		map(std::initializer_list<value_type> init, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, comp, alloc);
		}
		map(std::initializer_list<value_type> init, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, alloc);
		}

		~map()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		map &operator=(const map &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		map &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		map &operator=(map &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		map &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		map &operator=(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- element access -- //

		T &at(const Key &key) { return wrapped().at(key); }
		const T &at(const Key &key) const { return wrapped().at(key); }

		T &operator[](const Key &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex); // operator[] performs an insertion if key doesn't exist
			return wrapped()[key];
		}
		T &operator[](Key &&key)
		{
			std::lock_guard<std::mutex> lock(this->mutex); // operator[] performs an insertion if key doesn't exist
			return wrapped()[std::move(key)];
		}

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

		reverse_iterator rbegin() noexcept { return wrapped().rbegin(); }
		const_reverse_iterator rbegin() const noexcept { return wrapped().rbegin(); }
		const_reverse_iterator crbegin() const noexcept { return wrapped().crbegin(); }

		reverse_iterator rend() noexcept { return wrapped().rend(); }
		const_reverse_iterator rend() const noexcept { return wrapped().rend(); }
		const_reverse_iterator crend() const noexcept { return wrapped().crend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		std::pair<iterator, bool> insert(const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(value);
		}
		template<typename P>
		std::pair<iterator, bool> insert(P &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::forward<P>(value));
		}

		iterator insert(const_iterator hint, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, value);
		}

		template<typename P>
		iterator insert(const_iterator hint, P &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::forward<P>(value));
		}

		template<typename InputIt>
		void insert(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(first, last);
		}

		void insert(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(ilist);
		}

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		std::pair<iterator, bool> insert(value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(value));
		}
		iterator insert(const_iterator hint, value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(wrapped));
		}

		insert_return_type insert(node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(nh));
		}
		iterator insert(const_iterator hint, node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(nh));
		}

		template<typename M>
		std::pair<iterator, bool> insert_or_assign(const key_type &k, M &&obj)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_or_assign(k, std::forward<M>(obj));
		}
		template<typename M>
		std::pair<iterator, bool> insert_or_assign(key_type &&k, M &&obj)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_or_assign(std::move(k), std::forward<M>(obj));
		}

		template<typename M>
		std::pair<iterator, bool> insert_or_assign(const_iterator hint, const key_type &k, M &&obj)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_or_assign(hint, k, std::forward<M>(obj));
		}
		template<typename M>
		std::pair<iterator, bool> insert_or_assign(const_iterator hint, key_type &&k, M &&obj)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_or_assign(hint, std::move(k), std::forward<M>(obj));
		}

		template<typename ...Args>
		std::pair<iterator, bool> try_emplace(const key_type &k, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().try_emplace(k, std::forward<Args>(args)...);
		}
		template<typename ...Args>
		std::pair<iterator, bool> try_emplace(key_type &&k, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().try_emplace(std::move(k), std::forward<Args>(args)...);
		}

		template<typename ...Args>
		iterator try_emplace(const_iterator hint, const key_type &k, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().try_emplace(hint, k, std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator try_emplace(const_iterator hint, key_type &&k, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().try_emplace(hint, std::move(k), std::forward<Args>(args)...);
		}

		#endif

		template<typename ...Args>
		std::pair<iterator, bool> emplace(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator emplace_hint(const_iterator hint, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

		size_type erase(const key_type &k)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(k);
		}

	public: // -- swap -- //

		void swap(map &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(map &a, map &b) { a.swap(b); }

	public: // -- extract -- //

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		node_type extract(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(pos);
		}
		node_type extract(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(key);
		}

		#endif

		// !! ADD MERGE FUNCTIONS (C++17)

	public: // -- lookup -- //

		size_type count(const Key &key) const { return wrapped().count(key); }
		template<typename K>
		size_type count(const K &key) const { return wrapped().count(key); }

		iterator find(const Key &key) { return wrapped().find(key); }
		const_iterator find(const Key &key) const { return wrapped().find(key); }

		template<typename K>
		iterator find(const K &key) { return wrapped().find(key); }
		template<typename K>
		const_iterator find(const K &key) const { return wrapped().find(key); }

		bool contains(const Key &key) const { return wrapped().contains(key); }
		template<typename K>
		bool contains(const K &key) const { return wrapped().contains(key); }

		std::pair<iterator, iterator> equal_range(const Key &key) { return wrapped().equal_range(key); }
		std::pair<const_iterator, const_iterator> equal_range(const Key &key) const { return wrapped().equal_range(key); }

		template<typename K>
		std::pair<iterator, iterator> equal_range(const K &key) { return wrapped().equal_range(key); }
		template<typename K>
		std::pair<const_iterator, const_iterator> equal_range(const K &key) const { return wrapped().equal_range(key); }

		iterator lower_bound(const Key &key) { return wrapped().lower_bound(key); }
		const_iterator lower_bound(const Key &key) const { return wrapped().lower_bound(key); }

		template<typename K>
		iterator lower_bound(const K &key) { return wrapped().lower_bound(key); }
		template<typename K>
		const_iterator lower_bound(const K &key) const { return wrapped().lower_bound(key); }

		iterator upper_bound(const Key &key) { return wrapped().upper_bound(key); }
		const_iterator upper_bound(const Key &key) const { return wrapped().upper_bound(key); }

		template<typename K>
		iterator upper_bound(const K &key) { return wrapped().upper_bound(key); }
		template<typename K>
		const_iterator upper_bound(const K &key) const { return wrapped().upper_bound(key); }

	public: // -- cmp types -- //

		key_compare key_comp() const { return wrapped().key_comp(); }
		value_compare value_comp() const { return wrapped().value_comp(); }

	public: // -- cmp -- //

		friend bool operator==(const map &a, const map &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const map &a, const map &b) { return a.wrapped() != b.wrapped(); }
		friend bool operator<(const map &a, const map &b) { return a.wrapped() < b.wrapped(); }
		friend bool operator<=(const map &a, const map &b) { return a.wrapped() <= b.wrapped(); }
		friend bool operator>(const map &a, const map &b) { return a.wrapped() > b.wrapped(); }
		friend bool operator>=(const map &a, const map &b) { return a.wrapped() >= b.wrapped(); }
	};
	template<typename Key, typename T, typename Compare, typename Allocator>
	struct router<GC::map<Key, T, Compare, Allocator>>
	{
		template<typename F>
		static void route(const GC::map<Key, T, Compare, Allocator> &map, F func)
		{
			std::lock_guard<std::mutex> lock(map.mutex);
			GC::route(map.wrapped(), func);
		}
	};

	template<typename Key, typename T, typename Compare = std::less<Key>, typename Allocator = std::allocator<std::pair<const Key, T>>>
	class multimap
	{
	private: // -- data -- //

		typedef std::multimap<Key, T, Compare, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<multimap>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::key_type key_type;
		typedef typename wrapped_t::mapped_type mapped_type;

		typedef typename wrapped_t::value_type value_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::key_compare key_compare;

		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::reverse_iterator reverse_iterator;
		typedef typename wrapped_t::const_reverse_iterator const_reverse_iterator;

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		typedef typename wrapped_t::node_type node_type;
		#endif

		typedef typename wrapped_t::value_compare value_compare; // alias multimap's nested value_compare class

	public: // -- ctor / dtor -- //

		multimap()
		{
			new (buffer) wrapped_t();
		}
		explicit multimap(const Compare &comp, const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(comp, alloc);
		}
		explicit multimap(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		template<typename InputIt>
		multimap(InputIt first, InputIt last, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, comp, alloc);
		}
		template<typename InputIt>
		multimap(InputIt first, InputIt last, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, alloc);
		}

		multimap(const multimap &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		multimap(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		multimap(const multimap &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		multimap(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		multimap(multimap &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		multimap(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		multimap(multimap &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		multimap(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		multimap(std::initializer_list<value_type> init, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, comp, alloc);
		}
		multimap(std::initializer_list<value_type> init, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, alloc);
		}

		~multimap()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		multimap &operator=(const multimap &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		multimap &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		multimap &operator=(multimap &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		multimap &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		multimap &operator=(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

		reverse_iterator rbegin() noexcept { return wrapped().rbegin(); }
		const_reverse_iterator rbegin() const noexcept { return wrapped().rbegin(); }
		const_reverse_iterator crbegin() const noexcept { return wrapped().crbegin(); }

		reverse_iterator rend() noexcept { return wrapped().rend(); }
		const_reverse_iterator rend() const noexcept { return wrapped().rend(); }
		const_reverse_iterator crend() const noexcept { return wrapped().crend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		iterator insert(const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(value);
		}
		template<typename P>
		iterator insert(P &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::forward<P>(value));
		}

		iterator insert(const_iterator hint, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, value);
		}

		template<typename P>
		iterator insert(const_iterator hint, P &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::forward<P>(value));
		}

		template<typename InputIt>
		void insert(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(first, last);
		}

		void insert(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(ilist);
		}

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		iterator insert(value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(value));
		}
		iterator insert(const_iterator hint, value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(wrapped));
		}

		iterator insert(node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(nh));
		}
		iterator insert(const_iterator hint, node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(nh));
		}

		#endif

		template<typename ...Args>
		iterator emplace(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator emplace_hint(const_iterator hint, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

		size_type erase(const key_type &k)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(k);
		}

	public: // -- swap -- //

		void swap(multimap &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(multimap &a, multimap &b) { a.swap(b); }

	public: // -- extract -- //

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		node_type extract(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(pos);
		}
		node_type extract(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(key);
		}

		#endif

		// !! ADD MERGE FUNCTIONS (C++17)

	public: // -- lookup -- //

		size_type count(const Key &key) const { return wrapped().count(key); }
		template<typename K>
		size_type count(const K &key) const { return wrapped().count(key); }

		iterator find(const Key &key) { return wrapped().find(key); }
		const_iterator find(const Key &key) const { return wrapped().find(key); }

		template<typename K>
		iterator find(const K &key) { return wrapped().find(key); }
		template<typename K>
		const_iterator find(const K &key) const { return wrapped().find(key); }

		bool contains(const Key &key) const { return wrapped().contains(key); }
		template<typename K>
		bool contains(const K &key) const { return wrapped().contains(key); }

		std::pair<iterator, iterator> equal_range(const Key &key) { return wrapped().equal_range(key); }
		std::pair<const_iterator, const_iterator> equal_range(const Key &key) const { return wrapped().equal_range(key); }

		template<typename K>
		std::pair<iterator, iterator> equal_range(const K &key) { return wrapped().equal_range(key); }
		template<typename K>
		std::pair<const_iterator, const_iterator> equal_range(const K &key) const { return wrapped().equal_range(key); }

		iterator lower_bound(const Key &key) { return wrapped().lower_bound(key); }
		const_iterator lower_bound(const Key &key) const { return wrapped().lower_bound(key); }

		template<typename K>
		iterator lower_bound(const K &key) { return wrapped().lower_bound(key); }
		template<typename K>
		const_iterator lower_bound(const K &key) const { return wrapped().lower_bound(key); }

		iterator upper_bound(const Key &key) { return wrapped().upper_bound(key); }
		const_iterator upper_bound(const Key &key) const { return wrapped().upper_bound(key); }

		template<typename K>
		iterator upper_bound(const K &key) { return wrapped().upper_bound(key); }
		template<typename K>
		const_iterator upper_bound(const K &key) const { return wrapped().upper_bound(key); }

	public: // -- cmp types -- //

		key_compare key_comp() const { return wrapped().key_comp(); }
		value_compare value_comp() const { return wrapped().value_comp(); }

	public: // -- cmp -- //

		friend bool operator==(const multimap &a, const multimap &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const multimap &a, const multimap &b) { return a.wrapped() != b.wrapped(); }
		friend bool operator<(const multimap &a, const multimap &b) { return a.wrapped() < b.wrapped(); }
		friend bool operator<=(const multimap &a, const multimap &b) { return a.wrapped() <= b.wrapped(); }
		friend bool operator>(const multimap &a, const multimap &b) { return a.wrapped() > b.wrapped(); }
		friend bool operator>=(const multimap &a, const multimap &b) { return a.wrapped() >= b.wrapped(); }
	};
	template<typename Key, typename T, typename Compare, typename Allocator>
	struct router<GC::multimap<Key, T, Compare, Allocator>>
	{
		template<typename F>
		static void route(const GC::multimap<Key, T, Compare, Allocator> &multimap, F func)
		{
			std::lock_guard<std::mutex> lock(multimap.mutex);
			GC::route(multimap.wrapped(), func);
		}
	};

	template<typename Key, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>, typename Allocator = std::allocator<Key>>
	class unordered_set
	{
	private: // -- data -- //

		typedef std::unordered_set<Key, Hash, KeyEqual, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<unordered_set>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::key_type key_type;
		typedef typename wrapped_t::value_type value_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::hasher hasher;
		typedef typename wrapped_t::key_equal key_equal;

		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::local_iterator local_iterator;
		typedef typename wrapped_t::const_local_iterator const_local_iterator;

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		typedef typename wrapped_t::node_type node_type;
		typedef typename wrapped_t::insert_return_type insert_return_type;
		#endif

	public: // -- ctor / dtor -- //

		unordered_set()
		{
			new (buffer) wrapped_t();
		}
		explicit unordered_set(size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(bucket_count, hash, equal, alloc);
		}

		unordered_set(size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(bucket_count, alloc);
		}
		unordered_set(size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(bucket_count, hash, alloc);
		}

		explicit unordered_set(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		template<typename InputIt>
		unordered_set(InputIt first, InputIt last)
		{
			new (buffer) wrapped_t(first, last);
		}
		template<typename InputIt>
		unordered_set(InputIt first, InputIt last, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, bucket_count, hash, equal, alloc);
		}

		template<typename InputIt>
		unordered_set(InputIt first, InputIt last, size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, bucket_count, alloc);
		}

		template<typename InputIt>
		unordered_set(InputIt first, InputIt last, size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, bucket_count, hash, alloc);
		}

		unordered_set(const unordered_set &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		unordered_set(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		unordered_set(const unordered_set &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		unordered_set(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		unordered_set(unordered_set &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		unordered_set(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		unordered_set(unordered_set &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		unordered_set(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		unordered_set(std::initializer_list<value_type> init)
		{
			new (buffer) wrapped_t(init);
		}
		unordered_set(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, bucket_count, hash, equal, alloc);
		}

		unordered_set(std::initializer_list<value_type> init, size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, bucket_count, alloc);
		}
		unordered_set(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, bucket_count, hash, alloc);
		}

		~unordered_set()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		unordered_set &operator=(const unordered_set &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		unordered_set &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		unordered_set &operator=(unordered_set &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		unordered_set &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		unordered_set &operator=(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		std::pair<iterator, bool> insert(const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(value);
		}
		std::pair<iterator, bool> insert(value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(value));
		}

		iterator insert(const_iterator hint, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, value);
		}
		iterator insert(const_iterator hint, value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(value));
		}

		template<typename InputIt>
		void insert(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(first, last);
		}

		void insert(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(ilist);
		}

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		insert_return_type insert(node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(nh));
		}
		iterator insert(const_iterator hint, node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(nh));
		}

		#endif

		template<typename ...Args>
		std::pair<iterator, bool> emplace(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator emplace_hint(const_iterator hint, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

		size_type erase(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(key);
		}

	public: // -- swap -- //

		void swap(unordered_set &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(unordered_set &a, unordered_set &b) { a.swap(b); }

	public: // -- extract -- //

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		node_type extract(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(pos);
		}
		node_type extract(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(key);
		}

		#endif

		// !! ADD MERGE FUNCTIONS (C++17)

	public: // -- lookup -- //

		size_type count(const Key &key) const { return wrapped().count(key); }
		template<typename K>
		size_type count(const K &key) const { return wrapped().count(key); }

		iterator find(const Key &key) { return wrapped().find(key); }
		const_iterator find(const Key &key) const { return wrapped().find(key); }

		template<typename K>
		iterator find(const K &key) { return wrapped().find(key); }
		template<typename K>
		const_iterator find(const K &key) const { return wrapped().find(key); }

		bool contains(const Key &key) const { return wrapped().contains(key); }
		template<typename K>
		bool contains(const K &key) const { return wrapped().contains(key); }

		std::pair<iterator, iterator> equal_range(const Key &key) { return wrapped().equal_range(key); }
		std::pair<const_iterator, const_iterator> equal_range(const Key &key) const { return wrapped().equal_range(key); }

		template<typename K>
		std::pair<iterator, iterator> equal_range(const K &key) { return wrapped().equal_range(key); }
		template<typename K>
		std::pair<const_iterator, const_iterator> equal_range(const K &key) const { return wrapped().equal_range(key); }

	public: // -- bucket interface -- //

		local_iterator begin(size_type n) { return wrapped().begin(n); }
		const_local_iterator begin(size_type n) const { return wrapped().begin(n); }
		const_local_iterator cbegin(size_type n) const { return wrapped().cbegin(n); }

		local_iterator end(size_type n) { return wrapped().end(n); }
		const_local_iterator end(size_type n) const { return wrapped().end(n); }
		const_local_iterator cend(size_type n) const { return wrapped().cend(n); }

		size_type bucket_count() const { return wrapped().bucket_count(); }
		size_type max_bucket_count() const { return wrapped().max_bucket_count(); }

		size_type bucket_size(size_type n) const { return wrapped().bucket_size(n); }
		size_type bucket(const Key &key) const { return wrapped().bucket(key); }

	public: // -- hash policy -- //

		float load_factor() const { return wrapped().load_factor(); }

		float max_load_factor() const { return wrapped().max_load_factor(); }
		void max_load_factor(float ml)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().max_load_factor(ml);
		}

		void rehash(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().rehash(count);
		}

		void reserve(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reserve(count);
		}

	public: // -- observers -- //

		hasher hash_function() const { return wrapped().hash_function(); }
		key_equal key_eq() const { return wrapped().key_eq(); }

	public: // -- cmp -- //

		friend bool operator==(const unordered_set &a, const unordered_set &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const unordered_set &a, const unordered_set &b) { return a.wrapped() != b.wrapped(); }
	};
	template<typename Key, typename Hash, typename KeyEqual, typename Allocator>
	struct router<GC::unordered_set<Key, Hash, KeyEqual, Allocator>>
	{
		template<typename F>
		static void route(const GC::unordered_set<Key, Hash, KeyEqual, Allocator> &set, F func)
		{
			std::lock_guard<std::mutex> lock(set.mutex);
			GC::route(set.wrapped(), func);
		}
	};

	template<typename Key, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>, typename Allocator = std::allocator<Key>>
	class unordered_multiset
	{
	private: // -- data -- //

		typedef std::unordered_multiset<Key, Hash, KeyEqual, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<unordered_multiset>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::key_type key_type;
		typedef typename wrapped_t::value_type value_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::hasher hasher;
		typedef typename wrapped_t::key_equal key_equal;

		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::local_iterator local_iterator;
		typedef typename wrapped_t::const_local_iterator const_local_iterator;

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		typedef typename wrapped_t::node_type node_type;
		#endif

	public: // -- ctor / dtor -- //

		unordered_multiset()
		{
			new (buffer) wrapped_t();
		}
		explicit unordered_multiset(size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(bucket_count, hash, equal, alloc);
		}

		unordered_multiset(size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(bucket_count, alloc);
		}
		unordered_multiset(size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(bucket_count, hash, alloc);
		}

		explicit unordered_multiset(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		template<typename InputIt>
		unordered_multiset(InputIt first, InputIt last)
		{
			new (buffer) wrapped_t(first, last);
		}
		template<typename InputIt>
		unordered_multiset(InputIt first, InputIt last, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, bucket_count, hash, equal, alloc);
		}

		template<typename InputIt>
		unordered_multiset(InputIt first, InputIt last, size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, bucket_count, alloc);
		}

		template<typename InputIt>
		unordered_multiset(InputIt first, InputIt last, size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, bucket_count, hash, alloc);
		}

		unordered_multiset(const unordered_multiset &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		unordered_multiset(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		unordered_multiset(const unordered_multiset &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		unordered_multiset(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		unordered_multiset(unordered_multiset &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		unordered_multiset(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		unordered_multiset(unordered_multiset &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		unordered_multiset(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		unordered_multiset(std::initializer_list<value_type> init)
		{
			new (buffer) wrapped_t(init);
		}
		unordered_multiset(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, bucket_count, hash, equal, alloc);
		}

		unordered_multiset(std::initializer_list<value_type> init, size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, bucket_count, alloc);
		}
		unordered_multiset(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, bucket_count, hash, alloc);
		}

		~unordered_multiset()
		{
			wrapped().~wrapped_t();
		}

	public: // -- asgn -- //

		unordered_multiset &operator=(const unordered_multiset &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		unordered_multiset &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		unordered_multiset &operator=(unordered_multiset &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		unordered_multiset &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		unordered_multiset &operator=(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		iterator insert(const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(value);
		}
		iterator insert(value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(value));
		}

		iterator insert(const_iterator hint, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, value);
		}
		iterator insert(const_iterator hint, value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(value));
		}

		template<typename InputIt>
		void insert(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(first, last);
		}

		void insert(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(ilist);
		}

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		iterator insert(node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(nh));
		}
		iterator insert(const_iterator hint, node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(nh));
		}

		#endif

		template<typename ...Args>
		iterator emplace(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator emplace_hint(const_iterator hint, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

		size_type erase(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(key);
		}

	public: // -- swap -- //

		void swap(unordered_multiset &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(unordered_multiset &a, unordered_multiset &b) { a.swap(b); }

	public: // -- extract -- //

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		node_type extract(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(pos);
		}
		node_type extract(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(key);
		}

		#endif

		// !! ADD MERGE FUNCTIONS (C++17)

	public: // -- lookup -- //

		size_type count(const Key &key) const { return wrapped().count(key); }
		template<typename K>
		size_type count(const K &key) const { return wrapped().count(key); }

		iterator find(const Key &key) { return wrapped().find(key); }
		const_iterator find(const Key &key) const { return wrapped().find(key); }

		template<typename K>
		iterator find(const K &key) { return wrapped().find(key); }
		template<typename K>
		const_iterator find(const K &key) const { return wrapped().find(key); }

		bool contains(const Key &key) const { return wrapped().contains(key); }
		template<typename K>
		bool contains(const K &key) const { return wrapped().contains(key); }

		std::pair<iterator, iterator> equal_range(const Key &key) { return wrapped().equal_range(key); }
		std::pair<const_iterator, const_iterator> equal_range(const Key &key) const { return wrapped().equal_range(key); }

		template<typename K>
		std::pair<iterator, iterator> equal_range(const K &key) { return wrapped().equal_range(key); }
		template<typename K>
		std::pair<const_iterator, const_iterator> equal_range(const K &key) const { return wrapped().equal_range(key); }

	public: // -- bucket interface -- //

		local_iterator begin(size_type n) { return wrapped().begin(n); }
		const_local_iterator begin(size_type n) const { return wrapped().begin(n); }
		const_local_iterator cbegin(size_type n) const { return wrapped().cbegin(n); }

		local_iterator end(size_type n) { return wrapped().end(n); }
		const_local_iterator end(size_type n) const { return wrapped().end(n); }
		const_local_iterator cend(size_type n) const { return wrapped().cend(n); }

		size_type bucket_count() const { return wrapped().bucket_count(); }
		size_type max_bucket_count() const { return wrapped().max_bucket_count(); }

		size_type bucket_size(size_type n) const { return wrapped().bucket_size(n); }
		size_type bucket(const Key &key) const { return wrapped().bucket(key); }

	public: // -- hash policy -- //

		float load_factor() const { return wrapped().load_factor(); }

		float max_load_factor() const { return wrapped().max_load_factor(); }
		void max_load_factor(float ml)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().max_load_factor(ml);
		}

		void rehash(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().rehash(count);
		}

		void reserve(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reserve(count);
		}

	public: // -- observers -- //

		hasher hash_function() const { return wrapped().hash_function(); }
		key_equal key_eq() const { return wrapped().key_eq(); }

	public: // -- cmp -- //

		friend bool operator==(const unordered_multiset &a, const unordered_multiset &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const unordered_multiset &a, const unordered_multiset &b) { return a.wrapped() != b.wrapped(); }
	};
	template<typename Key, typename Hash, typename KeyEqual, typename Allocator>
	struct router<GC::unordered_multiset<Key, Hash, KeyEqual, Allocator>>
	{
		template<typename F>
		static void route(const GC::unordered_multiset<Key, Hash, KeyEqual, Allocator> &set, F func)
		{
			std::lock_guard<std::mutex> lock(set.mutex);
			GC::route(set.wrapped(), func);
		}
	};

	template<typename Key, typename T, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>, typename Allocator = std::allocator<std::pair<const Key, T>>>
	class unordered_map
	{
	private: // -- data -- //

		typedef std::unordered_map<Key, T, Hash, KeyEqual, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<unordered_map>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::key_type key_type;
		typedef typename wrapped_t::mapped_type mapped_type;

		typedef typename wrapped_t::value_type value_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::hasher hasher;
		typedef typename wrapped_t::key_equal key_equal;

		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::local_iterator local_iterator;
		typedef typename wrapped_t::const_local_iterator const_local_iterator;

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		typedef typename wrapped_t::node_type node_type;
		typedef typename wrapped_t::insert_return_type insert_return_type;
		#endif

	public: // -- ctor / dtor -- //

		unordered_map()
		{
			new (buffer) wrapped_t();
		}
		explicit unordered_map(size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(bucket_count, hash, equal, alloc);
		}

		unordered_map(size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(bucket_count, alloc);
		}
		unordered_map(size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(bucket_count, hash, alloc);
		}

		explicit unordered_map(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		template<typename InputIt>
		unordered_map(InputIt first, InputIt last)
		{
			new (buffer) wrapped_t(first, last);
		}
		template<typename InputIt>
		unordered_map(InputIt first, InputIt last, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, bucket_count, hash, equal, alloc);
		}

		template<typename InputIt>
		unordered_map(InputIt first, InputIt last, size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, bucket_count, alloc);
		}

		template<typename InputIt>
		unordered_map(InputIt first, InputIt last, size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, bucket_count, hash, alloc);
		}

		unordered_map(const unordered_map &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		unordered_map(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		unordered_map(const unordered_map &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		unordered_map(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		unordered_map(unordered_map &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		unordered_map(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		unordered_map(unordered_map &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		unordered_map(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		unordered_map(std::initializer_list<value_type> init)
		{
			new (buffer) wrapped_t(init);
		}
		unordered_map(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, bucket_count, hash, equal, alloc);
		}

		unordered_map(std::initializer_list<value_type> init, size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, bucket_count, alloc);
		}
		unordered_map(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, bucket_count, hash, alloc);
		}

		~unordered_map()
		{
			wrapped().~wrapped_t();
		}

	public: // -- assign -- //

		unordered_map &operator=(const unordered_map &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		unordered_map &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		unordered_map &operator=(unordered_map &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		unordered_map &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		unordered_map &operator=(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		std::pair<iterator, bool> insert(const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(value);
		}

		template<typename P>
		std::pair<iterator, bool> insert(P &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::forward<P>(value));
		}

		iterator insert(const_iterator hint, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, value);
		}

		template<typename P>
		iterator insert(const_iterator hint, P &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::forward<P>(value));
		}

		template<typename InputIt>
		void insert(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(first, last);
		}

		void insert(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(ilist);
		}

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		std::pair<iterator, bool> insert(value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(value));
		}
		iterator insert(const_iterator hint, value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(value));
		}

		insert_return_type insert(node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(nh));
		}
		iterator insert(const_iterator hint, node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(nh));
		}

		template<typename M>
		std::pair<iterator, bool> insert_or_assign(const key_type &k, M &&obj)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_or_assign(k, std::forward<M>(obj));
		}
		template<typename M>
		std::pair<iterator, bool> insert_or_assign(key_type &&k, M &&obj)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_or_assign(std::move(k), std::forward<M>(obj));
		}

		template<typename M>
		iterator insert_or_assign(const_iterator hint, const key_type &k, M &&obj)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_or_assign(hint, k, std::forward<M>(obj));
		}
		template<typename M>
		iterator insert_or_assign(const_iterator hint, key_type &&k, M &&obj)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert_or_assign(hint, std::move(k), std::forward<M>(obj));
		}

		template<typename ...Args>
		std::pair<iterator, bool> try_emplace(const key_type &k, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().try_emplace(k, std::forward<Args>(args)...);
		}
		template<typename ...Args>
		std::pair<iterator, bool> try_emplace(key_type &&k, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().try_emplace(std::move(k), std::forward<Args>(args)...);
		}

		template<typename ...Args>
		iterator try_emplace(const_iterator hint, const key_type &k, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().try_emplace(hint, k, std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator try_emplace(const_iterator hint, key_type &&k, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().try_emplace(hint, std::move(k), std::forward<Args>(args)...);
		}

		#endif

		template<typename ...Args>
		std::pair<iterator, bool> emplace(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator emplace_hint(const_iterator hint, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

		size_type erase(const key_type &k)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(k);
		}

	public: // -- swap -- //

		void swap(unordered_map &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(unordered_map &a, unordered_map &b) { a.swap(b); }

	public: // -- extract -- //

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		node_type extract(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(pos);
		}
		node_type extract(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(key);
		}

		#endif

		// !! ADD MERGE FUNCTIONS (C++17)

	public: // -- element access -- //

		T &at(const Key &key) { return wrapped().at(key); }
		const T &at(const Key &key) const { return wrapped().at(key); }

		T &operator[](const Key &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex); // operator[] performs an insertion if key doesn't exist
			return wrapped()[key];
		}
		T &operator[](Key &&key)
		{
			std::lock_guard<std::mutex> lock(this->mutex); // operator[] performs an insertion if key doesn't exist
			return wrapped()[std::move(key)];
		}

	public: // -- lookup -- //

		size_type count(const Key &key) const { return wrapped().count(key); }
		template<typename K>
		size_type count(const K &key) const { return wrapped().count(key); }

		iterator find(const Key &key) { return wrapped().find(key); }
		const_iterator find(const Key &key) const { return wrapped().find(key); }

		template<typename K>
		iterator find(const K &key) { return wrapped().find(key); }
		template<typename K>
		const_iterator find(const K &key) const { return wrapped().find(key); }

		bool contains(const Key &key) const { return wrapped().contains(key); }
		template<typename K>
		bool contains(const K &key) const { return wrapped().contains(key); }

		std::pair<iterator, iterator> equal_range(const Key &key) { return wrapped().equal_range(key); }
		std::pair<const_iterator, const_iterator> equal_range(const Key &key) const { return wrapped().equal_range(key); }

		template<typename K>
		std::pair<iterator, iterator> equal_range(const K &key) { return wrapped().equal_range(key); }
		template<typename K>
		std::pair<const_iterator, const_iterator> equal_range(const K &key) const { return wrapped().equal_range(key); }

	public: // -- bucket interface -- //

		local_iterator begin(size_type n) { return wrapped().begin(n); }
		const_local_iterator begin(size_type n) const { return wrapped().begin(n); }
		const_local_iterator cbegin(size_type n) const { return wrapped().cbegin(n); }

		local_iterator end(size_type n) { return wrapped().end(n); }
		const_local_iterator end(size_type n) const { return wrapped().end(n); }
		const_local_iterator cend(size_type n) const { return wrapped().cend(n); }

		size_type bucket_count() const { return wrapped().bucket_count(); }
		size_type max_bucket_count() const { return wrapped().max_bucket_count(); }

		size_type bucket_size(size_type n) const { return wrapped().bucket_size(n); }
		size_type bucket(const Key &key) const { return wrapped().bucket(key); }

	public: // -- hash policy -- //

		float load_factor() const { return wrapped().load_factor(); }

		float max_load_factor() const { return wrapped().max_load_factor(); }
		void max_load_factor(float ml)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().max_load_factor(ml);
		}

		void rehash(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().rehash(count);
		}

		void reserve(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reserve(count);
		}

	public: // -- observers -- //

		hasher hash_function() const { return wrapped().hash_function(); }
		key_equal key_eq() const { return wrapped().key_eq(); }

	public: // -- cmp -- //

		friend bool operator==(const unordered_map &a, const unordered_map &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const unordered_map &a, const unordered_map &b) { return a.wrapped() != b.wrapped(); }
	};
	template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
	struct router<GC::unordered_map<Key, T, Hash, KeyEqual, Allocator>>
	{
		template<typename F>
		static void route(const GC::unordered_map<Key, T, Hash, KeyEqual, Allocator> &map, F func)
		{
			std::lock_guard<std::mutex> lock(map.mutex);
			GC::route(map.wrapped(), func);
		}
	};

	template<typename Key, typename T, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>, typename Allocator = std::allocator<std::pair<const Key, T>>>
	class unordered_multimap
	{
	private: // -- data -- //

		typedef std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator> wrapped_t; // the wrapped type

		alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

		mutable std::mutex mutex; // router synchronizer

		friend struct GC::router<unordered_multimap>;

	private: // -- data accessors -- //

		// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
		wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
		const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

	public: // -- typedefs -- //

		typedef typename wrapped_t::key_type key_type;
		typedef typename wrapped_t::mapped_type mapped_type;

		typedef typename wrapped_t::value_type value_type;

		typedef typename wrapped_t::size_type size_type;
		typedef typename wrapped_t::difference_type difference_type;

		typedef typename wrapped_t::hasher hasher;
		typedef typename wrapped_t::key_equal key_equal;

		typedef typename wrapped_t::allocator_type allocator_type;

		typedef typename wrapped_t::reference reference;
		typedef typename wrapped_t::const_reference const_reference;

		typedef typename wrapped_t::pointer pointer;
		typedef typename wrapped_t::const_pointer const_pointer;

		typedef typename wrapped_t::iterator iterator;
		typedef typename wrapped_t::const_iterator const_iterator;

		typedef typename wrapped_t::local_iterator local_iterator;
		typedef typename wrapped_t::const_local_iterator const_local_iterator;

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		typedef typename wrapped_t::node_type node_type;
		#endif

	public: // -- ctor / dtor -- //

		unordered_multimap()
		{
			new (buffer) wrapped_t();
		}
		explicit unordered_multimap(size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(bucket_count, hash, equal, alloc);
		}

		unordered_multimap(size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(bucket_count, alloc);
		}
		unordered_multimap(size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(bucket_count, hash, alloc);
		}

		explicit unordered_multimap(const Allocator &alloc)
		{
			new (buffer) wrapped_t(alloc);
		}

		template<typename InputIt>
		unordered_multimap(InputIt first, InputIt last)
		{
			new (buffer) wrapped_t(first, last);
		}
		template<typename InputIt>
		unordered_multimap(InputIt first, InputIt last, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(first, last, bucket_count, hash, equal, alloc);
		}

		template<typename InputIt>
		unordered_multimap(InputIt first, InputIt last, size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, bucket_count, alloc);
		}

		template<typename InputIt>
		unordered_multimap(InputIt first, InputIt last, size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(first, last, bucket_count, hash, alloc);
		}

		unordered_multimap(const unordered_multimap &other)
		{
			new (buffer) wrapped_t(other.wrapped());
		}
		unordered_multimap(const wrapped_t &other)
		{
			new (buffer) wrapped_t(other);
		}

		unordered_multimap(const unordered_multimap &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other.wrapped(), alloc);
		}
		unordered_multimap(const wrapped_t &other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(other, alloc);
		}

		unordered_multimap(unordered_multimap &&other)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()));
		}
		unordered_multimap(wrapped_t &&other)
		{
			new (buffer) wrapped_t(std::move(other));
		}

		unordered_multimap(unordered_multimap &&other, const Allocator &alloc)
		{
			std::lock_guard<std::mutex> lock(other.mutex);
			new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
		}
		unordered_multimap(wrapped_t &&other, const Allocator &alloc)
		{
			new (buffer) wrapped_t(std::move(other), alloc);
		}

		unordered_multimap(std::initializer_list<value_type> init)
		{
			new (buffer) wrapped_t(init);
		}
		unordered_multimap(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
		{
			new (buffer) wrapped_t(init, bucket_count, hash, equal, alloc);
		}

		unordered_multimap(std::initializer_list<value_type> init, size_type bucket_count, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, bucket_count, alloc);
		}
		unordered_multimap(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash, const Allocator &alloc)
		{
			new (buffer) wrapped_t(init, bucket_count, hash, alloc);
		}

		~unordered_multimap()
		{
			wrapped().~wrapped_t();
		}

	public: // -- assign -- //

		unordered_multimap &operator=(const unordered_multimap &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other.wrapped();
			return *this;
		}
		unordered_multimap &operator=(const wrapped_t &other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = other;
			return *this;
		}

		unordered_multimap &operator=(unordered_multimap &&other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
			return *this;
		}
		unordered_multimap &operator=(wrapped_t &&other)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = std::move(other);
			return *this;
		}

		unordered_multimap &operator=(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped() = ilist;
			return *this;
		}

	public: // -- misc -- //

		allocator_type get_allocator() const { return wrapped().get_allocator(); }

	public: // -- iterators -- //

		iterator begin() noexcept { return wrapped().begin(); }
		const_iterator begin() const noexcept { return wrapped().begin(); }
		const_iterator cbegin() const noexcept { return wrapped().cbegin(); }

		iterator end() noexcept { return wrapped().end(); }
		const_iterator end() const noexcept { return wrapped().end(); }
		const_iterator cend() const noexcept { return wrapped().cend(); }

	public: // -- size / cap -- //

		bool empty() const noexcept { return wrapped().empty(); }
		size_type size() const noexcept { return wrapped().size(); }

		size_type max_size() const noexcept { return wrapped().max_size(); }

		void clear() noexcept
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().clear();
		}

	public: // -- insert / erase -- //

		iterator insert(const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(value);
		}

		template<typename P>
		iterator insert(P &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::forward<P>(value));
		}

		iterator insert(const_iterator hint, const value_type &value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, value);
		}

		template<typename P>
		iterator insert(const_iterator hint, P &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::forward<P>(value));
		}

		template<typename InputIt>
		void insert(InputIt first, InputIt last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(first, last);
		}

		void insert(std::initializer_list<value_type> ilist)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().insert(ilist);
		}

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17

		iterator insert(value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(value));
		}
		iterator insert(const_iterator hint, value_type &&value)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(value));
		}

		iterator insert(node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(std::move(nh));
		}
		iterator insert(const_iterator hint, node_type &&nh)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().insert(hint, std::move(nh));
		}

		#endif

		template<typename ...Args>
		iterator emplace(Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace(std::forward<Args>(args)...);
		}
		template<typename ...Args>
		iterator emplace_hint(const_iterator hint, Args &&...args)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
		}

		iterator erase(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(pos);
		}
		iterator erase(const_iterator first, const_iterator last)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(first, last);
		}

		size_type erase(const key_type &k)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().erase(k);
		}

	public: // -- swap -- //

		void swap(unordered_multimap &other)
		{
			GC::scoped_lock<std::mutex, std::mutex> locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
		friend void swap(unordered_multimap &a, unordered_multimap &b) { a.swap(b); }

	public: // -- extract -- //

		#if DRAGAZO_GARBAGE_COLLECT_CPP_VERSION_ID >= 17
		node_type extract(const_iterator pos)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(pos);
		}
		node_type extract(const key_type &key)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return wrapped().extract(key);
		}
		#endif

		// !! ADD MERGE FUNCTIONS (C++17)

	public: // -- lookup -- //

		size_type count(const Key &key) const { return wrapped().count(key); }
		template<typename K>
		size_type count(const K &key) const { return wrapped().count(key); }

		iterator find(const Key &key) { return wrapped().find(key); }
		const_iterator find(const Key &key) const { return wrapped().find(key); }

		template<typename K>
		iterator find(const K &key) { return wrapped().find(key); }
		template<typename K>
		const_iterator find(const K &key) const { return wrapped().find(key); }

		bool contains(const Key &key) const { return wrapped().contains(key); }
		template<typename K>
		bool contains(const K &key) const { return wrapped().contains(key); }

		std::pair<iterator, iterator> equal_range(const Key &key) { return wrapped().equal_range(key); }
		std::pair<const_iterator, const_iterator> equal_range(const Key &key) const { return wrapped().equal_range(key); }

		template<typename K>
		std::pair<iterator, iterator> equal_range(const K &key) { return wrapped().equal_range(key); }
		template<typename K>
		std::pair<const_iterator, const_iterator> equal_range(const K &key) const { return wrapped().equal_range(key); }

	public: // -- bucket interface -- //

		local_iterator begin(size_type n) { return wrapped().begin(n); }
		const_local_iterator begin(size_type n) const { return wrapped().begin(n); }
		const_local_iterator cbegin(size_type n) const { return wrapped().cbegin(n); }

		local_iterator end(size_type n) { return wrapped().end(n); }
		const_local_iterator end(size_type n) const { return wrapped().end(n); }
		const_local_iterator cend(size_type n) const { return wrapped().cend(n); }

		size_type bucket_count() const { return wrapped().bucket_count(); }
		size_type max_bucket_count() const { return wrapped().max_bucket_count(); }

		size_type bucket_size(size_type n) const { return wrapped().bucket_size(n); }
		size_type bucket(const Key &key) const { return wrapped().bucket(key); }

	public: // -- hash policy -- //

		float load_factor() const { return wrapped().load_factor(); }

		float max_load_factor() const { return wrapped().max_load_factor(); }
		void max_load_factor(float ml)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().max_load_factor(ml);
		}

		void rehash(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().rehash(count);
		}

		void reserve(size_type count)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			wrapped().reserve(count);
		}

	public: // -- observers -- //

		hasher hash_function() const { return wrapped().hash_function(); }
		key_equal key_eq() const { return wrapped().key_eq(); }

	public: // -- cmp -- //

		friend bool operator==(const unordered_multimap &a, const unordered_multimap &b) { return a.wrapped() == b.wrapped(); }
		friend bool operator!=(const unordered_multimap &a, const unordered_multimap &b) { return a.wrapped() != b.wrapped(); }
	};
	template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
	struct router<GC::unordered_multimap<Key, T, Hash, KeyEqual, Allocator>>
	{
		template<typename F>
		static void route(const GC::unordered_multimap<Key, T, Hash, KeyEqual, Allocator> &map, F func)
		{
			std::lock_guard<std::mutex> lock(map.mutex);
			GC::route(map.wrapped(), func);
		}
	};

private: // -- containers -- //

	// a container of info objects - implemented as a doubly-linked list for fast removal from the middle.
	// this container has no internal synchronization and is thus not thread safe.
	class obj_list
	{
	private: // -- data -- //

		info *first; // pointer to the first object
		info *last;  // pointer to the last object

	public: // -- ctor / dtor / asgn -- //

		// creates an empty obj list
		obj_list();

		// destroys the container but does not release resources - see unsafe_clear()
		~obj_list() = default;

		obj_list(const obj_list&) = delete;
		obj_list &operator=(const obj_list&) = delete;

	public: // -- access -- //

		// gets the first info object - null if there is none.
		// this is supplied for enabling iteration - any non-null value is a valid node.
		info *front() const { return first; }

		// adds obj to this database - und. if obj already belongs to one or more databases.
		void add(info *obj);
		// removes obj from this database - und. if obj does not belong to this database.
		void remove(info *obj);

		// merges the contents of other with the contents of this.
		// if other refers to this object, does nothing.
		// otherwise, other is guaranteed to be empty after this operation.
		void merge(obj_list &&other);

		// sets the contents of the list to empty without deallocating resources.
		// this should only be used if you're handling resource deallocation yourself.
		void unsafe_clear() { first = last = nullptr; }

		// returns true iff the container is empty
		bool empty() const noexcept { return !first; }
	};

private: // -- sentries -- //

	// a sentry for an atomic flag.
	// on construction, sets the flag and gets the previous value; on destruction, clears the flag if it was successfully taken.
	// conversion of the sentry to bool gets if the flag was successfully taken (i.e. if it was previously false)
	struct atomic_flag_sentry
	{
	private: // -- data -- //

		std::atomic_flag &_flag;
		bool _value;

	public: // -- interface -- //

		explicit atomic_flag_sentry(std::atomic_flag &flag) : _flag(flag)
		{
			_value = _flag.test_and_set();
		}
		~atomic_flag_sentry()
		{
			if (!_value) _flag.clear();
		}

		explicit operator bool() const noexcept { return !_value; }
		bool operator!() const noexcept { return _value; }
	};

private: // -- collection synchronizer -- //

	class collection_synchronizer
	{
	private: // -- synchronization -- //

		static std::mutex internal_mutex; // mutex used only for internal synchronization

		static std::thread::id collector_thread; // the thread id of the collector - none implies no collection is currently processing

	private: // -- collector-only resources -- //

		// these objects represent a snapshot of the object graph for use by the collector.
		// the current collector thread need not lock any mutex to use these resources.
		// if you LOCK internal_mutex and find there's NO current collection action, you can modify them.
		
		// the list of all objects currently under gc consideration.
		// during a collection action, any unreachable object in this list is subject to deletion.
		static obj_list objs;

		// the set of all rooted handles - DO NOT USE THIS FOR COLLECTION LOGIC.
		// the roots in this set are subject to deletion by arbitrary running threads after the sentry construction.
		// instead, you should use root_objs, which is guaranteed to be safe during the full collection action.
		// note: a root is the address of an info* (i.e. info**) not the info* itself.
		// this is because the same rooted handle can be repointed to a different object and still be a root.
		// this is what supplies information to the root_objs set for the collector.
		static std::unordered_set<info*const*> roots;

		// the set of all objects that are pointed-to by rooted handles (guaranteed not to contain null).
		// this should not be modified directly - should only be manipulated by a valid sentry.
		// this is a set to facilitate fast removal and to ensure there are no duplicates (efficiency).
		// this is separate from the roots set for two reasons:
		// 1) it's more convenient / efficient to perform the sweep logic in terms of objects.
		// 2) if the (rooted) handle the root obj was sourced from is destroyed, said (rooted) handle ceases to exist.
		//    this would be a problem later on in the collector, as we'd need to dereference the handle to get the obj to sweep.
		//    but with this approach that's not an issue for the collector, because it won't ever need to be dereferenced again.
		//    and we know the root obj won't be destroyed because the collector is the only one allowed to do that.
		static std::unordered_set<info*> root_objs;

		// the list of objects that should be destroyed after a collector pass.
		// this should not be modified directly - should only be manipulated by a valid sentry.
		// when an object is marked for deletion (i.e. unreachable) it is removed from objs and added to this list.
		// the sentry dtor will handle the logic of deleting the objects.
		static obj_list del_list;

	private: // -- caches -- //

		// these objects can be modified at any time so long as internal_mutex is locked.
		// operations on these objects should be non-blocking (don't call arbitrary code while it's locked).
		// among these are several caches:
		// all actions must be non-blocking, but if there's currently a collector thread you can't modify the collector-only resources.
		// thus, in these cases you add the action info to a cache, which will be applied when the collection action terminates.
		// additionally, if there's NOT a collection action in progress, you MUST apply the change immediately (do not cache it).
		// this is because the caches are only flushed upon termination of a collect action, not before one begins.

		// the scheduled obj add operations
		static obj_list objs_add_cache;

		// the scheduled root add/remove operations.
		// these sets must at all times be disjoint.
		static std::unordered_set<info*const*> roots_add_cache;
		static std::unordered_set<info*const*> roots_remove_cache;

		// cache used to support non-blocking handle repoint actions.
		// it is structured such that M[&raw_handle] is what it should be repointed to.
		static std::unordered_map<info**, info*> handle_repoint_cache; 

	public: // -- types -- //

		// a sentry object that manages synchronization of a collect action, as well as granting access to the data to act on.
		// you should (default) construct an object of this type before beginning a collect action (and before acquiring any long-running mutexes).
		// you must then ensure the operation was successful (i.e. operator bool() = true).
		// if it is successful, you can continue the collect action, otherwise you must abort the collection without raising an error.
		class collection_sentry
		{
		private: // -- data -- //

			bool success; // the success flag (if we successfully began a collection action)

		public: // -- ctor / dtor / asgn / etc -- //

			collection_sentry(const collection_sentry&) = delete;
			collection_sentry &operator=(const collection_sentry&) = delete;

			// begins a collection action - this must be performed before any collection pass.
			// if there is already a collection action in progress, marks as failure.
			// otherwise the calling thread is marked as the collector thread, all cached database modifications are applied, and marks as success.
			// the success/failure flag can be checked via operator bool (true is success).
			// if this operation does not succeed, it is undefined behavior to continue the collection pass.
			collection_sentry();

			// ends a collection action - this must be performed after any collection pass.
			// this will delete everything marked for deletion by mark_delete() before ending the collection action.
			// this must be called regardless of if the sentry construction resulted in a valid collection action (i.e. operator bool).
			~collection_sentry();

			// returns true iff the sentry construction was successful and the collection action can continue.
			explicit operator bool() const noexcept { return success; }
			// returns true iff the sentry construction failed and the collection action cannot be continued.
			bool operator!() const noexcept { return !success; }

		public: // -- data access -- //

			// gets the objs database.
			// it is undefined behavior to call this function if the sentry construction failed (see operator bool).
			auto &get_objs()
			{
				assert(success);
				return collection_synchronizer::objs;
			}

			// gets the roots database.
			// it is undefined behavior to call this function if the sentry construction failed (see operator bool).
			auto &get_root_objs()
			{
				assert(success);
				return collection_synchronizer::root_objs;
			}

			// marks obj for deletion - obj must not have already been marked for deletion.
			// this automatically removes obj from the obj database.
			// it is undefined behavior to call this function if the sentry construction failed (see operator bool).
			// returns the address of the next object (i.e. obj->next).
			info *mark_delete(info *obj)
			{
				assert(success);

				info *next = obj->next;

				objs.remove(obj);
				del_list.add(obj);

				return next;
			}
		};

	public: // -- interface -- //

		// returns true iff the calling thread is the current collector thread
		static bool this_is_collector_thread();

		// schedules a handle creation action that points to null.
		// a new raw_handle_t value will be created and assigned to handle.
		static void schedule_handle_create_null(info *&handle);
		// schedules a handle creation action that points to a new object - inserts new_obj into the obj database.
		// new_obj must not already exist in the gc database (see create alias below).
		// a new raw_handle_t value will be created and assigned to handle.
		static void schedule_handle_create_bind_new_obj(info *&handle, info *new_obj);
		// schedules a handle creation action that points at a pre-existing handle - marks the new handle as a root.
		// a new raw_handle_t value will be created and assigned to handle.
		static void schedule_handle_create_alias(info *&raw_handle, info *const &src_handle);

		// schedules a handle deletion action - unroots the handle and purges it from the handle repoint cache.
		static void schedule_handle_destroy(info *const &handle);

		// schedules a handle unroot operation - unmarks handle as a root.
		static void schedule_handle_unroot(info *const &raw_handle);

		// schedules a handle repoint action.
		// handle shall eventually be repointed to null before the next collection action.
		// if handle is destroyed, it must first be removed from this cache via unschedule_handle().
		static void schedule_handle_repoint_null(info *&handle);
		// schedules a handle repoint action.
		// handle shall eventually be repointed to new_value before the next collection action.
		// if handle is destroyed, it must first be removed from this cache via unschedule_handle().
		static void schedule_handle_repoint(info *&handle, info *const &new_value);
		// schedules a handle repoint action that swaps the pointed-to info objects of two handles atomically.
		// handle_a shall eventually point to whatever handle_b used to point to and vice versa.
		// if wither handle is destroyed, it must first be removed from this cache via unschedule_handle().
		static void schedule_handle_repoint_swap(info *&handle_a, info *&handle_b);

	private: // -- private interface (unsafe) -- //
		
		// marks handle as a root - internal_mutex should be locked
		static void __schedule_handle_root(info *const &handle);
		// unmarks handle as a root - internal_mutex should be locked
		static void __schedule_handle_unroot(info *const &handle);

		// the underlying function for all handle repoint actions.
		// handles the logic of managing the repoint cache for repointing handle to target.
		static void __raw_schedule_handle_repoint(info *&handle, info *target);

		// gets the current target info object of new_value.
		// otherwise returns the current repoint target if it's in the repoint database.
		// otherwise returns the current pointed-to value of value.
		static info *__get_current_target(info *const &handle);
	};

private: // -- data -- //

	static std::atomic<strategies> _strategy; // the auto collect tactics currently in place

	static std::atomic<sleep_time_t> _sleep_time; // the amount of time to sleep after an automatic timed collection cycle

private: // -- misc -- //

	GC() = delete; // not instantiatable

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
	
	// performs a mark sweep operation from the given handle.
	static void __mark_sweep(info *handle);

	// the first invocation of this function begins a new thread to perform timed garbage collection.
	// all subsequent invocations do nothing.
	static void start_timed_collect();

private: // -- utility router functions -- //
	
	static void router_unroot(const smart_handle &arc);

private: // -- functions you should never ever call ever. did i mention YOU SHOULD NOT CALL THESE?? -- //

	// the function to be executed by the timed collector thread. DO NOT CALL THIS.
	static void __timed_collect_func();
};

// ------------------------- //

// -- std specializations -- //

// ------------------------- //

template<typename T>
struct std::hash<GC::ptr<T>>
{
	std::size_t operator()(const GC::ptr<T> &p) const { return std::hash<T*>()(p.get()); }
};

// standard wrapper for atomic_ptr.
// it might be faster to use atomic_ptr directly (depending on compiler).
template<typename T>
struct std::atomic<GC::ptr<T>>
{
private: // -- data -- //

	GC::atomic_ptr<T> value;

	friend struct GC::router<std::atomic<GC::ptr<T>>>;

public: // -- ctor / dtor / asgn -- //

	atomic() = default;

	~atomic() = default;

	atomic(const atomic&) = delete;
	atomic &operator=(const atomic&) = delete;

public: // -- store / load -- //

	atomic(const GC::ptr<T> &desired) : value(desired) {}
	atomic &operator=(const GC::ptr<T> &desired) { value = desired; return *this; }

	void store(const GC::ptr<T> &desired) { value.store(desired); }
	GC::ptr<T> load() const { return value.load(); }

	operator GC::ptr<T>() const { return static_cast<GC::ptr<T>>(value); }

public: // -- exchange -- //

	GC::ptr<T> exchange(const GC::ptr<T> &desired) { return value.exchange(desired); }

	// !! add compare exchange stuff !! //

public: // -- lock info -- //

	static constexpr bool is_always_lock_free = GC::atomic_ptr<T>::is_always_lock_free;

	bool is_lock_free() const noexcept { return is_always_lock_free; }

public: // -- swap -- //

	void swap(atomic &other) { value.swap(other.value); }
	friend void swap(atomic &a, atomic &b) { a.value.swap(b.value); }
};

// an appropriate specialization for atomic_ptr (does not use any calls to gc functions - see generic std::atomic<T> ill-formed construction)
template<typename T>
struct GC::router<std::atomic<GC::ptr<T>>>
{
	template<typename F> static void route(const std::atomic<GC::ptr<T>> &atomic, F func)
	{
		GC::route(atomic.value, func);
	}
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

// outputs the stored pointer to the stream - equivalent to ostr << ptr.load().get()
template<typename T, typename U, typename V>
std::basic_ostream<U, V> &operator<<(std::basic_ostream<U, V> &ostr, const GC::atomic_ptr<T> &ptr)
{
	ostr << ptr.load().get();
	return ostr;
}
template<typename T, typename U, typename V>
std::basic_ostream<U, V> &operator<<(std::basic_ostream<U, V> &ostr, const std::atomic<GC::ptr<T>> &ptr)
{
	ostr << ptr.load().get();
	return ostr;
}

#endif
