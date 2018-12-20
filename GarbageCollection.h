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

// ------------------------ //

// -- Garbage Collection -- //

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
	private: typedef decltype(static_cast<void(*)(const T&, F)>(GC::router<std::remove_cv_t<T>>::route)) __type_check; // just used to make sure it exists
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
	// another common category of owned object is a by-value container (e.g. std::vector, std::list, std::set, etc.).
	// another case is a uniquely-owning pointer or reference - e.g. std::unique_ptr, or any other (potentially-smart) pointer/reference to an object you know you own uniquely.
	// of course, these cases can be mixed - e.g. a by-value member std::unique_ptr which is a uniquely-owning pointer to a by-value container std::vector of a gc type.
	
	// it is important to remember that a container type like std::vector<T> is a gc type if T is a gc type.

	// object reachability traversals are performed by router functions.
	// for a gc type T, its router functions route a function-like object to its owned gc objects recursively.
	// because object ownership cannot be cyclic, this will never degrade into infinite recursion.

	// routing to anything you don't own is undefined behavior.
	// thus, although it is legal to route to the "contents" of an owned object (i.e. owned object of an owned object), it is typically dangerous to do so.
	// in general, you should just route to all your by-value members - it is their responsibility to properly route the message the rest of the way.

	// if you are the owner of a gc type object, it is undefined behavior not to route to it (except in the special case of a mutable router function - see below).

	// for a gc type T, a "mutable" owned object is defined to be an owned object for which you could legally route to its contents but said contents can be changed after construction.
	// e.g. std::vector, std::list, std::set, and std::unique_ptr are all examples of "mutable" gc types because you own them (and their contents), but their contents can change.
	// an "immutable" or "normal" owned object is defined as any owned object that is not "mutable".

	// more formally, consider an owned object x:
	// suppose you examine the set of all objects routed-to through x recursively.
	// x is a "mutable" owned gc object iff for any two such router invocation sets taken over the lifetime of x the two sets are different. 

	// it should be noted that re-pointing a GC::ptr, GC::atomic_ptr, or std::atomic<GC::ptr> is not a mutating action for the purposes of classifying a "mutable" owned gc object.
	// this is due to the fact that you would never route to its pointed-to contents due to it being a shared resource.

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

		std::atomic<std::size_t> ref_count = 1;                 // the reference count for this allocation
		
		std::atomic_flag destroying = ATOMIC_FLAG_INIT; // marks if the object is currently in the process of being destroyed (multi-delete safety flag)
		bool             destroy_completed = false;     // marks if destruction completed

		bool marked; // only used for GC::collect() - otherwise undefined

		info *prev; // the std::list iterator contract isn't quite what we want
		info *next; // so we need to manage a linked list on our own

		// populates info - ref count starts at 1 - prev/next are undefined
		info(void *_obj, std::size_t _count, const info_vtable *_vtable)
			: obj(_obj), count(_count), vtable(_vtable)
		{}

		// -- helpers -- //

		void destroy() { std::cerr << "dtor " << this << ' '; vtable->destroy(*this); }
		void dealloc() { std::cerr << "dloc " << this << ' '; vtable->dealloc(*this); }

		void route(router_fn func) { vtable->route(*this, func); }
		void mutable_route(mutable_router_fn func) { vtable->mutable_route(*this, func); }
	};

	// used to select constructor paths that lack the addref step
	static struct no_addref_t {} no_addref;

	// used to select constructor paths that bind a new object
	static struct bind_new_obj_t {} bind_new_obj;

	class smart_handle
	{
	private: // -- data -- //

		// the raw handle to manage.
		// after construction, this must never be modified directly.
		// all modification actions should be delegated to one of the collection_synchronizer repoint cache functions.
		// all actions on handles are non-blocking, including the destructor - because of this handle must be dynamic.
		// this ensures that it'll still be accessible after the smart_handle is destroyed (other logic will delete it).
		info **const handle;

	public: // -- ctor / dtor / asgn -- //

		// initializes the info handle to null and marks it as a root.
		smart_handle(std::nullptr_t = nullptr);

		// initializes the info handle with the specified value and marks it as a root.
		// the init object is added to the objects database in the same atomic step as the handle initialization.
		// init must be the correct value of a current object - thus the return value of raw_handle() cannot be used.
		smart_handle(info *init, bind_new_obj_t);

		// unroots the internal handle - the roots database must not already be locked by the calling thread.
		// if non-null, decrements the reference count.
		~smart_handle();

		// constructs a new smart handle to alias another
		smart_handle(const smart_handle &other);
		// repoints this smart_handle to other - equivalent to this->reset(other)
		smart_handle &operator=(const smart_handle &other);

	public: // -- interface -- //

		// gets the raw handle - guaranteed to outlive the object during a gc cycle.
		// the value of the referenced pointer does not reflect the true current value.
		// said value is only meant as a snapshot of the gc graph structure at an instance for the garbage collector.
		// thus, using the value of the pointer (and not just the reference to the pointer) is undefined behavior.
		// therefore this should never be used as an argument to a smart_handle constructor.
		info *const &raw_handle() const noexcept { return *handle; }

		// safely points the underlying raw handle at the new handle.
		void reset(const smart_handle &new_value);
		// safely points the underlying raw handle at nothing (null).
		void reset();
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

public: // -- ptr -- //

	// a self-managed garbage-collected pointer to type T.
	// if T is an unbound array, it is considered the "array" form, otherwise it is the "scalar" form.
	// scalar and array forms may offer different interfaces.
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
			// since we can't do an atomic swap, we need to do the usual swap algorithm
			// this is to ensure there's never an object that isn't pointed to during an intermediate step

			ptr temp = *this;
			*this = other;
			other = temp;
		}
		friend void swap(ptr &a, ptr &b) { a.swap(b); }
	};

	// !!!!!!!!!!!!!!!!!!
	// ATOMIC_PTR NEEDS SOME MAJOR CHANGES NOW THAT THE ENTIRE SYSTEM HAS BEEN CHANGED
	// !!!!!!!!!!!!!!!!!!

	// defines an atomic gc ptr
	template<typename T>
	struct atomic_ptr
	{
	private: // -- data -- //

		GC::ptr<T> value;

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
			// assignment calls reset(), which is already atomic due to locking GC::mutex
			value = desired;
			return *this;
		}

		void store(const GC::ptr<T> &desired)
		{
			// assignment calls reset(), which is already atomic due to locking GC::mutex
			value = desired;
		}
		GC::ptr<T> load() const
		{
			GC::ptr<T> ret(GC::no_init_t{});

			{
				std::lock_guard<std::mutex> lock(GC::objs.mutex);
				ret.__init(nullptr, nullptr); // initialize - not in same step as below because __init doesn't inc ref count

				// we can ignore this return value because ret is always null prior to this call
				ret.reset(value.obj, value.handle);
			}

			return ret;
		}

		operator GC::ptr<T>() const
		{
			return load();
		}

	public: // -- exchange -- //

		GC::ptr<T> exchange(const GC::ptr<T> &desired)
		{
			// not using lock_guard because we need special lock behavior - see below
			std::unique_lock<std::mutex> lock(GC::objs.mutex);

			GC::ptr<T> ret(GC::no_init_t{});
			ret.__init(nullptr, nullptr); // initialize - not in same step as below because __init doesn't inc ref count

			/*                       */ ret.__reset(value.obj, value.handle); // we can ignore this return value because ret is always null prior to this call
			bool call_handle_del_list = value.__reset(desired.obj, desired.handle);

			// unlock early because we need to do other stuff like call GC::handle_del_list() and ptr ctors/dtors
			lock.unlock();

			// handle __reset return value flag (see __reset() comments).
			if (call_handle_del_list) GC::handle_del_list();

			return ret;
		}

		// !! add compare exchange stuff !! //

	public: // -- lock info -- //

		static constexpr bool is_always_lock_free = false;

		bool is_lock_free() const noexcept { return is_always_lock_free; }

	public: // -- swap -- //

		void swap(atomic_ptr &other) { value.swap(other.value); }
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
	struct router<atomic_ptr<GC::ptr<T>>>
	{
		template<typename F> static void route(const atomic_ptr<T> &atomic, F func)
		{
			// we avoid calling any gc functions by not actually using the load function - we just use the object directly.
			// this is safe because the synchronization mechanism inside atomic is to lock GC::mutex, and routers already assume it's locked anyway.
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
		template<typename F> static void route(const std::queue<T, Container> &stack, F func)
		{
			const auto &container = __get_adapter_container(stack);
			route_range(container.begin(), container.end(), func);
		}
	};

	template<typename T, typename Container, typename Compare>
	struct router<std::priority_queue<T, Container, Compare>>
	{
		template<typename F> static void route(const std::priority_queue<T, Container, Compare> &stack, F func)
		{
			const auto &container = __get_adapter_container(stack);
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
			[](info &handle) { std::cerr << "dtor " << &handle << '\n'; reinterpret_cast<element_type*>(handle.obj)->~element_type(); },
			[](info &handle) { std::cerr << "dloc " << &handle << '\n'; allocator_t::dealloc(handle.obj); },
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

		assert(collection_synchronizer::is_rooted(res.handle.raw_handle()));
		assert(collection_synchronizer::get_current_target(&res.handle.raw_handle()) == handle);

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

private: // -- collection deadlock protector -- //

	class collection_synchronizer
	{
	private: // -- collector-only resources -- //

		static obj_list objs; // the list of all objects under gc consideration

		static std::unordered_set<info *const*> roots; // the set of all root handles - must not be modified directly

		static std::vector<info*> del_list; // the list of info objects that should be destroyed after a collector pass

	private: // -- internal data -- //

		static std::mutex internal_mutex; // mutex used only for internal synchronization

		static std::thread::id collector_thread; // the thread id of the collector - none implies no collection is currently processing

		static obj_list objs_add_cache; // the scheduled obj add operations

		static std::unordered_set<info *const*> roots_add_cache; // the scheduled root operations
		static std::unordered_set<info *const*> roots_remove_cache; // the scheduled unroot operations

		// cache used to support non-blocking handle repoint actions.
		// it is structured such that M[&raw_handle] is what it should be repointed to.
		static std::unordered_map<info**, info*> handle_repoint_cache; 

		// all actions on handles are non-blocking, including the destructor which unroots it.
		// but during a gc cycle, the handle must stay alive, so it must be a dynamic allocation.
		// upon calling schedule_handle_destroy(), it's added to this list for eventual deletion.
		static std::vector<info**> handle_dealloc_list;

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
			auto &get_objs() { return collection_synchronizer::objs; }

			// gets the roots database.
			// it is undefined behavior to call this function if the sentry construction failed (see operator bool).
			auto &get_roots() { return collection_synchronizer::roots; }

			// marks obj for deletion - obj must not have already been marked for deletion.
			// this automatically removes obj from the obj database.
			// it is undefined behavior to call this function if the sentry construction failed (see operator bool).
			void mark_delete(info *obj)
			{
				objs.remove(obj);
				del_list.push_back(obj);
			}
		};

	public: // -- interface -- //

		// returns true iff the calling thread is the current collector thread
		static bool this_is_collector_thread();

		// schedules a handle creation action that points to null.
		// raw_handle need not be initialized prior to this call - it will be immedately repointed to null.
		static void schedule_handle_create_null(info *&raw_handle);
		// schedules a handle creation action that points to a new object - inserts new_obj into the obj database.
		// raw_handle need not be initialized prior to this call - it will be immediately repointed to src_handle.
		// new_obj must not already exist in the gc database.
		static void schedule_handle_create_bind_new_obj(info *&raw_handle, info *new_obj);
		// schedules a handle creation action that points at a pre-existing handle - marks the new handle as a root.
		// raw_handle need not be initialized prior to this call - it will be immediately repointed to src_handle.
		static void schedule_handle_create_alias(info *&raw_handle, info *&src_handle);

		// schedules a handle deletion action - unroots the handle and purges it from the handle repoint cache.
		static void schedule_handle_destroy(info *&raw_handle);

		// schedules a handle unroot operation - unmarks handle as a root.
		static void schedule_handle_unroot(info *const &raw_handle);

		// schedules a handle repoint action.
		// raw_handle shall eventually be repointed to new_value before the next collection action.
		// new_value is the address of an info* - null represents repointing raw_handle to null.
		// if raw_handle is destroyed, it must first be removed from this cache via unschedule_handle().
		static void schedule_handle_repoint(info *&raw_handle, info *const *new_value);

	private: // -- private interface (unsafe) -- //

		// as schedule_handle_root() but not thread safe - you should first lock internal_mutex
		static void __schedule_handle_root(info *&raw_handle);

		// as schedule_handle_unroot() but not thread safe - you should first lock internal_mutex
		static void __schedule_handle_unroot(info *const &raw_handle);

		// as schedule_handle_repoint() but not thread safe - you should lock internal_mutex
		static void __schedule_handle_repoint(info *&raw_handle, info *const *new_value);
		



	public:
		// gets the current target info object of new_value.
		// if new_value is null, returns null.
		// otherwise returns the current repoint target if it's in the repoint database.
		// otherwise returns the current pointed-to value of new_value.
		static info *get_current_target(info *const *new_value);
		// as get_current_target() but not thread safe - you should first lock internal_mutex.
		static info *__get_current_target(info *const *new_value);


		public: 
			static bool is_rooted(info *const &inf)
			{
				std::lock_guard<std::mutex> internal_lock(internal_mutex);

				return roots.find(&inf) != roots.end() || roots_add_cache.find(&inf) != roots_add_cache.end();
			}
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

// standard wrapper for atomic_ptr
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
		// we avoid calling any gc functions by not actually using the load function - we just use the object directly.
		// this is safe because the synchronization mechanism inside atomic is to lock GC::mutex, and routers already assume it's locked anyway.
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
