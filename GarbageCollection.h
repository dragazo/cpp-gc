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

	// defines if T is a router function type - facilitates a type safety mechanism for GC::route().
	template<typename T> struct is_router_function : std::false_type {};

	template<> struct is_router_function<GC::router_fn> : std::true_type {};
	template<> struct is_router_function<GC::mutable_router_fn> : std::true_type {};

public: // -- router functions -- //

	// THE FOLLOWING INFORMATION IS CRITICAL FOR ANY USAGE OF THIS LIBRARY.

	// a type T is defined to be "gc" if it owns an object that is itself considered gc.
	// by definition, GC::ptr, GC::atomic_ptr, and std::atomic<GC::ptr> are gc types.
	// ownership means said "owned" object's lifetime is entirely controlled by the "owner".
	// an object may only have one owner at any point in time - shared ownership is considered non-owning.

	// object reachability traversals are performed by router functions.
	// for a gc type T, its router functions route a function-like object to its owned gc objects recursively.
	// because object ownership cannot be cyclic, this will never degrade into infinite recursion.

	// for a gc type T, a "mutable" owned object is defined to be an object for which you would route to its contents but said contents can be changed after construction.
	// e.g. std::vector, std::list, std::set, and std::unique_ptr are all examples of "mutable" gc types because you own them (and their contents), but their contents can change.
	// an "immutable" or "normal" owned object is defined as any object that is not "mutable".

	// for a normal router event (i.e. GC::router_fn) this must at least route to all owned gc objects.
	// for a mutable router event (i.e. GC::mutable_router_fn) this must at least route to all owned "mutable" gc objects.
	// the mutable router event system is entirely a method for optimization.

	// this represents the router function set for objects of type T.
	// router functions take const T& and a function-like object whose type depends on if it's for normal or mutable router events.
	// the function-like object is guaranteed to be small and trivial - for efficiency, you should take it by value.
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
	template<typename T, typename F, std::enable_if_t<GC::is_router_function<F>::value, int> = 0>
	static void route(const T &obj, F func) { GC::router<std::remove_cv_t<T>>::route(obj, func); }

	// recursively routes to each object in an iteration range - should only be used inside router functions
	template<typename IterBegin, typename IterEnd, typename F, std::enable_if_t<GC::is_router_function<F>::value, int> = 0>
	static void route_range(IterBegin begin, IterEnd end, F func) { for (; begin != end; ++begin) GC::route(*begin, func); }

private: // -- private types -- //

	struct info;

	// the virtual function table type for info objects.
	struct info_vtable
	{
		void(*const destroy)(info&); // a function to destroy the object
		void(*const dealloc)(info&); // a function to deallocate memory - called after destroy

		void(*const route)(info&, router_fn);         // a router function to use for this object
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
		const std::size_t count; // the number of elements in obj

		const info_vtable *const vtable; // virtual function table to use

		std::size_t ref_count = 1;      // the reference count for this allocation
		bool        destroying = false; // marks if the object is currently in the process of being destroyed (multi-delete safety flag)

		bool marked; // only used for GC::collect() - otherwise undefined

		info *prev; // the std::list iterator contract isn't quite what we want
		info *next; // so we need to manage a linked list on our own

		// populates info - ref count starts at 1 - prev/next are undefined
		info(void *_obj, std::size_t _count, const info_vtable *_vtable)
			: obj(_obj), count(_count), vtable(_vtable)
		{}

		// -- helpers -- //

		void destroy() { vtable->destroy(*this); }
		void dealloc() { vtable->dealloc(*this); }

		void route(router_fn func) { vtable->route(*this, func); }
		void mutable_route(mutable_router_fn func) { vtable->mutable_route(*this, func); }
	};

	// used to select a GC::ptr constructor that does not perform a GC::root operation
	struct no_init_t {};

public: // -- router function type definitions -- //

	// type used for a normal router event
	struct router_fn
	{
	private: // -- contents hidden for security -- //

		void(*const func)(info *const &); // raw function pointer to call

		router_fn(void(*_func)(info *const &)) : func(_func) {}
		router_fn(std::nullptr_t) = delete;

		void operator()(info *const &arg) { func(arg); }
		void operator()(info *&&arg) = delete; // for safety - ensures we can't call with an rvalue

		friend class GC;
	};
	// type used for mutable router event
	struct mutable_router_fn
	{
	private: // -- contents hidden for security -- //

		void(*const func)(info *const &); // raw function pointer to call

		mutable_router_fn(void(*_func)(info *const &)) : func(_func) {}
		mutable_router_fn(std::nullptr_t) = delete;

		void operator()(info *const &arg) { func(arg); }
		void operator()(info *&&arg) = delete; // for safety - ensures we can't call with an rvalue

		friend class GC;
	};

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

		element_type *obj; // pointer to the object

		GC::info *handle; // the handle to use for gc management functions.

		friend class GC;

	private: // -- helpers -- //

		// changes what handle we should use, properly unlinking ourselves from the old one and linking to the new one.
		// the new handle must come from a pre-existing ptr object of a compatible type (i.e. static or dynamic cast).
		// _obj must be properly sourced from _handle->obj and non-null if _handle is non-null.
		// if _handle (and _obj) is null, the resulting state is empty.
		// RETURNS TRUE IFF YOU MUST CALL GC::handle_del_list() AFTER THE LOCK ON GC::mutex IS RELEASED.
		bool __reset(element_type *_obj, GC::info *_handle)
		{
			// repoint to the proper object
			obj = _obj;

			// we only need to do anything else if we refer to different gc allocations
			if (handle != _handle)
			{
				bool call_handle_del_list = false;

				// drop our object
				if (handle) call_handle_del_list = GC::__delref(handle);

				// take on other's object
				handle = _handle;
				if (handle) GC::__addref(handle);

				return call_handle_del_list;
			}
			else return false;
		}
		// as __reset but begins by locking GC::mutex. additionally, internally handles the return value flag (see above).
		// this operation is atomic.
		void reset(element_type *_obj, GC::info *_handle)
		{
			bool call_handle_del_list = false;

			{
				std::lock_guard<std::mutex> lock(GC::mutex);
				call_handle_del_list = __reset(_obj, _handle);
			}

			if (call_handle_del_list) GC::handle_del_list();
		}

		// creates an empty ptr but does no elaborate initialization (e.g. DOES NOT ROOT THE INTERNAL HANDLE)
		explicit ptr(GC::no_init_t) : obj(nullptr), handle(nullptr) {}
		// must be used after the no_init_t ctor - ROOTS INTERNAL HANDLE AND SETS HANDLE.
		// _obj must be properly sourced from _handle->obj and non-null if _handle is non-null.
		// if _handle (and _obj) is null, creates a valid, but empty ptr.
		// GC::mutex must be locked prior to invocation.
		// DOES NOT INC THE REF COUNT!!
		void __init(element_type *_obj, GC::info *_handle)
		{
			// initialize pointers before rooting so we always point at something valid while under gc control
			obj = _obj;
			handle = _handle;

			GC::__root(handle);
		}

		// constructs a new ptr instance with the specified obj and handle.
		// for obj and handle: both must either be null or non-null - mixing null/non-null is undefined behavior.
		// INCREMENTS THE REF COUNT on handle (if non-null).
		ptr(element_type *_obj, info *_handle) : obj(_obj), handle(_handle)
		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			GC::__root(handle);
			if (handle) GC::__addref(handle);
		}

	public: // -- ctor / dtor / asgn -- //

		// creates an empty ptr (null)
		ptr(std::nullptr_t = nullptr) : obj(nullptr), handle(nullptr)
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
		ptr(const ptr<J> &other) : obj(static_cast<element_type*>(other.obj)), handle(other.handle)
		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			GC::__root(handle); // register handle as a root
			if (handle) GC::__addref(handle); // we're new - inc ref count
		}

		// assigns a pre-existing gc pointer a new object. allows any conversion that can be statically-checked.
		ptr &operator=(const ptr &other) { reset(other.obj, other.handle); return *this; }
		template<typename J, std::enable_if_t<std::is_convertible<J*, T*>::value, int> = 0>
		ptr &operator=(const ptr<J> &other) { reset(static_cast<element_type*>(other.obj), other.handle); return *this; }

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

	public: // -- misc -- //

		// returns the number of references to the current object.
		// if this object is not pointing at any object, returns 0.
		std::size_t use_count() const
		{
			std::lock_guard<std::mutex> lock(GC::mutex);
			return handle ? handle->ref_count : 0;
		}

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
			// regardless of sources for either ptr, reference counts won't change so we can use a trivial swap
			std::lock_guard<std::mutex> lock(GC::mutex);
			std::swap(obj, other.obj);
			std::swap(handle, other.handle);
		}
		friend void swap(ptr &a, ptr &b) { a.swap(b); }
	};

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
				std::lock_guard<std::mutex> lock(GC::mutex);
				ret.__init(nullptr, nullptr); // initialize - not in same step as below because __init doesn't inc ref count

				// we can ignore this return value because ret is always null prior to this call
				ret.__reset(value.obj, value.handle);
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
			std::unique_lock<std::mutex> lock(GC::mutex);

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

		void swap(atomic_ptr &other)
		{
			// regardless of sources for either ptr, reference counts won't change so we can use a trivial swap
			std::lock_guard<std::mutex> lock(GC::mutex);
			std::swap(value.obj, other.value.obj);
			std::swap(value.handle, other.value.handle);
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

	template<typename T, typename Container>
	struct router<std::stack<T, Container>>
	{
		// intentionally blank - we can't iterate through a stack
	};

	template<typename T, typename Container>
	struct router<std::queue<T, Container>>
	{
		// intentionally blank - we can't iterate through a queue
	};

	template<typename T, typename Container, typename Compare>
	struct router<std::priority_queue<T, Container, Compare>>
	{
		// intentionally blank - we can't iterate through a priority queue
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

		auto lambda = [](info &handle, auto func) { GC::route(*reinterpret_cast<element_type*>(handle.obj), func); };

		static const info_vtable _vtable(
			[](info &handle) { reinterpret_cast<element_type*>(handle.obj)->~element_type(); },
			[](info &handle) { allocator_t::dealloc(handle.obj); },
			lambda,
			lambda
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

		ptr<T> res(GC::no_init_t{});

		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			__link(handle); // link the info object
			res.__init(obj, handle); // initialize ptr with handle
			handle->route(GC::__unroot); // claim this object's children

			__start_timed_collect(); // begin timed collect
		}

		// return the created ptr
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

		auto lambda = [](info &handle, auto func) { for (std::size_t i = 0; i < handle.count; ++i) GC::route(reinterpret_cast<scalar_type*>(handle.obj)[i], func); };

		static const info_vtable _vtable(
			[](info &handle) { for (std::size_t i = 0; i < handle.count; ++i) reinterpret_cast<scalar_type*>(handle.obj)[i].~scalar_type(); },
			[](info &handle) { allocator_t::dealloc(handle.obj); },
			lambda,
			lambda
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
		new (handle) info(obj, scalar_count, &_vtable);
		
		// -- do the garbage collection aspects -- //

		ptr<T> res(GC::no_init_t{});

		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			__link(handle); // link the info object

			// initialize ptr with handle (the cast is safe because element_type is either scalar_type or bound array of scalar_type)
			res.__init(reinterpret_cast<element_type*>(obj), handle);

			handle->route(GC::__unroot); // claim this object's children

			__start_timed_collect(); // begin timed collect
		}

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

		auto lambda = [](info &handle, auto func) { GC::route(*reinterpret_cast<T*>(handle.obj), func); };

		static const info_vtable _vtable(
			[](info &handle) { Deleter()(reinterpret_cast<T*>(handle.obj)); },
			[](info &handle) { allocator_t::dealloc(&handle); },
			lambda,
			lambda
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

		ptr<T> res(GC::no_init_t{});

		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			__link(handle); // link the info object
			res.__init(obj, handle); // initialize ptr with handle
			handle->route(GC::__unroot); // claim this object's children

			__start_timed_collect(); // begin timed collect
		}

		// return the created ptr
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

		auto lambda = [](info &handle, auto func) { for (std::size_t i = 0; i < handle.count; ++i) GC::route(reinterpret_cast<T*>(handle.obj)[i], func); };

		static const info_vtable _vtable(
			[](info &handle) { Deleter()(reinterpret_cast<T*>(handle.obj)); },
			[](info &handle) { allocator_t::dealloc(&handle); },
			lambda,
			lambda
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

		ptr<T[]> res(GC::no_init_t{});

		{
			std::lock_guard<std::mutex> lock(GC::mutex);

			__link(handle); // link the info object
			res.__init(obj, handle); // initialize ptr with handle
			handle->route(GC::__unroot); // claim this object's children

			__start_timed_collect(); // begin timed collect
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
