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
#include <cstdint>
#include <climits>
#include <typeinfo>
#include <iterator>
#include <functional>

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

#include <variant>
#include <optional>

// --------------------- //

// -- cpp-gc settings -- //

// --------------------- //

// controls if extra undefined behavior checks are performed at runtime for user-level code.
// these help safeguard some common und cases at the expense of runtime performance.
// however, if you're sure you never invoke undefined behavior, disabling these could give more performant code.
// at the very least i suggest leaving them on during development and for testing.
// non-zero enables these additional checks - zero disables them.
#define DRAGAZO_GARBAGE_COLLECT_EXTRA_UND_CHECKS 1

// if nonzero, several debugging features are enabled:
// 1) GC::ptr will be set to null upon destruction
#define DRAGAZO_GARBAGE_COLLECT_DEBUGGING_FEATURES 1

// to ease mutex contention among threads, cpp-gc allows you to partition threads into specific disjunction groups for gc.
// only threads in the same disjunction can share objects - it is undefined behavior to violate this.
// if this setting is nonzero, violating disjunction boundaries results in an exception in the violating thread instead of undefined behavior.
// this only applies to pointing a GC::ptr at an object from a different object disjunction set.
// you could potentially also violate this by using raw pointers to access the object (or even worse - add mutable roots to it) - THESE CASES ARE NOT CHECKED.
// because violating gc disjunctions is particularly-bad undefined behavior I HIGHLY SUGGEST YOU LEAVE THIS ON ALWAYS!!
// however, if you're SUPER DUPER confident you don't violate this, you can disable it to save space and time (not worth the risk).
// e.g. if your program will only ever run on a single thread this can safely be disabled with no chance of violation.
#define DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_SAFETY_CHECKS 1

// the default type of lockable to use in wrappers.
// i suggest you use some form of recursive mutex - otherwise e.g. a wrapped container's element type could collect under a lock and deadlock.
// if you want some other type for a specific object, you should use the available template utilities instead of changing this globally.
typedef std::recursive_mutex __gc_default_wrapper_lockable_t;

// -------------------------------- //

// -- utility types forward decl -- //

// -------------------------------- //

// don't use these directly - use their aliases in the GC class.
// e.g. don't use __gc_vector, use GC::vector.

template<typename T, typename Deleter, typename Lockable>
class __gc_unique_ptr;

template<typename T, typename Allocator, typename Lockable>
class __gc_vector;
template<typename T, typename Allocator, typename Lockable>
class __gc_deque;

template<typename T, typename Allocator, typename Lockable>
class __gc_forward_list;
template<typename T, typename Allocator, typename Lockable>
class __gc_list;

template<typename Key, typename Compare, typename Allocator, typename Lockable>
class __gc_set;
template<typename Key, typename Compare, typename Allocator, typename Lockable>
class __gc_multiset;

template<typename Key, typename T, typename Compare, typename Allocator, typename Lockable>
class __gc_map;
template<typename Key, typename T, typename Compare, typename Allocator, typename Lockable>
class __gc_multimap;

template<typename Key, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
class __gc_unordered_set;
template<typename Key, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
class __gc_unordered_multiset;

template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
class __gc_unordered_map;
template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
class __gc_unordered_multimap;

template<typename Lockable, typename ...Types>
class __gc_variant;

template<typename T, typename Lockable>
class __gc_optional;

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
	using is_router_function_object = GC::is_same_any<T, GC::router_fn, GC::mutable_router_fn>;

public: // -- router function definitions -- //

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
    // routing to the same object twice is likewise undefined behavior.
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
	// all mutating actions in a mutable gc object (e.g. adding items to a std::vector<GC::ptr<T>>) must occur in a manner mutually exclusive with the object's router function.
	// this is because any thread may at any point make routing requests to any number of objects under gc management in any order and for any purpose.
	// thus, if you have e.g. a std::vector<GC::ptr<T>>, you should also have a mutex to guard it on insertion/deletion/reordering of elements and for the router function.
	// additionally, if your router function locks one or more mutexes, performing any actions that may self-route (e.g. a full garbage collection) will deadlock if any of the locks are still held.
	// this can be fixed by either unlocking the mutex(es) prior to performing the self-routing action or by switching to a recursive mutex.
	// this would likely need to be encapsulated by methods of your class to ensure external code can't violate this requirement (it is undefined behavior to violate this).

	// on the bright side, cpp-gc has wrappers for all standard containers that internally apply all of this logic without the need to remember it.
	// so, you could use a GC::vector<GC::ptr<T>> instead of a std::vector<GC::ptr<T>> and avoid the need to be careful or encapsulate anything.

	// the following requirements pertain to router functions:
	// for a normal router function (i.e. GC::router_fn) this must at least route to all owned gc objects.
	// for a mutable router function (i.e. GC::mutable_router_fn) this must at least route to all owned "mutable" gc objects.
	// the mutable router function system is entirely a method for optimization and can safely be ignored if desired.
    
	// the following struct represents the router function set for objects of type T.
    // the T in router<T> must not be cv-qualified.
	// router functions must be static, named "route", return void, and take two args: a reference to (possibly cv-qualified) T and a by-value router function object (i.e. GC::router_fn or GC::mutable_router_fn).
	// if you don't care about the efficiency mechanism of mutable router functions, you can define the function type as a template type paramter, but it must be deducible.
	// the default implementation is no-op, which is suitable for any non-gc type.
	// this should not be used directly for routing to owned objects - use the helper function GC::route() and GC::route_range() instead.
	template<typename T>
	struct router
	{
		// make sure we don't accidentally select a cv/ref-qualified default implementation
		static_assert(std::is_same<T, std::remove_cv_t<std::remove_reference_t<T>>>::value, "router type T must not be cv/ref-qualified");

		// defining this as true in any router<T> specialization marks it as trivial (more efficient algorithms).
		// this is only safe iff ALL router functions in said specialization are no-op.
		// otherwise it should be declared false or not declared at all (if not present, false is assumed).
		// if set to true the router function(s) are ignored entirely (in fact, in this case they don't even need to exist).
		static constexpr bool is_trivial = true;
	};

public: // -- router intrinsics -- //

	// get's if T's router is defined as trivial (i.e. this is true iff T is not a gc type).
	// if T's router defines is_trivial to true, this value is true.
	// if T's router defines is_trivial to false or does not define it at all, this value is false.
	template<typename T>
	struct has_trivial_router
	{
	private: // -- helpers -- //

		// given a router type, creates an overload set that, when passed nullptr, resolves to a single function.
		// said function's return type denotes the proper value.
		template<typename R>
		static std::false_type __returns_router_is_trivial(void*);

		template<typename R, std::enable_if_t<R::is_trivial, int> = 0>
		static std::true_type __returns_router_is_trivial(std::nullptr_t);

	public: // -- public stuff -- //

		static constexpr bool value = decltype(__returns_router_is_trivial<router<std::remove_cv_t<std::remove_reference_t<T>>>>(nullptr))::value;
	};

	// gets if all Types... are defined as trivial (i.e. this is true iff all the types are non-gc types).
	// if no types are given, the value is true (i.e. nothing is trivial).
	// equivalent to the C++17 fold expression (has_trivial_router<Types>::value && ...).
	template<typename ...Types>
	struct all_have_trivial_routers : std::true_type {};
	template<typename T1, typename ...TN>
	struct all_have_trivial_routers<T1, TN...> : std::integral_constant<bool, has_trivial_router<T1>::value && all_have_trivial_routers<TN...>::value> {};

public: // -- user-level routing utilities -- //

	// recursively routes to obj - should only be used inside router functions
	template<typename T, typename F, std::enable_if_t<!has_trivial_router<T>::value && is_router_function_object<F>::value, int> = 0>
	static void route(const T &obj, F func)
	{
		// call the underlying router function - only required to be defined for non-trivial routers
		GC::router<std::remove_cv_t<T>>::route(obj, func);
	}
	template<typename T, typename F, std::enable_if_t<has_trivial_router<T>::value && is_router_function_object<F>::value, int> = 0>
	static void route(const T &obj, F func) {}

	// recursively routes to each object in an iteration range - should only be used inside router functions
	template<typename IterBegin, typename IterEnd, typename F, std::enable_if_t<!has_trivial_router<typename std::iterator_traits<IterBegin>::value_type>::value && is_router_function_object<F>::value, int> = 0>
	static void route_range(IterBegin begin, IterEnd end, F func)
	{
		for (; begin != end; ++begin) GC::route(*begin, func);
	}
	template<typename IterBegin, typename IterEnd, typename F, std::enable_if_t<has_trivial_router<typename std::iterator_traits<IterBegin>::value_type>::value && is_router_function_object<F>::value, int> = 0>
	static void route_range(IterBegin begin, IterEnd end, F func) {}

public: // -- exception types -- //

	// exception type thrown by operations that violate testable disjunction rules.
	// DISJUNCTION_SAFETY_CHECKS must be enabled for these to be checked.
	class disjunction_error : public std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

private: // -- private types -- //

	struct info;
	
	class disjoint_module;

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
	public: // -- core info -- //

		void *const       obj;   // pointer to the managed object
		const std::size_t count; // the number of elements in obj (meaning varies by implementer)

		const info_vtable *const vtable; // virtual function table to use

		// the disjunction this handle was constructed in.
		// this must be used for disjoint utility functions.
		// also used for applying disjunction safety checks.
		disjoint_module *const disjunction;

		// populates info - ref count starts at 1 - prev/next are undefined
		info(void *_obj, std::size_t _count, const info_vtable *_vtable)
			: obj(_obj), count(_count), vtable(_vtable), disjunction(disjoint_module::local())
		{}

	public: // -- vtable helpers -- //

		void destroy() { vtable->destroy(*this); }
		void dealloc() { vtable->dealloc(*this); }

		void route(router_fn func) { vtable->route(*this, func); }
		void mutable_route(mutable_router_fn func) { vtable->mutable_route(*this, func); }

	public: // -- special resources -- //

		// reference count - should only be used by disjoint module function under internal_mutex lock
		std::size_t ref_count;

		// mark flag - should only be used by the collector
		bool marked;

		// dlist pointers - should only be modified by obj_list methods.
		// dlists have no other internal synchronization, so external code must make this thread safe if needed.
		info *prev, *next;

	public: // -- traversal utilities -- //

		// marks this object and traverses to all routable targets for recursive marking.
		// objects that have already been marked are skipped, so this is worst case O(n) in the number of existing objects.
		void mark_sweep();
	};

	// used to select constructor paths that bind a new object
	static struct bind_new_obj_t {} bind_new_obj;

	// represents a raw_handle_t value with encapsulated syncronization logic.
	// you should not use raw_handle_t directly - use this instead.
	// NOT THREADSAFE - read/write from several threads on an instance of this object is undefined behavior.
	class smart_handle
	{
	private: // -- data -- //

		// the raw handle to manage.
		// after construction, this must never be modified directly.
		// all modification actions should be delegated to one of the collection_synchronizer functions.
		info *raw;

		// the disjunction this handle was constructed in.
		// this is the disjunction that must be used by disjoint utility functions (all wrapped inside this class).
		// also used for applying disjunction safety checks (if enabled).
		disjoint_module *const disjunction;

		friend class GC;

	private: // -- private interface -- //

		// initializes the info handle with the specified value and marks it as a root.
		// the init object is added to the objects database in the same atomic step as the handle initialization.
		// init must be the correct value of a current object - thus the return value of raw_handle() cannot be used.
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if the object is in a different disjunction.
		smart_handle(info *init, bind_new_obj_t) : disjunction(disjoint_module::local())
		{
			disjunction->schedule_handle_create_bind_new_obj(*this, init);
		}

	public: // -- ctor / dtor / asgn -- //

		// initializes the info handle to null and marks it as a root.
		smart_handle(std::nullptr_t = nullptr) : disjunction(disjoint_module::local())
		{
			disjunction->schedule_handle_create_null(*this);
		}
		
		// constructs a new smart handle to alias another.
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if other's object is in a different disjunction.
		smart_handle(const smart_handle &other) : disjunction(disjoint_module::local())
		{
			disjunction->schedule_handle_create_alias(*this, other);
		}

		// unroots the internal handle.
		~smart_handle()
		{
			disjunction->schedule_handle_destroy(*this);
		}

		// safely repoints this smart_handle to other - equivalent to this->reset(other).
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if other's object is in a different disjunction.
		smart_handle &operator=(const smart_handle &other) { reset(other); return *this; }

	public: // -- interface -- //

		// gets the raw handle - guaranteed to outlive the object during a gc cycle.
		// the value of the referenced pointer does not reflect the true current value.
		// said value is only meant as a snapshot of the gc graph structure at an instance for the garbage collector.
		// thus, using the value of the info* (and not just the pointer to the pointer) is undefined behavior.
		// therefore this should never be used to get an argument for a smart_handle constructor.
		info *const &raw_handle() const noexcept { return raw; }

		// safely repoints the underlying raw handle at the new handle's object.
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if the new handle's object is in a different disjunction.
		void reset(const smart_handle &new_value)
		{
			disjunction->schedule_handle_repoint(*this, new_value);
		}
		// safely repoints the underlying raw handle at no object (null).
		void reset()
		{
			disjunction->schedule_handle_repoint_null(*this);
		}

		// safely swaps the underlying raw handles.
		void swap(smart_handle &other)
		{
			disjunction->schedule_handle_repoint_swap(*this, other);
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

public: // -- array typing helpers -- //

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

public: // -- cv type helping -- //

	// a type representing if T is not cv qualified
	template<typename T>
	using no_cv = std::is_same<T, std::remove_cv_t<T>>;

	// copies the cv qualifiers from From and applies them to To.
	// if To is initially cv-qualified, those qualifiers are dropped prior to performing the cv copying process.
	template<typename From, typename To>
	struct copy_cv
	{
	private: // -- helpers -- //

		typedef std::remove_cv_t<To> Dest;
		typedef std::conditional_t<std::is_const<From>::value, const Dest, Dest> C_Dest;
		typedef std::conditional_t<std::is_volatile<From>::value, volatile C_Dest, C_Dest> CV_Dest;

	public: // -- interface -- //

		typedef CV_Dest type;
	};

	template<typename From, typename To>
	using copy_cv_t = typename copy_cv<From, To>::type;

	// given a type T, aliases type T directly - used for e.g. preserving cv qualifiers during sfinae decltype deduction
	template<typename T> struct type_alias { typedef T type; };

public: // -- ptr -- //

	// a self-managed garbage-collected pointer to type T.
	// if T is an unbound array, it is considered the "array" form, otherwise it is the "scalar" form.
	// scalar and array forms may offer different interfaces.
	// NOT THREADSAFE - this type is NOT internally synchronized.
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

		// the raw handle wrapper - this is where all the important gc logic takes place
		smart_handle handle;

		// IMPORTANT: at all times during a ptr instance's lifetime: obj == null <=> handle == null
		// however, handle cannot be checked for null (in a fast manner due to atomicity).
		// violation of this constraint is undefined behavior.
		// therefore, context must be used to guarantee this.
		// e.g. constructing a new ptr can test this by testing the info* object passed to the obj insertion function for the gc database.
		// e.g. a pre-existing ptr will be assumed to satisfy this.

		friend class GC;

	private: // -- helpers -- //

		// changes what handle we should use, properly unlinking ourselves from the old one and linking to the new one.
		// the new handle must come from a pre-existing ptr object.
		// new_obj must be properly-sourced from new_handle->obj and non-null if new_handle is non-null.
		// if new_handle (and new_obj) is null, the resulting state is empty.
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if the new handle's object is in a different disjunction.
		void reset(element_type *new_obj, const smart_handle &new_handle)
		{
			obj = new_obj;
			handle.reset(new_handle);
		}
		// as reset(nullptr, nullptr) but avoids the intermediate conversion from nullptr to smart_handle
		void reset()
		{
			obj = nullptr;
			handle.reset();
		}

		// constructs a new ptr instance with the specified obj and pre-existing handle.
		// this is equivalent to reset() but done at construction time for efficiency.
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if the new handle's object is in a different disjunction.
		ptr(element_type *new_obj, const smart_handle &new_handle) : obj(new_obj), handle(new_handle) {}

		// constructs a new ptr instance with the specified obj and handle.
		// for obj and handle: both must either be null or non-null - mixing null/non-null is undefined behavior.
		// new_handle is automatically added to the gc database.
		// new_handle must NOT have been sourced via handle.raw_handle().
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if the objects is in a different disjunction.
		ptr(element_type *new_obj, info *new_handle, bind_new_obj_t) : obj(new_obj), handle(new_handle, GC::bind_new_obj) {}

	public: // -- ctor / dtor / asgn -- //

		// creates an empty ptr (null)
		ptr(std::nullptr_t = nullptr) : obj(nullptr), handle(nullptr) {}

		~ptr()
		{
			#if DRAGAZO_GARBAGE_COLLECT_DEBUGGING_FEATURES

			// set obj to null (better to get nullptr exceptions than segfaults)
			// doing/not doing this doesn't matter for the obj/handle nullity assertion because we've ended the ptr's lifetime.
			obj = nullptr;

			#endif
		}

		// constructs a new gc pointer from a pre-existing one. allows any conversion that can be statically-checked.
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if other's object is in a different disjunction.
		ptr(const ptr &other) : obj(other.obj), handle(other.handle)
		{}
		template<typename J, std::enable_if_t<std::is_convertible<J*, T*>::value, int> = 0>
		ptr(const ptr<J> &other) : obj(static_cast<element_type*>(other.obj)), handle(other.handle)
		{}

		// assigns a pre-existing gc pointer a new object. allows any conversion that can be statically-checked.
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if other's object is in a different disjunction.
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

		// points this ptr at nothing (null) and severs ownership of the current object (if any).
		ptr &operator=(std::nullptr_t) { reset(); return *this; }

	public: // -- obj access -- //

		// gets a pointer to the managed object. if this ptr does not point at a managed object, returns null.
		// note that the returned pointer may become invalid if this ptr is reassigned or destroyed.
		element_type *get() const noexcept { return obj; }

		template<typename J = T, std::enable_if_t<std::is_same<T, J>::value && !std::is_same<J, void>::value, int> = 0>
		auto &operator*() const& { return *get(); }
		void operator*() && = delete; // for safety reasons, we don't allow dereferencing an rvalue ptr

		element_type *operator->() const& noexcept { return get(); }
		void operator->() && = delete; // for safety reasons, we don't allow dereferencing an rvalue ptr

		// returns true iff this ptr points to a managed object (non-null)
		explicit operator bool() const noexcept { return get() != nullptr; }

	public: // -- array obj access -- //

		// accesses an item in an array. only defined if T is an array type.
		// undefined behavior if index is out of bounds.
		template<typename J = T, std::enable_if_t<std::is_same<T, J>::value && GC::is_unbound_array<J>::value && !std::is_same<J, void>::value, int> = 0>
		auto &operator[](std::ptrdiff_t index) const& { return get()[index]; }
		void operator[](std::ptrdiff_t) && = delete; // for safety reasons, we don't allow dereferencing an rvalue ptr

		// returns a ptr to the element with the specified index (no bounds checking).
		// said ptr aliases the entire array - the array will not be deleted while the alias is still reachable.
		// if this pointer does not refer to an object, the returned ptr is null and does not alias anything.
		// exactly equivalent to GC::alias(p.get() + index, p).
		template<typename J = T, std::enable_if_t<std::is_same<T, J>::value && GC::is_unbound_array<J>::value, int> = 0>
		[[nodiscard]]
		ptr<element_type> alias(std::ptrdiff_t index) const { return GC::alias(get() + index, *this); }

	public: // -- comparison -- //

		friend bool operator==(const ptr &a, const ptr &b) noexcept { return a.get() == b.get(); }
		friend bool operator!=(const ptr &a, const ptr &b) noexcept { return a.get() != b.get(); }
		friend bool operator<(const ptr &a, const ptr &b) noexcept { return a.get() < b.get(); }
		friend bool operator<=(const ptr &a, const ptr &b) noexcept { return a.get() <= b.get(); }
		friend bool operator>(const ptr &a, const ptr &b) noexcept { return a.get() > b.get(); }
		friend bool operator>=(const ptr &a, const ptr &b) noexcept { return a.get() >= b.get(); }

		friend bool operator==(const ptr &a, const element_type *b) noexcept { return a.get() == b; }
		friend bool operator!=(const ptr &a, const element_type *b) noexcept { return a.get() != b; }
		friend bool operator<(const ptr &a, const element_type *b) noexcept { return a.get() < b; }
		friend bool operator<=(const ptr &a, const element_type *b) noexcept { return a.get() <= b; }
		friend bool operator>(const ptr &a, const element_type *b) noexcept { return a.get() > b; }
		friend bool operator>=(const ptr &a, const element_type *b) noexcept { return a.get() >= b; }

		friend bool operator==(const element_type *a, const ptr &b) noexcept { return a == b.get(); }
		friend bool operator!=(const element_type *a, const ptr &b) noexcept { return a != b.get(); }
		friend bool operator<(const element_type *a, const ptr &b) noexcept { return a < b.get(); }
		friend bool operator<=(const element_type *a, const ptr &b) noexcept { return a <= b.get(); }
		friend bool operator>(const element_type *a, const ptr &b) noexcept { return a > b.get(); }
		friend bool operator>=(const element_type *a, const ptr &b) noexcept { return a >= b.get(); }

	public: // -- swap -- //

		// swaps this ptr and the other ptr without performing unnecessary atomic inc/dec operations for managing the reference counts.
		// if DISJUNCTION_SAFETY_CHECKS are enabled, throws GC::disjunction_error if either raw repoint action would be a disjunction violation.
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
			if (this != &other)
			{
				std::scoped_lock locks(this->mutex, other.mutex);
				value.swap(other.value);
			}
		}
		friend void swap(atomic_ptr &a, atomic_ptr &b) { a.swap(b); }
	};

public: // -- ptr casting -- //

	template<typename To, typename From, std::enable_if_t<std::is_convertible<From, To>::value || std::is_same<std::remove_cv_t<To>, std::remove_cv_t<From>>::value, int> = 0>
	[[nodiscard]]
	static ptr<To> staticCast(const GC::ptr<From> &p)
	{
		return ptr<To>(static_cast<typename ptr<To>::element_type*>(p.obj), p.handle);
	}

	template<typename To, typename From, std::enable_if_t<std::is_polymorphic<From>::value && !std::is_array<To>::value, int> = 0>
	[[nodiscard]]
	static ptr<To> dynamicCast(const GC::ptr<From> &p)
	{
		auto obj = dynamic_cast<typename ptr<To>::element_type*>(p.obj);
		return obj ? ptr<To>(obj, p.handle) : ptr<To>();
	}

	template<typename To, typename From, std::enable_if_t<std::is_same<std::remove_cv_t<To>, std::remove_cv_t<From>>::value, int> = 0>
	[[nodiscard]]
	static ptr<To> constCast(const GC::ptr<From> &p)
	{
		return ptr<To>(const_cast<typename ptr<To>::element_type*>(p.obj), p.handle);
	}

	template<typename To, typename From>
	[[nodiscard]]
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

			std::lock_guard<std::mutex> lock(atomic.mutex);

			GC::route(atomic.value, func);
		}
	};

public: // -- C-style array router specializations -- //

	// routes a message directed at a C-style bounded array to each element in said array
	template<typename T, std::size_t N>
	struct router<T[N]>
	{
		static constexpr bool is_trivial = has_trivial_router<T>::value;

		template<typename F>
		static void route(const T(&objs)[N], F func)
		{
			for (const T &i : objs) GC::route(i, func);
		}
	};

	// ill-formed variant for unbounded arrays
	template<typename T>
	struct router<T[]>
	{
		// intentionally left blank - we don't know the extent, so we can't route to its contents
	};

public: // -- stdlib misc router specializations -- //

	template<typename T, std::size_t N>
	struct router<std::array<T, N>>
	{
		static constexpr bool is_trivial = has_trivial_router<T>::value;

		template<typename F>
		static void route(const std::array<T, N> &arr, F func)
		{
			GC::route_range(arr.begin(), arr.end(), func);
		}
	};

	template<typename T1, typename T2>
	struct router<std::pair<T1, T2>>
	{
		static constexpr bool is_trivial = all_have_trivial_routers<T1, T2>::value;

		template<typename F>
		static void route(const std::pair<T1, T2> &pair, F func)
		{
			GC::route(pair.first, func);
			GC::route(pair.second, func);
		}
	};

	template<typename ...Types>
	struct router<std::tuple<Types...>>
	{
		static constexpr bool is_trivial = all_have_trivial_routers<Types...>::value;

		template<typename F>
		struct helper
		{
			// routes to tuple with index I and recurses to I + 1.
			// as I defaults to 0, route() will route to all tuple elements in order of increasing index.
			template<std::size_t I = 0, std::enable_if_t<(I < sizeof...(Types)), int> = 0>
			static void route(const std::tuple<Types...> &tuple, F func)
			{
				GC::route(std::get<I>(tuple), func);
				route<I + 1>(tuple, func);
			}
			template<std::size_t I = 0, std::enable_if_t<(I >= sizeof...(Types)), int> = 0>
			static void route(const std::tuple<Types...> &tuple, F func) {}
		};

		template<typename F>
		static void route(const std::tuple<Types...> &tuple, F func)
		{
			helper<F>::route(tuple, func);
		}
	};

	template<typename T>
	struct router<std::atomic<T>>
	{
		// ill-formed - we would need to read the atomic's value and if the T in atomic<T> uses a gc function on fetch, we could deadlock.
		// you can avoid this problem if you can avoid calling any gc functions, in which case feel free to make a valid specialization.
	};

public: // -- stdlib container router specializations -- //

	template<typename T, typename Deleter>
	struct router<std::unique_ptr<T, Deleter>>
	{
		static constexpr bool is_trivial = has_trivial_router<T>::value;

		template<typename F>
		static void route(const std::unique_ptr<T, Deleter> &obj, F func)
		{
			if (obj) GC::route(*obj, func);
		}
	};

	template<typename T, typename Allocator>
	struct router<std::vector<T, Allocator>>
	{
		static constexpr bool is_trivial = has_trivial_router<T>::value;

		template<typename F>
		static void route(const std::vector<T, Allocator> &vec, F func)
		{
			GC::route_range(vec.begin(), vec.end(), func);
		}
	};

	template<typename T, typename Allocator>
	struct router<std::deque<T, Allocator>>
	{
		static constexpr bool is_trivial = has_trivial_router<T>::value;

		template<typename F>
		static void route(const std::deque<T, Allocator> &deque, F func)
		{
			GC::route_range(deque.begin(), deque.end(), func);
		}
	};

	template<typename T, typename Allocator>
	struct router<std::forward_list<T, Allocator>>
	{
		static constexpr bool is_trivial = has_trivial_router<T>::value;

		template<typename F>
		static void route(const std::forward_list<T, Allocator> &list, F func)
		{
			GC::route_range(list.begin(), list.end(), func);
		}
	};

	template<typename T, typename Allocator>
	struct router<std::list<T, Allocator>>
	{
		static constexpr bool is_trivial = has_trivial_router<T>::value;

		template<typename F>
		static void route(const std::list<T, Allocator> &list, F func)
		{
			GC::route_range(list.begin(), list.end(), func);
		}
	};
	
	template<typename Key, typename Compare, typename Allocator>
	struct router<std::set<Key, Compare, Allocator>>
	{
		static constexpr bool is_trivial = has_trivial_router<Key>::value;

		template<typename F>
		static void route(const std::set<Key, Compare, Allocator> &set, F func)
		{
			GC::route_range(set.begin(), set.end(), func);
		}
	};

	template<typename Key, typename Compare, typename Allocator>
	struct router<std::multiset<Key, Compare, Allocator>>
	{
		static constexpr bool is_trivial = has_trivial_router<Key>::value;

		template<typename F>
		static void route(const std::multiset<Key, Compare, Allocator> &set, F func)
		{
			GC::route_range(set.begin(), set.end(), func);
		}
	};

	template<typename Key, typename T, typename Compare, typename Allocator>
	struct router<std::map<Key, T, Compare, Allocator>>
	{
		static constexpr bool is_trivial = all_have_trivial_routers<Key, T>::value;

		template<typename F>
		static void route(const std::map<Key, T, Compare, Allocator> &map, F func)
		{
			GC::route_range(map.begin(), map.end(), func);
		}
	};

	template<typename Key, typename T, typename Compare, typename Allocator>
	struct router<std::multimap<Key, T, Compare, Allocator>>
	{
		static constexpr bool is_trivial = all_have_trivial_routers<Key, T>::value;

		template<typename F>
		static void route(const std::multimap<Key, T, Compare, Allocator> &map, F func)
		{
			GC::route_range(map.begin(), map.end(), func);
		}
	};

	template<typename Key, typename Hash, typename KeyEqual, typename Allocator>
	struct router<std::unordered_set<Key, Hash, KeyEqual, Allocator>>
	{
		static constexpr bool is_trivial = has_trivial_router<Key>::value;

		template<typename F>
		static void route(const std::unordered_set<Key, Hash, KeyEqual, Allocator> &set, F func)
		{
			GC::route_range(set.begin(), set.end(), func);
		}
	};

	template<typename Key, typename Hash, typename KeyEqual, typename Allocator>
	struct router<std::unordered_multiset<Key, Hash, KeyEqual, Allocator>>
	{
		static constexpr bool is_trivial = has_trivial_router<Key>::value;

		template<typename F>
		static void route(const std::unordered_multiset<Key, Hash, KeyEqual, Allocator> &set, F func)
		{
			GC::route_range(set.begin(), set.end(), func);
		}
	};

	template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
	struct router<std::unordered_map<Key, T, Hash, KeyEqual, Allocator>>
	{
		static constexpr bool is_trivial = all_have_trivial_routers<Key, T>::value;

		template<typename F>
		static void route(const std::unordered_map<Key, T, Hash, KeyEqual, Allocator> &map, F func)
		{
			GC::route_range(map.begin(), map.end(), func);
		}
	};

	template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
	struct router<std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator>>
	{
		static constexpr bool is_trivial = all_have_trivial_routers<Key, T>::value;

		template<typename F>
		static void route(const std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator> &map, F func)
		{
			GC::route_range(map.begin(), map.end(), func);
		}
	};

	template<typename ...Types>
	struct router<std::variant<Types...>>
	{
		static constexpr bool is_trivial = all_have_trivial_routers<Types...>::value;

		template<typename F>
		static void route(const std::variant<Types...> &var, F func)
		{
			// visit the variant and route to whatever we get
			std::visit([func](const auto &option) { GC::route(option, func); }, var);
		}
	};
	template<typename T>
	struct router<std::optional<T>>
	{
		static constexpr bool is_trivial = has_trivial_router<T>::value;

		template<typename F>
		static void route(const std::optional<T> &opt, F func)
		{
			if (opt.has_value()) GC::route(opt.value(), func);
		}
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
		static constexpr bool is_trivial = has_trivial_router<Container>::value;

		template<typename F> static void route(const std::stack<T, Container> &stack, F func)
		{
			const Container &container = __get_adapter_container(stack);
			GC::route(container, func);
		}
	};

	template<typename T, typename Container>
	struct router<std::queue<T, Container>>
	{
		static constexpr bool is_trivial = has_trivial_router<Container>::value;

		template<typename F> static void route(const std::queue<T, Container> &queue, F func)
		{
			const Container &container = __get_adapter_container(queue);
			GC::route(container, func);
		}
	};

	template<typename T, typename Container, typename Compare>
	struct router<std::priority_queue<T, Container, Compare>>
	{
		static constexpr bool is_trivial = has_trivial_router<Container>::value;

		template<typename F> static void route(const std::priority_queue<T, Container, Compare> &pqueue, F func)
		{
			const Container &container = __get_adapter_container(pqueue);
			GC::route(container, func);
		}
	};

private: // -- aligned raw memory allocators -- //

	// given an alignment, defines functions that allocate blocks of uninitialized memory with at least that alignment.
	template<std::size_t align>
	struct raw_aligned_allocator
	{
		static_assert(align != 0 && (align & (align - 1)) == 0, "align must be a power of 2");

		// allocates size bytes - on failure returns nullptr (no exceptions).
		static void *alloc(std::size_t size)
		{
			if constexpr (align <= alignof(std::max_align_t)) return std::malloc(size);
			else return GC::aligned_malloc(size, align);
		}
		// deallocates a block allocated by alloc().
		static void dealloc(void *p)
		{
			if constexpr (align <= alignof(std::max_align_t)) std::free(p);
			else GC::aligned_free(p);
		}
	};

	// given zero or more types, gets the smallest alignment value that satisfies all types.
	template<typename ...Types>
	static constexpr std::size_t alignment_requirement = std::max({ (std::size_t)1, alignof(Types)... });

	// given zero or more types, gets a raw aligned allocator that satisfies all types.
	template<typename ...Types>
	using raw_aligned_allocator_for = raw_aligned_allocator<alignment_requirement<Types...>> ;

private: // -- checked allocators -- //

	// wrapper for an allocator that additionally performs gc-specific logic.
	// the allocator you provide must return null on allocation failure - MUST NOT THROW.
	// the resulting wrapped allocator (this type) throws std::bad_alloc on allocation failure.
	template<typename allocator_t>
	struct checked_allocator
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

	// given zero or more types, provides a checked allocator type whose alignment is suitable for all specified types.
	// throws std::bad_alloc on allocation failure.
	template<typename ...Types>
	using checked_aligned_allocator_for = checked_allocator<raw_aligned_allocator_for<Types...>>;

private: // -- data block alignment guards -- //

	// given an element type and a number of elements, returns the padded size to safely put an info object at the end.
	// assumes the base address will be at least aligned to the stricter of T and info - does not include space for the info block itself.
	// usage: use this value instead of Count * sizeof(T) for use in single-allocation gc buffers containing one or more objects followed by an info block.
	// this effectively computes Count * sizeof(T) and rounds up to the next multiple of alignof(info).
	template<typename T, std::size_t Count>
	struct pad_size_for_info : std::integral_constant<std::size_t, (Count * sizeof(T)) + ((~(Count * sizeof(T)) + 1) & (alignof(info) - 1))> {};

public: // -- ptr allocation -- //

	// creates a new dynamic instance of T that is bound to a ptr.
	// throws any exception resulting from T's constructor but does not leak resources if this occurs.
	template<typename T, typename ...Args, std::enable_if_t<!std::is_array<T>::value, int> = 0>
	[[nodiscard]]
	static ptr<T> make(Args &&...args)
	{
		// -- normalize T -- //

		// strip cv qualifiers
		typedef std::remove_cv_t<T> element_type;
		
		// get the allocator
		typedef checked_aligned_allocator_for<element_type, info> allocator_t;

		// -- create the vtable -- //

		// use lambda to conveniently create the set of all router functions for type T
		auto router_set = [](info &handle, auto func) { GC::route(*reinterpret_cast<element_type*>(handle.obj), func); };

		static const info_vtable _vtable(
			[](info &handle) { reinterpret_cast<element_type*>(handle.obj)->~element_type(); },
			[](info &handle) { allocator_t::dealloc(handle.obj); },
			router_set,
			router_set
		);

		// -- create the buffer for both the object and its info object -- //

		// alias the pad size type
		typedef pad_size_for_info<element_type, 1> pad_size;

		// allocate the buffer space
		void *const buf = allocator_t::alloc(pad_size::value + sizeof(info));

		// alias the buffer partitions
		element_type *const obj = reinterpret_cast<element_type*>(buf);
		info         *const handle = reinterpret_cast<info*>(reinterpret_cast<char*>(buf) + pad_size::value);

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
	[[nodiscard]]
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
		typedef checked_aligned_allocator_for<scalar_type, info> allocator_t;

		// -- create the vtable -- //

		// use lambda to conveniently create the set of all router functions for type T
		auto router_set = [](info &handle, auto func) { for (std::size_t i = 0; i < handle.count; ++i) GC::route(reinterpret_cast<scalar_type*>(handle.obj)[i], func); };

		static const info_vtable _vtable(
			[](info &handle) { for (std::size_t i = 0; i < handle.count; ++i) reinterpret_cast<scalar_type*>(handle.obj)[i].~scalar_type(); },
			[](info &handle) { allocator_t::dealloc(handle.obj); },
			router_set,
			router_set
		);

		// -- create the buffer for the objects and their info object -- //

		// compute the size of the array
		std::size_t arr_sz = scalar_count * sizeof(scalar_type);

		// to make sure the info object is aligned, round this up to a multiple of alignof(info).
		// note: this is not necessary if the element type size is already a multiple of alignof(info).
		if constexpr ((sizeof(element_type) & (alignof(info) - 1)) != 0)
		{
			arr_sz = arr_sz + ((~arr_sz + 1) & (alignof(info) - 1));
		}

		// allocate the buffer space (owning)
		void *const buf = allocator_t::alloc(arr_sz + sizeof(info));
		
		// alias the two buffer partitions
		scalar_type *const obj = reinterpret_cast<scalar_type*>(buf);
		info        *const handle = reinterpret_cast<info*>(reinterpret_cast<char*>(buf) + arr_sz);

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
	[[nodiscard]]
	static ptr<T> make()
	{
		// create it with the unbound array form and reinterpret it to the bound array form
		return reinterpretCast<T>(make<std::remove_extent_t<T>[]>(std::extent<T>::value));
	}

	// adopts a pre-existing (dynamic) scalar instance of T that is after this call bound to a ptr for management.
	// throws any exception resulting from failed memory allocation.
	// if an exception is thrown, obj is destroyed by the deleter.
	// if obj is null, does nothing and returns a null ptr object (which does not refer to an object).
	// it is UNDEFINED BEHAVIOR for obj to not have a true object type of T - e.g. obj must not be pointer to base.
	// if EXTRA_UND_CHECKS is enabled and T is a polymorphic type, throws std::invalid_argument if this is violated.
	template<typename T, typename Deleter = std::default_delete<T>>
	[[nodiscard]]
	static ptr<T> adopt(T *obj)
	{
		// -- verification -- //

		// if obj is null, return an null ptr
		if (!obj) return {};

		// und for obj to not be of type T - this is because we need the object's real router functions.
		// otherwise we'd be using the wrong router functions and lead to undefined behavior.
		// if T is polymorphic we can check that now - otherwise is left as undefined behavior.
		#if DRAGAZO_GARBAGE_COLLECT_EXTRA_UND_CHECKS
		
		if constexpr (std::is_polymorphic<T>::value)
		{
			if (typeid(*obj) != typeid(T))
			{
				Deleter()(obj);
				throw std::invalid_argument("UND: obj was pointer to base");
			}
		}

		#endif

		// -- normalize T -- //

		// get the allocator
		typedef checked_aligned_allocator_for<info> allocator_t;

		// -- create the vtable -- //

		// use lambda to conveniently create the set of all router functions for type T
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

	// adopts a pre-existing (dynamic) array of T that is after this call bound to a ptr for management.
	// throws any expection resulting from failed memory allocation.
	// if an exception is thrown, the array is destroyed by the deleter.
	// if obj is null, does nothing and returns a null ptr object (which does not refer to an object).
	// otherwise obj must point to a valid array of exactly size count (no more, no less).
	template<typename T, typename Deleter = std::default_delete<T[]>>
	[[nodiscard]]
	static ptr<T[]> adopt(T obj[], std::size_t count)
	{
		// -- verification -- //

		// if obj is null, return an null ptr
		if (!obj) return {};

		// -- normalize T -- //

		// get the allocator
		typedef checked_aligned_allocator_for<info> allocator_t;

		// -- create the vtable -- //

		// use lambda to conveniently create the set of all router functions for type T
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

	// creates a new ptr that points to obj, which is logically considered to be part of src's pointed-to object.
	// src's pointed-to object will not be deleted while the alias is still reachable.
	// thus, the created ptr is an owner of the entire object src points to, but only points to obj.
	// creating an alias to an alias correctly binds to the original object.
	// while allowed, it is ill-advised to alias a part of src's pointed-to object that could be deleted (e.g. element of std::vector).
	// if obj or src is null, creates a null ptr that does not alias an object.
	template<typename T, typename U>
	[[nodiscard]]
	static ptr<T> alias(T *obj, const ptr<U> &src)
	{
		return obj && src ? ptr<T>(obj, src.handle) : ptr<T>();
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

public: // -- wrapper traits -- //

	// the default lockable type to use for wrappers
	typedef __gc_default_wrapper_lockable_t default_lockable_t;

	// given a type T, gets its wrapper traits, including the unwrapped and wrapped type equivalents.
	// the wrapped type must be gc-ready with a properly-mutexed router function.
	// the unwrapped type need not be gc-ready - this is allowed to be identical to the wrapped type.
	// applying unwrapped or wrapped more than once should yield the same type (e.g. wrapped<wrapped<T>> = wrapped<T>).
	// the default implementation returns T for both types if T is not cv qualified, otherwise refers to the non-cv T wrapper_traits with copied cv qualifiers.
	template<typename T>
	struct wrapper_traits
	{
	private: // -- helpers -- //

		// clang has a bug that requires template alias lookups to have the same first-order visibility as the usage site.
		// thus the impl has to go in the public section of something for the alias to be used publically.
		struct impl
		{
			// return type is the unwrapped type to use - uses sfinae to avoid self-referential typedefs.
			template<typename _T = T, std::enable_if_t<std::is_same<T, _T>::value && GC::no_cv<_T>::value, int> = 0>
			static type_alias<_T> _get_unwrapped_type();
			template<typename _T = T, std::enable_if_t<std::is_same<T, _T>::value && !GC::no_cv<_T>::value, int> = 0>
			static type_alias<GC::copy_cv_t<_T, typename wrapper_traits<std::remove_cv_t<_T>>::unwrapped_type>> _get_unwrapped_type();

			// return type is the wrapped type to use - uses sfinae to avoid self-referential typedefs.
			template<typename _Lockable, typename _T = T, std::enable_if_t<std::is_same<T, _T>::value && GC::no_cv<_T>::value, int> = 0>
			static type_alias<_T> _get_wrapped_type();
			template<typename _Lockable, typename _T = T, std::enable_if_t<std::is_same<T, _T>::value && !GC::no_cv<_T>::value, int> = 0>
			static type_alias<GC::copy_cv_t<_T, typename wrapper_traits<std::remove_cv_t<_T>>::template wrapped_type<_Lockable>>> _get_wrapped_type();
		};

	public: // -- typedefs -- //

		// gets an equivalent type that is not necessarily gc-ready
		using unwrapped_type = typename decltype(impl::_get_unwrapped_type())::type;
		
		// gets an equivalent type that is gc ready potentially via usage of _Lockable
		template<typename _Lockable>
		using wrapped_type = typename decltype(impl::template _get_wrapped_type<_Lockable>())::type;
	};

	// given a type T, gets an equivalent type that is not necessarily gc-ready
	template<typename T>
	using make_unwrapped_t = typename wrapper_traits<T>::unwrapped_type;

	// given a type T, gets an equivalent type that is gc-ready
	template<typename T, typename _Lockable = default_lockable_t>
	using make_wrapped_t = typename wrapper_traits<T>::template wrapped_type<_Lockable>;

public: // -- stdlib wrapper aliases -- //

	template<typename T, typename Deleter = std::default_delete<T>, typename _Lockable = default_lockable_t>
	using unique_ptr = make_wrapped_t<std::unique_ptr<T, Deleter>, _Lockable>;

	template<typename T, typename Allocator = std::allocator<T>, typename _Lockable = default_lockable_t>
	using vector = make_wrapped_t<std::vector<T, Allocator>, _Lockable>;
	template<typename T, typename Allocator = std::allocator<T>, typename _Lockable = default_lockable_t>
	using deque = make_wrapped_t<std::deque<T, Allocator>, _Lockable>;

	template<typename T, typename Allocator = std::allocator<T>, typename _Lockable = default_lockable_t>
	using forward_list = make_wrapped_t<std::forward_list<T, Allocator>, _Lockable>;
	template<typename T, typename Allocator = std::allocator<T>, typename _Lockable = default_lockable_t>
	using list = make_wrapped_t<std::list<T, Allocator>, _Lockable>;

	template<typename Key, typename Compare = std::less<Key>, typename Allocator = std::allocator<Key>, typename _Lockable = default_lockable_t>
	using set = make_wrapped_t<std::set<Key, Compare, Allocator>, _Lockable>;
	template<typename Key, typename Compare = std::less<Key>, typename Allocator = std::allocator<Key>, typename _Lockable = default_lockable_t>
	using multiset = make_wrapped_t<std::multiset<Key, Compare, Allocator>, _Lockable>;

	template<typename Key, typename T, typename Compare = std::less<Key>, typename Allocator = std::allocator<std::pair<const Key, T>>, typename _Lockable = default_lockable_t>
	using map = make_wrapped_t<std::map<Key, T, Compare, Allocator>, _Lockable>;
	template<typename Key, typename T, typename Compare = std::less<Key>, typename Allocator = std::allocator<std::pair<const Key, T>>, typename _Lockable = default_lockable_t>
	using multimap = make_wrapped_t<std::multimap<Key, T, Compare, Allocator>, _Lockable>;

	template<typename Key, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>, typename Allocator = std::allocator<Key>, typename _Lockable = default_lockable_t>
	using unordered_set = make_wrapped_t<std::unordered_set<Key, Hash, KeyEqual, Allocator>, _Lockable>;
	template<typename Key, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>, typename Allocator = std::allocator<Key>, typename _Lockable = default_lockable_t>
	using unordered_multiset = make_wrapped_t<std::unordered_multiset<Key, Hash, KeyEqual, Allocator>, _Lockable>;

	template<typename Key, typename T, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>, typename Allocator = std::allocator<std::pair<const Key, T>>, typename _Lockable = default_lockable_t>
	using unordered_map = make_wrapped_t<std::unordered_map<Key, T, Hash, KeyEqual, Allocator>, _Lockable>;
	template<typename Key, typename T, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>, typename Allocator = std::allocator<std::pair<const Key, T>>, typename _Lockable = default_lockable_t>
	using unordered_multimap = make_wrapped_t<std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator>, _Lockable>;

	template<typename ...Types>
	using variant = make_wrapped_t<std::variant<Types...>, default_lockable_t>; // uses default lockable
	template<typename _Lockable, typename ...Types>
	using variant_explicit = make_wrapped_t<std::variant<Types...>, _Lockable>; // uses explicitly-specified lockable

	template<typename T, typename _Lockable = default_lockable_t>
	using optional = make_wrapped_t<std::optional<T>, _Lockable>;

public: // -- stdlib adapter aliases -- //

	template<typename T, typename Container = std::deque<T>, typename _Lockable = default_lockable_t>
	using stack = std::stack<T, make_wrapped_t<Container, _Lockable>>;

	template<typename T, typename Container = std::deque<T>, typename _Lockable = default_lockable_t>
	using queue = std::queue<T, make_wrapped_t<Container, _Lockable>>;
	
	template<typename T, typename Container = std::vector<T>, typename Compare = std::less<typename Container::value_type>, typename _Lockable = default_lockable_t>
	using priority_queue = std::priority_queue<T, make_wrapped_t<Container, _Lockable>, Compare>;

public: // -- gc-specific threading stuff -- //

	// specifies that the new thread should use the primary disjunction (i.e. the one created on initial program start - what the primary thread uses).
	// this is what std::thread and any other os-specific threading utility uses (only because they can't be overridden in standard C++).
	// you'll probably want to avoid using this option - inherit_disjunction is almost always the better, faster, safer choice.
	static struct primary_disjunction_t {} primary_disjunction;
	// specifies that the new thread should inherit its parent's disjunction.
	static struct inherit_disjunction_t {} inherit_disjunction;
	// specifies that the new thread should be in its own (new) disjunction.
	static struct new_disjunction_t {} new_disjunction;

	class thread
	{
	private: // -- data -- //

		std::thread t; // the wrapped thread

	public: // -- typedefs -- //

		typedef std::thread::native_handle_type native_handle_type;
		
		typedef std::thread::id id;

	public: // -- ctor / dtor / asgn --//

		thread() noexcept = default;
		~thread() = default;

		thread(thread &&other) noexcept : t(std::move(other.t)) {}
		thread(std::thread &&other) noexcept : t(std::move(other)) {}

		thread &operator=(thread &&other) noexcept { t = std::move(other.t); return *this; }
		thread &operator=(std::thread &&other) noexcept { t = std::move(other); return *this; }

		template<typename Function, typename ...Args>
		explicit thread(primary_disjunction_t, Function &&f, Args &&...args) : t(std::forward<Function>(f), std::forward<Args>(args)...) {}

		template<typename Function, typename ...Args>
		explicit thread(inherit_disjunction_t, Function &&f, Args &&...args) : t([](shared_disjoint_handle &&parent_module, std::decay_t<Function> &&ff, std::decay_t<Args> &&...fargs)
		{
			// now that we're a new thread, repoint our local disjunction handle to our parent.
			// the danger with non-primary/local shared handles is if the non-primary/local shared handle is the last to be destroyed.
			// this isn't a problem in this case because we can move the non-primary/local handle (func arg) into the local handle slot.
			disjoint_module::local_handle() = std::move(parent_module);

			// then invoke the user function
			std::invoke(std::move(ff), std::move(fargs)...);

		}, disjoint_module::local_handle(), std::forward<Function>(f), std::forward<Args>(args)...)
		{}

		template<typename Function, typename ...Args>
		explicit thread(new_disjunction_t, Function &&f, Args &&...args) : t([](std::decay_t<Function> &&ff, std::decay_t<Args> &&...fargs)
		{
			// now that we're a new thread, repoint our local disjunction handle to a new disjunction
			disjoint_module_container::get().create_new_disjunction(disjoint_module::local_handle());

			// then invoke the user function
			std::invoke(std::move(ff), std::move(fargs)...);

		}, std::forward<Function>(f), std::forward<Args>(args)...)
		{}

	public: // -- observers -- //

		bool joinable() const noexcept { return t.joinable(); }

		std::thread::id get_id() const noexcept { return t.get_id(); }

		native_handle_type native_handle() { return t.native_handle(); }

		static unsigned int hardware_concurrency() noexcept { return std::thread::hardware_concurrency(); }

	public: // -- operations -- //

		void join() { t.join(); }

		void detach() { t.detach(); }

		void swap(thread &other) noexcept { t.swap(other.t); }
		void swap(std::thread &other) noexcept { t.swap(other); }

		friend void swap(thread &a, thread &b) noexcept { using std::swap; swap(a.t, b.t); }
		friend void swap(thread &a, std::thread &b) noexcept { using std::swap; swap(a.t, b); }
		friend void swap(std::thread &a, thread &b) noexcept { using std::swap; swap(a, b.t); }
	};

public: // -- collection logic utilities -- //

	// a sentry object that controls the ignore feature of GC::collect().
	// on construction, begins an ignore action; on destruction ends it.
	// while this object exists, calls to GC::collect() from any thread bound to the same disjoint gc module will be ignored.
	// this is typically only used for preventing deadlock situations caused by calling GC::collect() while a router mutex is held.
	class ignore_collect_sentry
	{
	private: // -- data -- //

		std::size_t prev_count;

		// the disjunction this object was constructed in.
		// must be used for disjoint utility functions involving this object.
		disjoint_module *const disjunction;

	public: // -- ctor / dtor / asgn -- //

		ignore_collect_sentry() : disjunction(disjoint_module::local()) { prev_count = disjunction->begin_ignore_collect(); }
		~ignore_collect_sentry() { disjunction->end_ignore_collect(); }

		// returns true iff there were no ignore actions in progress prior to the start of this one.
		bool no_prev_ignores() const noexcept { return prev_count == 0; }
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

		// returns true iff obj is contained in this list.
		// this is O(n) and for efficiency reasons should only be used in debug code.
		bool contains(info *obj) const noexcept;
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

private: // -- gc disjoint module -- //

	class shared_disjoint_handle;
	class weak_disjoint_handle;

	// provides the gc logic for all objects from a single disjunction.
	// i.e. all objects and GC::ptr info from the set of all threads that can share gc objects with one another.
	// it is undefined behavior to point to or access a gc object from a different disjunction.
	class disjoint_module
	{
	private: // -- synchronization -- //

		std::mutex internal_mutex; // mutex used only for internal synchronization

		std::thread::id collector_thread; // the thread id of the collector - none implies no collection is currently processing

		std::size_t ignore_collect_count = 0; // the number of sources requesting collect actions to be ignored for this module

	private: // -- collector-only resources -- //

		// these objects represent a snapshot of the object graph for use by the collector.
		// the current collector thread need not lock any mutex to use these resources.
		// if you LOCK internal_mutex and find there's NO current collection action, you can modify them.
		
		// the list of all objects currently under gc consideration.
		// during a collection action, any unreachable object in this list is subject to deletion.
		obj_list objs;

		// the set of all rooted handles - DO NOT USE THIS FOR COLLECTION LOGIC.
		// the roots in this set are subject to deletion by arbitrary running threads after the sentry construction.
		// instead, you should use root_objs, which is guaranteed to be safe during the full collection action.
		// note: a root is the address of an info* (i.e. info**) not the info* itself.
		// this is because the same rooted handle can be repointed to a different object and still be a root.
		// this is what supplies information to the root_objs set for the collector.
		std::unordered_set<info*const*> roots;

		// the set of all objects that are pointed-to by rooted handles (guaranteed not to contain null).
		// this should not be modified directly - should only be manipulated by a valid sentry.
		// this is a set to facilitate fast removal and to ensure there are no duplicates (efficiency).
		// this is separate from the roots set for two reasons:
		// 1) it's more convenient / efficient to perform the sweep logic in terms of objects.
		// 2) if the (rooted) handle the root obj was sourced from is destroyed, said (rooted) handle ceases to exist.
		//    this would be a problem later on in the collector, as we'd need to dereference the handle to get the obj to sweep.
		//    but with this approach that's not an issue for the collector, because it won't ever need to be dereferenced again.
		//    and we know the root obj won't be destroyed because the collector is the only one allowed to do that.
		std::unordered_set<info*> root_objs;

		// the list of objects that should be destroyed after a collector pass.
		// this should not be modified directly - should only be manipulated by a valid sentry.
		// when an object is marked for deletion (i.e. unreachable) it is removed from objs and added to this list.
		// the sentry dtor will handle the logic of deleting the objects.
		obj_list del_list;

	private: // -- modified caches -- //

		// these are like caches (see below) but have special rules.
		// for proper usage, read each modified cache's associated comments.

		// controls ref count deletion behavior:
		// if false, delete the objects immediately.
		// if true, cache the request in ref_count_del_cache.
		// thus if false, the cache is considered a collector-only resource and must not be modified.
		// if this is false, it is safe to directly modify the obj list under normal mutex lock.
		bool cache_ref_count_del_actions = false;

		// the shared resource cache for ref count deletion actions.
		// objects in this cache MUST currently be in the obj list (NOT in the obj add cache).
		// DO NOT unlink objects from the obj list when you put them in this list (just a cache).
		// see cache_ref_count_del_actions for how to use this cache properly.
		std::unordered_set<info*> ref_count_del_cache;

	private: // -- caches -- //

		// these objects can be modified at any time so long as internal_mutex is locked.
		// operations on these objects should be non-blocking (don't call arbitrary code while it's locked).
		// among these are several caches:
		// all actions must be non-blocking, but if there's currently a collector thread you can't modify the collector-only resources.
		// thus, in these cases you add the action info to a cache, which will be applied when the collection action terminates.
		// additionally, if there's NOT a collection action in progress, you MUST apply the change immediately (do not cache it).
		// this is because the caches are only flushed upon termination of a collect action, not before one begins.

		// the scheduled obj add operations.
		// used by new obj insertion during a collection action.
		std::unordered_set<info*> objs_add_cache;

		// the scheduled root add/remove operations.
		// these sets must at all times be disjoint.
		std::unordered_set<info*const*> roots_add_cache;
		std::unordered_set<info*const*> roots_remove_cache;

		// cache used to support non-blocking handle repoint actions.
		// it is structured such that M[&raw_handle] is what it should be repointed to.
		std::unordered_map<info**, info*> handle_repoint_cache; 

	public: // -- ctor / dtor / asgn -- //

		disjoint_module() = default;

		~disjoint_module();

		disjoint_module(const disjoint_module&) = delete;
		disjoint_module &operator=(const disjoint_module&) = delete;

	public: // -- interface -- //

		// performs a collection action on (only) this disjoint gc module.
		// returns false iff another thread is performing a collection on this module.
		bool collect();
		// performs a blocking collection - USE WITH IMMENSE CAUTION.
		// if another thread is performing a collection on the current module, waits for it to finish before collecting.
		// equivalent to "while (!collect()) ;"
		void blocking_collect();

		// returns true iff the calling thread is the current collector thread for (only) this disjoint module
		bool this_is_collector_thread();

		// schedules a handle creation action that points to null.
		// raw_handle need not be initialized prior to this call.
		void schedule_handle_create_null(smart_handle &handle);
		// schedules a handle creation action that points to a new object - inserts new_obj into the obj database.
		// new_obj must not already exist in the gc database (see create alias below).
		// the reference count for new_obj is initialized to 1.
		// raw_handle need not be initialized prior to this call.
		void schedule_handle_create_bind_new_obj(smart_handle &handle, info *new_obj);
		// schedules a handle creation action that points at a pre-existing handle - marks the new handle as a root.
		// raw_handle need not be initialized prior to this call.
		// increments the reference count of the referenced target.
		void schedule_handle_create_alias(smart_handle &raw_handle, const smart_handle &src_handle);

		// schedules a handle deletion action - unroots the handle and purges it from the handle repoint cache.
		// for any call to schedule_handle_create_*(), said handle must be sent here before the end of its lifetime.
		// calling this function implies that the handle no longer exists as far as the gc systems are concerned.
		// reference counting and other gc logic will commence due to the gc reference being destroyed.
		void schedule_handle_destroy(const smart_handle &handle);

		// schedules a handle unroot operation - unmarks handle as a root.
		void schedule_handle_unroot(const smart_handle &raw_handle);

		// schedules a handle repoint action.
		// handle shall eventually be repointed to null before the next collection action.
		// automatically performs reference counting logic.
		void schedule_handle_repoint_null(smart_handle &handle);
		// schedules a handle repoint action.
		// handle shall eventually be repointed to new_value before the next collection action.
		// automatically performs reference counting logic.
		void schedule_handle_repoint(smart_handle &handle, const smart_handle &new_value);
		// schedules a handle repoint action that swaps the pointed-to info objects of two handles atomically.
		// handle_a shall eventually point to whatever handle_b used to point to and vice versa.
		void schedule_handle_repoint_swap(smart_handle &handle_a, smart_handle &handle_b);

		// begins an ignore collect action for this disjoint module.
		// returns the number of (active) ignore collect actions prior to the start of this one.
		// e.g. if this returns zero there were no prior ignore collect actions.
		// the same thread that called this must later call end_ignore_collect() exactly once.
		std::size_t begin_ignore_collect();
		// ends an ignore collect action that was previously started by the calling thread.
		void end_ignore_collect();

	private: // -- private interface (unsafe) -- //
		
		// marks handle as a root - internal_mutex should be locked
		void __schedule_handle_root(const smart_handle &handle);
		// unmarks handle as a root - internal_mutex should be locked
		void __schedule_handle_unroot(const smart_handle &handle);

		// the underlying function for all handle repoint actions.
		// handles the logic of managing the repoint cache for repointing handle to target.
		// DOES NOT HANDLE REFERENCE COUNT LOGIC - DO THAT ON YOUR OWN.
		void __raw_schedule_handle_repoint(smart_handle &handle, info *target);

		// gets the current target info object of new_value.
		// otherwise returns the current repoint target if it's in the repoint database.
		// otherwise returns the current pointed-to value of value.
		info *__get_current_target(const smart_handle &handle);

		// performs the reference count decrement logic on target (allowed to be null).
		// internal_lock is the (already-owned) lock on internal_mutex that was taken previously.
		// you should do all your other work first before calling this.
		// if the reference count falls to zero and there's no collection action, deletes the object.
		// otherwise does nothing (aside from the reference count decrement).
		// handles all deletion logic internally.
		// THIS MUST BE THE LAST THING YOU DO UNDER INTERNAL_MUTEX LOCK.
		void __MUST_BE_LAST_ref_count_dec(info *target, std::unique_lock<std::mutex> internal_lock);

	private: // -- factory accessor shared data -- //

		// the repoint target for the local disjunction.
		// this is only meant to be used by logic internally imbedded within the below factory accessor functions.
		// this is not atomic because it's only modified during the dtor of the primary disjunction at static dtor time, and thus threads are not expected to exist.
		// if this is null, use local_handle().get(), otherwise use this (a detour around the destroyed therad_local local handle object which would otherwise point to the same object).
		static disjoint_module *local_detour;

	public: // -- factory accessors -- //

		// gets the primary disjunction - the default one that all threads use unless instructed otherwise.
		// this has static storage duration - thus this disjoint module's lifetime is the entire program lifetime.
		static const shared_disjoint_handle &primary_handle();
		// gets the disjunction that the calling thread belongs to.
		// this has thread_local storage duration - references/pointers to the handle itself should not be passed between threads.
		// IF THIS IS EVER REPOINTED IT MUST HAPPEN PRIOR TO CALLING ANY GC MANAGEMENT FUNCTIONS FROM THE CALLING THREAD!!
		// the only valid repoint targets are sourced from disjoint_module_container::create_new_disjunction().
		// CALLS TO THIS FUNCTION AFTER THE LIFETIME OF THREAD_LOCAL OBJECTS IN A GIVEN THREAD HAVE EXPIRED (E.G. WITHIN STATIC DTORS) IS UNDEFINED BEHAVIOR - SEE local_raw()).
		// THIS SHOULD ONLY BE USED IF THE LOCAL THREAD IS GUARANTEED TO BE ALIVE (still running non-dtor code) - WHENEVER POSSIBLE, USE local() INSTEAD.
		static shared_disjoint_handle &local_handle();

		// gets a pointer to the primary disjunction.
		static disjoint_module *primary();
		// gets a pointer to the local disjunction.
		// works properly even if the local handle has already been destroyed (e.g. for use in static dtors).
		static disjoint_module *local();
	};

	// holds the info concerning all allocated disjunctions across all threads - including the primary disjunction.
	// this is the only source for creating new disjunctions.
	// this is used by the background collector to perform batch collections accross all threads.
	class disjoint_module_container
	{
	private: // -- data -- //

		std::mutex internal_mutex;
		
		// flag that marks if we're performing a collection action.
		bool collecting = false;

		// all the stored disjunctions.
		// this can be modfied under internal_mutex lock unless in collecting mode.
		// in collecting mode this is considered a collector-only resource.
		std::list<weak_disjoint_handle> disjunctions;

		// all the cached disjunction insertion actions.
		// this can be modified under internal_mutex lock.
		// unless in collecting mode, this cache must at all times be empty.
		std::list<weak_disjoint_handle> disjunction_add_cache;

	private: // -- ctor / dtor / asgn -- //

		disjoint_module_container() = default;
		~disjoint_module_container() = default;

		disjoint_module_container(const disjoint_module_container&) = delete;
		disjoint_module_container &operator=(const disjoint_module_container&) = delete;

	public: // -- interface -- //

		// gets the (only) disjoint module container instance
		static disjoint_module_container &get() { static disjoint_module_container c; return c; }

		// creates a new disjunction and stores it to dest.
		// this is the only valid repoint target for the local disjunction handle.
		void create_new_disjunction(shared_disjoint_handle &dest);

		// performs a collection pass for all stored dynamic disjunctions.
		// additionally performs culling logic for dangling disjunction handles.
		// THIS MUST ONLY BE INVOKED BY THE BACKGROUND COLLECTOR!!
		// this is because the internals of this function will repoint the local disjunction handle all over the place and leave it severed.
		// if collect is true, performs a collection on each stored disjunction, otherwise only culls dangling handles.
		void BACKGROUND_COLLECTOR_ONLY___collect(bool collect);
	};

	// data object used by shared/weak disjoint handles - entirely externally-managed
	struct handle_data
	{
	public: // -- data / types -- //

		// buffer for holding the module - empty on construction - must be empty before destruction of this object
		alignas(disjoint_module) char buffer[sizeof(disjoint_module)];

		// type used to represent the tag block
		typedef std::uint64_t tag_t;

		// the bitfield tag used to represent the 3 types of reference counts on this object.
		// this takes the form [high bits: lock][weak][low bits: strong]
		// the utility functions tag_add() and tag_sub() optionally perform additional und testing - i suggest using those instead.
		std::atomic<tag_t> tag = {0};

		// flag marking that the object has been destroyed.
		// used for synchronization between the weak/shared (strong) counter dec logic.
		std::atomic<bool> destroyed_flag = {false};

	public: // -- constants -- //

		static constexpr tag_t strong_bits = 56; // number of bits in the strong field
		static constexpr tag_t weak_bits = 4;   // number of bits in the weak field
		static constexpr tag_t lock_bits = 4;   // number of bits in the lock field

		static_assert(strong_bits + weak_bits + lock_bits <= CHAR_BIT * sizeof(tag_t), "handle data bit field problem");

		static constexpr tag_t strong_1 = (tag_t)1 << (0);                     // corresponds to a 1 in the strong field - can be or'd with other 1 values
		static constexpr tag_t weak_1 = (tag_t)1 << (strong_bits);             // corresponds to a 1 in the weak field - can be or'd with other 1 values
		static constexpr tag_t lock_1 = (tag_t)1 << (strong_bits + weak_bits); // corresponds to a 1 in the lock field - can be or'd with other 1 values

		static constexpr tag_t strong_mask = (((tag_t)1 << strong_bits) - 1) << (0);                   // the mask for the strong field
		static constexpr tag_t weak_mask = (((tag_t)1 << weak_bits) - 1) << (strong_bits);             // the mask for the weak field
		static constexpr tag_t lock_mask = (((tag_t)1 << lock_bits) - 1) << (strong_bits + weak_bits); // the mask for the lock field

	public: // -- utility functions -- //

		// gets the address of the buffer for holding the actual module - see buffer
		disjoint_module *get() noexcept { return reinterpret_cast<disjoint_module*>(buffer); }

		// adds v to the tag atomically and returns the previous value
		tag_t tag_add(tag_t v, std::memory_order order = std::memory_order_seq_cst);
		// subtracts v from the tag atomically and returns the previous value
		tag_t tag_sub(tag_t v, std::memory_order order = std::memory_order_seq_cst);

		// extracts the strong field from an encoded tag
		static constexpr tag_t extr_strong(tag_t v) { return v & strong_mask; }
		// extracts the weak field from an encoded tag
		static constexpr tag_t extr_weak(tag_t v) { return (v & weak_mask) >> (strong_bits); }
		// extracts the lock field from an encoded tag
		static constexpr tag_t extr_lock(tag_t v) { return v >> (strong_bits + weak_bits); }

		// gets the number of non-lock strong referneces - effectively (strong field - lock field)
		static constexpr tag_t non_lock_strongs(tag_t v) { return extr_strong(v) - extr_lock(v); }
	};

	// acts as an owning handle for a gc disjunction module.
	// repointing and destructor actions on this object trigger end-of-thread cleanup for gc resources.
	// thus, you should NEVER EVER use this type as a local object - undefined behavior.
	// this should only be used for the primary/local handles - all others should be weak_disjoint_handle.
	class shared_disjoint_handle
	{
	private: // -- data -- //

		disjoint_module *__module; // the pre-cached module pointer - at all times __module and data are both null or __module == data->get()
		handle_data *data; // the module allocation block - stores all the ref count info and the actual module itself

		friend class weak_disjoint_handle;
		friend class disjoint_module_container;

	private: // -- repointing -- //

		// repoints this handle at other - performs all relevant reference counting logic for automatic cleanup.
		// repointing to the same disjunction data block we already point at is no-op.
		// other must be sourced from a valid SHARED (strong) disjoint handle object.
		// MUST NOT BE USED FOR LOCKING!!
		void reset(handle_data *other = nullptr);

		// performs all logic necessary for locking onto a weak reference.
		// if the lock fails the resulting state is null.
		// other must be sourced from a valid WEAK disjoint handle object.
		// MUST BE USED FOR LOCKING!!
		void lock(handle_data *other);

	public: // -- ctor / dtor / asgn -- //

		// creates an owning handle that does not alias a disjunction
		explicit shared_disjoint_handle(std::nullptr_t = nullptr) noexcept;

		~shared_disjoint_handle();

		// creates a new owning handle that aliases the same disjunction
		shared_disjoint_handle(const shared_disjoint_handle &other);
		// creates a new owning handle that aliases the same disjunction - other is empty after this operation
		shared_disjoint_handle(shared_disjoint_handle &&other) noexcept;

		// repoints this owning handle to other's disjunction
		shared_disjoint_handle &operator=(const shared_disjoint_handle &other);
		// repoints this owning handle to other's disjunction - other is empty after this operation (unless other == this, in which case no-op).
		// this is the only way to use non-primary/local handles and not invoke undefined behavior (so long as you move into a primary/local handle).
		shared_disjoint_handle &operator=(shared_disjoint_handle &&other);

		// repoints this owning handle to null (no owned disjunction)
		shared_disjoint_handle &operator=(std::nullptr_t);

	public: // -- weak handle locking -- //

		// repoints this owning handle to the object aliased by non-owning handle other.
		// if the object aliased by other is at this point invalid, this results in null (for this object).
		shared_disjoint_handle &operator=(const weak_disjoint_handle &other);

	public: // -- module access -- //

		// returns true iff this owning handle aliases a disjunction
		explicit operator bool() const noexcept { return __module != nullptr; }
		// returns true iff this owning handle does not alias a disjunction
		bool operator!() const noexcept { return __module == nullptr; }

		// gets the aliased disjunction - if this handle does not alias a disjunction returns null
		disjoint_module *get() const noexcept { return __module; }

		disjoint_module *operator->() const noexcept { return get(); }
		disjoint_module &operator*() const { return *get(); }
	};
	// the non-owning form of shared_disjoint_handle.
	// this type is how you store local references to the shared version (which should only be used for primary/local handles).
	class weak_disjoint_handle
	{
	private: // -- data -- //

		handle_data *data; // the managed disjunction - EXTERNAL CODE SHOULD NOT MODIFY DIRECTLY - use reset()

		friend class shared_disjoint_handle;

	private: // -- repointing -- //

		// repoints this handle at other - performs all relevant reference counting logic for automatic cleanup.
		// repointing to the same disjunction data block we already point at is no-op.
		// other must be sourced from a valid weak or shared (strong) handle.
		void reset(handle_data *other = nullptr);

	public: // -- ctor / dtor / asgn -- //

		// creates a non-owning handle that does not alias a disjunction
		explicit weak_disjoint_handle(std::nullptr_t = nullptr) noexcept;

		~weak_disjoint_handle();

		// creates a new non-owning handle that aliases the same disjunction
		weak_disjoint_handle(const weak_disjoint_handle &other);
		// creates a new owning handle that aliases the same disjunction - other is empty after this operation
		weak_disjoint_handle(weak_disjoint_handle &&other) noexcept;
		
		// repoints this non-owning handle to other's disjunction
		weak_disjoint_handle &operator=(const weak_disjoint_handle &other);
		// repoints this non-owning handle to other's disjunction - other is empty after this operation (unless other == this, in which case no-op).
		weak_disjoint_handle &operator=(weak_disjoint_handle &&other);

		// repoints this non-owning handle to null (no owned disjunction)
		weak_disjoint_handle &operator=(std::nullptr_t);

	public: // -- shared handle aliasing -- //

		// creates a new non-owning handle that aliases the disjunction aliased by an owning handle
		weak_disjoint_handle(const shared_disjoint_handle &other);
		// reploints this non-owning handle to the disjunction aliased by an owning handle
		weak_disjoint_handle &operator=(const shared_disjoint_handle &other);

	public: // -- module queries -- //

		// returns true iff the last owning handle to the aliased disjunction has been severed.
		// by its very nature this function is racy - depending on your need locking might be a better option.
		// if this handle does not alias a disjunction, returns true.
		bool expired() const noexcept;
	};

	friend struct __gc_primary_usage_guard_t;
	friend struct __gc_local_usage_guard_t;

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
	[[nodiscard]]
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
	
	// the first invocation of this function begins a new thread to perform timed garbage collection.
	// all subsequent invocations do nothing.
	static void start_timed_collect();

private: // -- utility router functions -- //
	
	static void router_unroot(const smart_handle &arc);
};

// ------------------------- //

// -- std specializations -- //

// ------------------------- //

// hash functions for gc ptr
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

// ------------------------- //

// -- stdlib wrapper impl -- //

// ------------------------- //

template<typename T, typename Deleter, typename Lockable>
class __gc_unique_ptr
{
private: // -- data -- //

	typedef std::unique_ptr<T, Deleter> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_unique_ptr>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

public: // -- types -- //

	typedef typename wrapped_t::pointer pointer;

	typedef typename wrapped_t::element_type element_type;

	typedef typename wrapped_t::deleter_type deleter_type;

public: // -- ctor / dtor -- //

	constexpr __gc_unique_ptr() noexcept
	{
		new (buffer) wrapped_t();
	}
	constexpr __gc_unique_ptr(std::nullptr_t) noexcept
	{
		new (buffer) wrapped_t(nullptr);
	}

	explicit __gc_unique_ptr(pointer p) noexcept
	{
		new (buffer) wrapped_t(p);
	}

	template<typename D>
	__gc_unique_ptr(pointer p, D &&d)
	{
		new (buffer) wrapped_t(p, std::forward<D>(d)); // covers both deleter constructor forms
	}

	__gc_unique_ptr(__gc_unique_ptr &&other) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_unique_ptr(wrapped_t &&other) noexcept
	{
		new (buffer) wrapped_t(std::move(other));
	}

	template<typename U, typename E, typename L>
	__gc_unique_ptr(__gc_unique_ptr<U, E, L> &&other) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	template<typename U, typename E>
	__gc_unique_ptr(std::unique_ptr<U, E> &&other) noexcept
	{
		new (buffer) wrapped_t(std::move(other));
	}

	~__gc_unique_ptr()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_unique_ptr &operator=(__gc_unique_ptr &&other) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_unique_ptr &operator=(wrapped_t &&other) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	template<typename U, typename E, typename L, std::enable_if_t<!std::is_same<__gc_unique_ptr, __gc_unique_ptr<U, E, L>>::value, int> = 0>
	__gc_unique_ptr &operator=(__gc_unique_ptr<U, E, L> &&other) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::scoped_lock locks(this->mutex, other.mutex);
		wrapped() = std::move(other.wrapped());
		return *this;
	}
	template<typename U, typename E, std::enable_if_t<!std::is_same<wrapped_t, std::unique_ptr<U, E>>::value, int> = 0>
	__gc_unique_ptr &operator=(std::unique_ptr<U, E> &&other) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_unique_ptr &operator=(std::nullptr_t) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(this->mutex);
		wrapped() = nullptr;
		return *this;
	}

public: // -- management -- //

	pointer release() noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(this->mutex);
		return wrapped().release();
	}

	void reset(pointer ptr = pointer()) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(this->mutex);
		wrapped().reset(ptr);
	}

	template<typename U, typename Z = T, std::enable_if_t<std::is_same<T, Z>::value && GC::is_unbound_array<T>::value, int> = 0>
	void reset(U other) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(this->mutex);
		wrapped().reset(other);
	}

	template<typename Z = T, std::enable_if_t<std::is_same<T, Z>::value && GC::is_unbound_array<T>::value, int> = 0>
	void reset(std::nullptr_t = nullptr) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(this->mutex);
		wrapped().reset(nullptr);
	}

	void swap(__gc_unique_ptr &other) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other) noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_unique_ptr &a, __gc_unique_ptr &b) { a.swap(b); }
	friend void swap(__gc_unique_ptr &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_unique_ptr &b) { b.swap(a); }

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
template<typename T, typename Deleter, typename Lockable>
struct GC::router<__gc_unique_ptr<T, Deleter, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::has_trivial_router<T>::value;

	template<typename F>
	static void route(const __gc_unique_ptr<T, Deleter, Lockable> &obj, F func)
	{
		std::lock_guard lock(obj.mutex);
		GC::route(obj.wrapped(), func);
	}
};

// -- __gc_unique_ptr cmp -- //

template<typename T1, typename D1, typename L1, typename T2, typename D2, typename L2>
bool operator==(const __gc_unique_ptr<T1, D1, L1> &a, const __gc_unique_ptr<T2, D2, L2> &b) { return a.get() == b.get(); }
template<typename T1, typename D1, typename L1, typename T2, typename D2, typename L2>
bool operator!=(const __gc_unique_ptr<T1, D1, L1> &a, const __gc_unique_ptr<T2, D2, L2> &b) { return a.get() != b.get(); }
template<typename T1, typename D1, typename L1, typename T2, typename D2, typename L2>
bool operator<(const __gc_unique_ptr<T1, D1, L1> &a, const __gc_unique_ptr<T2, D2, L2> &b) { return a.get() < b.get(); }
template<typename T1, typename D1, typename L1, typename T2, typename D2, typename L2>
bool operator<=(const __gc_unique_ptr<T1, D1, L1> &a, const __gc_unique_ptr<T2, D2, L2> &b) { return a.get() <= b.get(); }
template<typename T1, typename D1, typename L1, typename T2, typename D2, typename L2>
bool operator>(const __gc_unique_ptr<T1, D1, L1> &a, const __gc_unique_ptr<T2, D2, L2> &b) { return a.get() > b.get(); }
template<typename T1, typename D1, typename L1, typename T2, typename D2, typename L2>
bool operator>=(const __gc_unique_ptr<T1, D1, L1> &a, const __gc_unique_ptr<T2, D2, L2> &b) { return a.get() >= b.get(); }

template<typename T, typename D, typename L>
bool operator==(const __gc_unique_ptr<T, D, L> &x, std::nullptr_t) { return x.get() == nullptr; }
template<typename T, typename D, typename L>
bool operator==(std::nullptr_t, const __gc_unique_ptr<T, D, L> &x) { return nullptr == x.get(); }

template<typename T, typename D, typename L>
bool operator!=(const __gc_unique_ptr<T, D, L> &x, std::nullptr_t) { return x.get() != nullptr; }
template<typename T, typename D, typename L>
bool operator!=(std::nullptr_t, const __gc_unique_ptr<T, D, L> &x) { return nullptr != x.get(); }

template<typename T, typename D, typename L>
bool operator<(const __gc_unique_ptr<T, D, L> &x, std::nullptr_t) { return x.get() < nullptr; }
template<typename T, typename D, typename L>
bool operator<(std::nullptr_t, const __gc_unique_ptr<T, D, L> &x) { return nullptr < x.get(); }

template<typename T, typename D, typename L>
bool operator<=(const __gc_unique_ptr<T, D, L> &x, std::nullptr_t) { return x.get() <= nullptr; }
template<typename T, typename D, typename L>
bool operator<=(std::nullptr_t, const __gc_unique_ptr<T, D, L> &x) { return nullptr <= x.get(); }

template<typename T, typename D, typename L>
bool operator>(const __gc_unique_ptr<T, D, L> &x, std::nullptr_t) { return x.get() > nullptr; }
template<typename T, typename D, typename L>
bool operator>(std::nullptr_t, const __gc_unique_ptr<T, D, L> &x) { return nullptr > x.get(); }

template<typename T, typename D, typename L>
bool operator>=(const __gc_unique_ptr<T, D, L> &x, std::nullptr_t) { return x.get() >= nullptr; }
template<typename T, typename D, typename L>
bool operator>=(std::nullptr_t, const __gc_unique_ptr<T, D, L> &x) { return nullptr >= x.get(); }

// --------------------------------------------------------------

template<typename T, typename Allocator, typename Lockable>
class __gc_vector
{
private: // -- data -- //

	typedef std::vector<T, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_vector>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	__gc_vector()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_vector(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	__gc_vector(size_type count, const T& value, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(count, value, alloc);
	}

	explicit __gc_vector(size_type count, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(count, alloc);
	}

	template<typename InputIt>
	__gc_vector(InputIt first, InputIt last, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, alloc);
	}

	__gc_vector(const __gc_vector &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_vector(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_vector(const __gc_vector &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_vector(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_vector(__gc_vector &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_vector(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_vector(__gc_vector &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_vector(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_vector(std::initializer_list<T> init, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, alloc);
	}

	~__gc_vector()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_vector &operator=(const __gc_vector &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_vector &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_vector &operator=(__gc_vector &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_vector &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_vector &operator=(std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = ilist;
		return *this;
	}

	void assign(size_type count, const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().assign(count, value);
	}

	template<typename InputIt>
	void assign(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().assign(first, last);
	}

	void assign(std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().reserve(new_cap);
	}
	size_type capacity() const noexcept { return wrapped().capacity(); }

	void shrink_to_fit()
	{
		std::lock_guard lock(this->mutex);
		wrapped().shrink_to_fit();
	}

	void clear() noexcept(noexcept(std::declval<Lockable>().lock()))
	{
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	iterator insert(const_iterator pos, const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, value);
	}
	iterator insert(const_iterator pos, T &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, std::move(value));
	}

	iterator insert(const_iterator pos, size_type count, const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, count, value);
	}

	template<typename InputIt>
	iterator insert(const_iterator pos, InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, first, last);
	}

	iterator insert(const_iterator pos, std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, ilist);
	}

	template<typename ...Args>
	iterator emplace(const_iterator pos, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

public: // -- push / pop -- //

	void push_back(const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_back(value);
	}
	void push_back(T &&value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_back(std::move(value));
	}

	template<typename ...Args>
	decltype(auto) emplace_back(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_back(std::forward<Args>(args)...);
	}

	void pop_back()
	{
		std::lock_guard lock(this->mutex);
		wrapped().pop_back();
	}

public: // -- resize -- //

	void resize(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().resize(count);
	}
	void resize(size_type count, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().resize(count, value);
	}

public: // -- swap -- //

	void swap(__gc_vector &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_vector &a, __gc_vector &b) { a.swap(b); }
	friend void swap(__gc_vector &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_vector &b) { b.swap(a); }

public: // -- cmp -- //

	friend bool operator==(const __gc_vector &a, const __gc_vector &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_vector &a, const __gc_vector &b) { return a.wrapped() != b.wrapped(); }
	friend bool operator<(const __gc_vector &a, const __gc_vector &b) { return a.wrapped() < b.wrapped(); }
	friend bool operator<=(const __gc_vector &a, const __gc_vector &b) { return a.wrapped() <= b.wrapped(); }
	friend bool operator>(const __gc_vector &a, const __gc_vector &b) { return a.wrapped() > b.wrapped(); }
	friend bool operator>=(const __gc_vector &a, const __gc_vector &b) { return a.wrapped() >= b.wrapped(); }
};
template<typename T, typename Allocator, typename Lockable>
struct GC::router<__gc_vector<T, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::has_trivial_router<T>::value;

	template<typename F>
	static void route(const __gc_vector<T, Allocator, Lockable> &vec, F func)
	{
		std::lock_guard lock(vec.mutex);
		GC::route(vec.wrapped(), func);
	}
};

template<typename T, typename Allocator, typename Lockable>
class __gc_deque
{
private: // -- data -- //

	typedef std::deque<T, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // the router synchronizer

	friend struct GC::router<__gc_deque>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	__gc_deque()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_deque(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	__gc_deque(size_type count, const T &value, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(count, value, alloc);
	}

	explicit __gc_deque(size_type count, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(count, alloc);
	}

	template<typename InputIt>
	__gc_deque(InputIt first, InputIt last, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, alloc);
	}

	__gc_deque(const __gc_deque &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_deque(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_deque(const __gc_deque &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_deque(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_deque(__gc_deque &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_deque(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_deque(__gc_deque &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_deque(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_deque(std::initializer_list<T> init, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, alloc);
	}

	~__gc_deque()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_deque &operator=(const __gc_deque &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_deque &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_deque &operator=(__gc_deque &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_deque &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_deque &operator=(std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = ilist;
		return *this;
	}

	void assign(size_type count, const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().assign(count, value);
	}

	template<typename InputIt>
	void assign(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().assign(first, last);
	}

	void assign(std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().reserve(new_cap);
	}
	size_type capacity() const noexcept { return wrapped().capacity(); }

	void shrink_to_fit()
	{
		std::lock_guard lock(this->mutex);
		wrapped().shrink_to_fit();
	}

	void clear() noexcept
	{
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	iterator insert(const_iterator pos, const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, value);
	}
	iterator insert(const_iterator pos, T &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, std::move(value));
	}

	iterator insert(const_iterator pos, size_type count, const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, count, value);
	}

	template<typename InputIt>
	iterator insert(const_iterator pos, InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, first, last);
	}

	iterator insert(const_iterator pos, std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, ilist);
	}

	template<typename ...Args>
	iterator emplace(const_iterator pos, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

public: // -- push / pop -- //

	void push_back(const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_back(value);
	}
	void push_back(T &&value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_back(std::move(value));
	}

	template<typename ...Args>
	decltype(auto) emplace_back(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_back(std::forward<Args>(args)...);
	}

	void pop_back()
	{
		std::lock_guard lock(this->mutex);
		wrapped().pop_back();
	}

	void push_front(const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_front(value);
	}
	void push_front(T &&value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_front(std::move(value));
	}

	template<typename ...Args>
	decltype(auto) emplace_front(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_front(std::forward<Args>(args)...);
	}

	void pop_front()
	{
		std::lock_guard lock(this->mutex);
		wrapped().pop_front();
	}

public: // -- resize -- //

	void resize(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().resize(count);
	}
	void resize(size_type count, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().resize(count, value);
	}

public: // -- swap -- //

	void swap(__gc_deque &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_deque &a, __gc_deque &b) { a.swap(b); }
	friend void swap(__gc_deque &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_deque &b) { b.swap(a); }

public: // -- cmp -- //

	friend bool operator==(const __gc_deque &a, const __gc_deque &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_deque &a, const __gc_deque &b) { return a.wrapped() != b.wrapped(); }
	friend bool operator<(const __gc_deque &a, const __gc_deque &b) { return a.wrapped() < b.wrapped(); }
	friend bool operator<=(const __gc_deque &a, const __gc_deque &b) { return a.wrapped() <= b.wrapped(); }
	friend bool operator>(const __gc_deque &a, const __gc_deque &b) { return a.wrapped() > b.wrapped(); }
	friend bool operator>=(const __gc_deque &a, const __gc_deque &b) { return a.wrapped() >= b.wrapped(); }
};
template<typename T, typename Allocator, typename Lockable>
struct GC::router<__gc_deque<T, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::has_trivial_router<T>::value;

	template<typename F>
	static void route(const __gc_deque<T, Allocator, Lockable> &vec, F func)
	{
		std::lock_guard lock(vec.mutex);
		GC::route(vec.wrapped(), func);
	}
};

template<typename T, typename Allocator, typename Lockable>
class __gc_forward_list
{
private: // -- data -- //

	typedef std::forward_list<T, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_forward_list>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	__gc_forward_list()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_forward_list(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	__gc_forward_list(size_type count, const T &value, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(count, value, alloc);
	}

	explicit __gc_forward_list(size_type count, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(count, alloc);
	}

	template<typename InputIt>
	__gc_forward_list(InputIt first, InputIt last, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, alloc);
	}

	__gc_forward_list(const __gc_forward_list &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_forward_list(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_forward_list(const __gc_forward_list &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_forward_list(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_forward_list(__gc_forward_list &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_forward_list(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_forward_list(__gc_forward_list &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_forward_list(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_forward_list(std::initializer_list<T> init, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, alloc);
	}

	~__gc_forward_list()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_forward_list &operator=(const __gc_forward_list &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_forward_list &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_forward_list &operator=(__gc_forward_list &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_forward_list &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_forward_list &operator=(std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = ilist;
		return *this;
	}

	void assign(size_type count, const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().assign(count, value);
	}

	template<typename InputIt>
	void assign(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().assign(first, last);
	}

	void assign(std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	iterator insert_after(const_iterator pos, const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_after(pos, value);
	}
	iterator insert_after(const_iterator pos, T &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_after(pos, std::move(value));
	}

	iterator insert_after(const_iterator pos, size_type count, const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_after(pos, count, value);
	}

	template<typename InputIt>
	iterator insert_after(const_iterator pos, InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_after(pos, first, last);
	}

	iterator insert_after(const_iterator pos, std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_after(pos, ilist);
	}

	template<typename ...Args>
	iterator emplace_after(const_iterator pos, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_after(pos, std::forward<Args>(args)...);
	}

	iterator erase_after(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase_after(pos);
	}
	iterator erase_after(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase_after(first, last);
	}

public: // -- push / pop -- //

	void push_front(const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_front(value);
	}
	void push_front(T &&value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_front(std::move(value));
	}

	template<typename ...Args>
	decltype(auto) emplace_front(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_front(std::forward<Args>(args)...);
	}

	void pop_front()
	{
		std::lock_guard lock(this->mutex);
		wrapped().pop_front();
	}

public: // -- resize -- //

	void resize(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().resize(count);
	}
	void resize(size_type count, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().resize(count, value);
	}

public: // -- swap -- //

	void swap(__gc_forward_list &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_forward_list &a, __gc_forward_list &b) { a.swap(b); }
	friend void swap(__gc_forward_list &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_forward_list &b) { b.swap(a); }

public: // -- cmp -- //

	friend bool operator==(const __gc_forward_list &a, const __gc_forward_list &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_forward_list &a, const __gc_forward_list &b) { return a.wrapped() != b.wrapped(); }
	friend bool operator<(const __gc_forward_list &a, const __gc_forward_list &b) { return a.wrapped() < b.wrapped(); }
	friend bool operator<=(const __gc_forward_list &a, const __gc_forward_list &b) { return a.wrapped() <= b.wrapped(); }
	friend bool operator>(const __gc_forward_list &a, const __gc_forward_list &b) { return a.wrapped() > b.wrapped(); }
	friend bool operator>=(const __gc_forward_list &a, const __gc_forward_list &b) { return a.wrapped() >= b.wrapped(); }

public: // -- merge -- //

	void merge(__gc_forward_list &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().merge(other.wrapped());
		}
	}
	void merge(__gc_forward_list &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().merge(std::move(other.wrapped()));
		}
	}

	template<typename Compare>
	void merge(__gc_forward_list &other, Compare comp)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().merge(other.wrapped(), comp);
		}
	}
	template<typename Compare>
	void merge(__gc_forward_list &&other, Compare comp)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().merge(std::move(other.wrapped()), comp);
		}
	}

	// ------------------------------------------------------------

	void merge(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().merge(other);
	}
	void merge(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().merge(std::move(other));
	}

	template<typename Compare>
	void merge(wrapped_t &other, Compare comp)
	{
		std::lock_guard lock(this->mutex);
		wrapped().merge(other, comp);
	}
	template<typename Compare>
	void merge(wrapped_t &&other, Compare comp)
	{
		std::lock_guard lock(this->mutex);
		wrapped().merge(std::move(other), comp);
	}

public: // -- splice -- //

	void splice_after(const_iterator pos, __gc_forward_list &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped());
		}
	}
	void splice_after(const_iterator pos, __gc_forward_list &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()));
		}
	}

	void splice_after(const_iterator pos, __gc_forward_list &other, const_iterator it)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped(), it);
		}
	}
	void splice_after(const_iterator pos, __gc_forward_list &&other, const_iterator it)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()), it);
		}
	}

	void splice_after(const_iterator pos, __gc_forward_list &other, const_iterator first, const_iterator last)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped(), first, last);
		}
	}
	void splice_after(const_iterator pos, __gc_forward_list &&other, const_iterator first, const_iterator last)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()), first, last);
		}
	}

	// ------------------------------------------------------------

	void splice_after(const_iterator pos, wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, other);
	}
	void splice_after(const_iterator pos, wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, std::move(other));
	}

	void splice_after(const_iterator pos, wrapped_t &other, const_iterator it)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, other, it);
	}
	void splice_after(const_iterator pos, wrapped_t &&other, const_iterator it)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, std::move(other), it);
	}

	void splice_after(const_iterator pos, wrapped_t &other, const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, other, first, last);
	}
	void splice_after(const_iterator pos, wrapped_t &&other, const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, std::move(other), first, last);
	}

public: // -- remove -- //

	decltype(auto) remove(const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().remove(value);
	}

	template<typename UnaryPredicate>
	decltype(auto) remove_if(UnaryPredicate p)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().remove_if(p);
	}

public: // -- ordering -- //

	void reverse() noexcept
	{
		std::lock_guard lock(this->mutex);
		wrapped().reverse();
	}

	decltype(auto) unique()
	{
		std::lock_guard lock(this->mutex);
		return wrapped().unique();
	}
	template<typename BinaryPredicate>
	decltype(auto) unique(BinaryPredicate p)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().unique(p);
	}

	void sort()
	{
		std::lock_guard lock(this->mutex);
		wrapped().sort();
	}
	template<typename Compare>
	void sort(Compare comp)
	{
		std::lock_guard lock(this->mutex);
		wrapped().sort(comp);
	}
};
template<typename T, typename Allocator, typename Lockable>
struct GC::router<__gc_forward_list<T, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::has_trivial_router<T>::value;

	template<typename F>
	static void route(const __gc_forward_list<T, Allocator, Lockable> &list, F func)
	{
		std::lock_guard lock(list.mutex);
		GC::route(list.wrapped(), func);
	}
};

template<typename T, typename Allocator, typename Lockable>
class __gc_list
{
private: // -- data -- //

	typedef std::list<T, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_list>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	__gc_list()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_list(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	__gc_list(size_type count, const T &value = T(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(count, value, alloc);
	}

	explicit __gc_list(size_type count, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(count, alloc);
	}

	template<typename InputIt>
	__gc_list(InputIt first, InputIt last, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, alloc);
	}

	__gc_list(const __gc_list &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_list(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_list(const __gc_list &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_list(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_list(__gc_list &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_list(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_list(__gc_list &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_list(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_list(std::initializer_list<T> init, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, alloc);
	}

	~__gc_list()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_list &operator=(const __gc_list &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_list &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_list &operator=(__gc_list &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_list &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_list &operator=(std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = ilist;
		return *this;
	}

	void assign(size_type count, const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().assign(count, value);
	}

	template<typename InputIt>
	void assign(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().assign(first, last);
	}

	void assign(std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	iterator insert(const_iterator pos, const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, value);
	}
	iterator insert(const_iterator pos, T &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, std::move(value));
	}

	iterator insert(const_iterator pos, size_type count, const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, count, value);
	}

	template<typename InputIt>
	iterator insert(const_iterator pos, InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, first, last);
	}

	iterator insert(const_iterator pos, std::initializer_list<T> ilist)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(pos, ilist);
	}

	template<typename ...Args>
	iterator emplace(const_iterator pos, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(pos, std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

public: // -- push / pop -- //

	void push_back(const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_back(value);
	}
	void push_back(T &&value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_back(std::move(value));
	}

	template<typename ...Args>
	decltype(auto) emplace_back(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_back(std::forward<Args>(args)...);
	}

	void pop_back()
	{
		std::lock_guard lock(this->mutex);
		wrapped().pop_back();
	}

	void push_front(const T &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_front(value);
	}
	void push_front(T &&value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().push_front(std::move(value));
	}

	template<typename ...Args>
	decltype(auto) emplace_front(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_front(std::forward<Args>(args)...);
	}

	void pop_front()
	{
		std::lock_guard lock(this->mutex);
		wrapped().pop_front();
	}

public: // -- resize -- //

	void resize(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().resize(count);
	}
	void resize(size_type count, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		wrapped().resize(count, value);
	}

public: // -- swap -- //

	void swap(__gc_list &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_list &a, __gc_list &b) { a.swap(b); }
	friend void swap(__gc_list &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_list &b) { b.swap(a); }

public: // -- cmp -- //

	friend bool operator==(const __gc_list &a, const __gc_list &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_list &a, const __gc_list &b) { return a.wrapped() != b.wrapped(); }
	friend bool operator<(const __gc_list &a, const __gc_list &b) { return a.wrapped() < b.wrapped(); }
	friend bool operator<=(const __gc_list &a, const __gc_list &b) { return a.wrapped() <= b.wrapped(); }
	friend bool operator>(const __gc_list &a, const __gc_list &b) { return a.wrapped() > b.wrapped(); }
	friend bool operator>=(const __gc_list &a, const __gc_list &b) { return a.wrapped() >= b.wrapped(); }

public: // -- merge -- //

	void merge(__gc_list &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().merge(other.wrapped());
		}
	}
	void merge(__gc_list &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().merge(std::move(other.wrapped()));
		}
	}

	template<typename Compare>
	void merge(__gc_list &other, Compare comp)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().merge(other.wrapped(), comp);
		}
	}
	template<typename Compare>
	void merge(__gc_list &&other, Compare comp)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().merge(std::move(other.wrapped()), comp);
		}
	}

	// ----------------------------------------------------------

	void merge(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().merge(other);
	}
	void merge(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().merge(std::move(other));
	}

	template<typename Compare>
	void merge(wrapped_t &other, Compare comp)
	{
		std::lock_guard lock(this->mutex);
		wrapped().merge(other, comp);
	}
	template<typename Compare>
	void merge(wrapped_t &&other, Compare comp)
	{
		std::lock_guard lock(this->mutex);
		wrapped().merge(std::move(other), comp);
	}

public: // -- splice -- //

	void splice_after(const_iterator pos, __gc_list &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped());
		}
	}
	void splice_after(const_iterator pos, __gc_list &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()));
		}
	}

	void splice_after(const_iterator pos, __gc_list &other, const_iterator it)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped(), it);
		}
	}
	void splice_after(const_iterator pos, __gc_list &&other, const_iterator it)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()), it);
		}
	}

	void splice_after(const_iterator pos, __gc_list &other, const_iterator first, const_iterator last)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, other.wrapped(), first, last);
		}
	}
	void splice_after(const_iterator pos, __gc_list &&other, const_iterator first, const_iterator last)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().splice_after(pos, std::move(other.wrapped()), first, last);
		}
	}

	// ----------------------------------------------------------

	void splice_after(const_iterator pos, wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, other);
	}
	void splice_after(const_iterator pos, wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, std::move(other));
	}

	void splice_after(const_iterator pos, wrapped_t &other, const_iterator it)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, other, it);
	}
	void splice_after(const_iterator pos, wrapped_t &&other, const_iterator it)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, std::move(other), it);
	}

	void splice_after(const_iterator pos, wrapped_t &other, const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, other, first, last);
	}
	void splice_after(const_iterator pos, wrapped_t &&other, const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().splice_after(pos, std::move(other), first, last);
	}

public: // -- remove -- //

	decltype(auto) remove(const T &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().remove(value);
	}

	template<typename UnaryPredicate>
	decltype(auto) remove_if(UnaryPredicate p)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().remove_if(p);
	}

public: // -- ordering -- //

	void reverse() noexcept
	{
		std::lock_guard lock(this->mutex);
		wrapped().reverse();
	}

	decltype(auto) unique()
	{
		std::lock_guard lock(this->mutex);
		return wrapped().unique();
	}
	template<typename BinaryPredicate>
	decltype(auto) unique(BinaryPredicate p)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().unique(p);
	}

	void sort()
	{
		std::lock_guard lock(this->mutex);
		wrapped().sort();
	}
	template<typename Compare>
	void sort(Compare comp)
	{
		std::lock_guard lock(this->mutex);
		wrapped().sort(comp);
	}
};
template<typename T, typename Allocator, typename Lockable>
struct GC::router<__gc_list<T, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::has_trivial_router<T>::value;

	template<typename F>
	static void route(const __gc_list<T, Allocator, Lockable> &list, F func)
	{
		std::lock_guard lock(list.mutex);
		GC::route(list.wrapped(), func);
	}
};

template<typename Key, typename Compare, typename Allocator, typename Lockable>
class __gc_set
{
private: // -- data -- //

	typedef std::set<Key, Compare, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_set>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	typedef typename wrapped_t::node_type node_type;
	typedef typename wrapped_t::insert_return_type insert_return_type;

public: // -- ctor / dtor -- //

	__gc_set()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_set(const Compare &comp, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(comp, alloc);
	}
	explicit __gc_set(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	template<typename InputIt>
	__gc_set(InputIt first, InputIt last, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, comp, alloc);
	}
	template<typename InputIt>
	__gc_set(InputIt first, InputIt last, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, alloc);
	}

	__gc_set(const __gc_set &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_set(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_set(const __gc_set &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_set(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_set(__gc_set &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_set(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_set(__gc_set &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_set(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_set(std::initializer_list<value_type> init, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, comp, alloc);
	}
	__gc_set(std::initializer_list<value_type> init, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, alloc);
	}

	~__gc_set()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_set &operator=(const __gc_set &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_set &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_set &operator=(__gc_set &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_set &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_set &operator=(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	std::pair<iterator, bool> insert(const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(value);
	}
	std::pair<iterator, bool> insert(value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(value));
	}

	iterator insert(const_iterator hint, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, value);
	}
	iterator insert(const_iterator hint, value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(value));
	}

	template<typename InputIt>
	void insert(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(first, last);
	}

	void insert(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(ilist);
	}

	insert_return_type insert(node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(nh));
	}
	iterator insert(const_iterator hint, node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(nh));
	}

	template<typename ...Args>
	std::pair<iterator, bool> emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator emplace_hint(const_iterator hint, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

	size_type erase(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(key);
	}

public: // -- swap -- //

	void swap(__gc_set &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_set &a, __gc_set &b) { a.swap(b); }
	friend void swap(__gc_set &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_set &b) { b.swap(a); }

public: // -- extract -- //

	node_type extract(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(pos);
	}
	node_type extract(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(key);
	}

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

	friend bool operator==(const __gc_set &a, const __gc_set &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_set &a, const __gc_set &b) { return a.wrapped() != b.wrapped(); }
	friend bool operator<(const __gc_set &a, const __gc_set &b) { return a.wrapped() < b.wrapped(); }
	friend bool operator<=(const __gc_set &a, const __gc_set &b) { return a.wrapped() <= b.wrapped(); }
	friend bool operator>(const __gc_set &a, const __gc_set &b) { return a.wrapped() > b.wrapped(); }
	friend bool operator>=(const __gc_set &a, const __gc_set &b) { return a.wrapped() >= b.wrapped(); }
};
template<typename Key, typename Compare, typename Allocator, typename Lockable>
struct GC::router<__gc_set<Key, Compare, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::has_trivial_router<Key>::value;

	template<typename F>
	static void route(const __gc_set<Key, Compare, Allocator, Lockable> &set, F func)
	{
		std::lock_guard lock(set.mutex);
		GC::route(set.wrapped(), func);
	}
};

template<typename Key, typename Compare, typename Allocator, typename Lockable>
class __gc_multiset
{
private: // -- data -- //

	typedef std::multiset<Key, Compare, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_multiset>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	typedef typename wrapped_t::node_type node_type;

public: // -- ctor / dtor -- //

	__gc_multiset()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_multiset(const Compare &comp, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(comp, alloc);
	}
	explicit __gc_multiset(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	template<typename InputIt>
	__gc_multiset(InputIt first, InputIt last, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, comp, alloc);
	}
	template<typename InputIt>
	__gc_multiset(InputIt first, InputIt last, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, alloc);
	}

	__gc_multiset(const __gc_multiset &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_multiset(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_multiset(const __gc_multiset &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_multiset(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_multiset(__gc_multiset &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_multiset(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_multiset(__gc_multiset &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_multiset(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_multiset(std::initializer_list<value_type> init, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, comp, alloc);
	}
	__gc_multiset(std::initializer_list<value_type> init, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, alloc);
	}

	~__gc_multiset()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_multiset &operator=(const __gc_multiset &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_multiset &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_multiset &operator=(__gc_multiset &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_multiset &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_multiset &operator=(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	iterator insert(const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(value);
	}
	iterator insert(value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(value));
	}

	iterator insert(const_iterator hint, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, value);
	}
	iterator insert(const_iterator hint, value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(value));
	}

	template<typename InputIt>
	void insert(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(first, last);
	}

	void insert(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(ilist);
	}

	iterator insert(node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(nh));
	}
	iterator insert(const_iterator hint, node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(nh));
	}

	template<typename ...Args>
	std::pair<iterator, bool> emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator emplace_hint(const_iterator hint, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

	size_type erase(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(key);
	}

public: // -- swap -- //

	void swap(__gc_multiset &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_multiset &a, __gc_multiset &b) { a.swap(b); }
	friend void swap(__gc_multiset &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_multiset &b) { b.swap(a); }

public: // -- extract -- //

	node_type extract(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(pos);
	}
	node_type extract(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(key);
	}

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

	friend bool operator==(const __gc_multiset &a, const __gc_multiset &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_multiset &a, const __gc_multiset &b) { return a.wrapped() != b.wrapped(); }
	friend bool operator<(const __gc_multiset &a, const __gc_multiset &b) { return a.wrapped() < b.wrapped(); }
	friend bool operator<=(const __gc_multiset &a, const __gc_multiset &b) { return a.wrapped() <= b.wrapped(); }
	friend bool operator>(const __gc_multiset &a, const __gc_multiset &b) { return a.wrapped() > b.wrapped(); }
	friend bool operator>=(const __gc_multiset &a, const __gc_multiset &b) { return a.wrapped() >= b.wrapped(); }
};
template<typename Key, typename Compare, typename Allocator, typename Lockable>
struct GC::router<__gc_multiset<Key, Compare, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::has_trivial_router<Key>::value;

	template<typename F>
	static void route(const __gc_multiset<Key, Compare, Allocator, Lockable> &set, F func)
	{
		std::lock_guard lock(set.mutex);
		GC::route(set.wrapped(), func);
	}
};

template<typename Key, typename T, typename Compare, typename Allocator, typename Lockable>
class __gc_map
{
private: // -- data -- //

	typedef std::map<Key, T, Compare, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_map>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	typedef typename wrapped_t::node_type node_type;
	typedef typename wrapped_t::insert_return_type insert_return_type;

	typedef typename wrapped_t::value_compare value_compare; // alias __gc_map's nested value_compare class

public: // -- ctor / dtor -- //

	__gc_map()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_map(const Compare &comp, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(comp, alloc);
	}
	explicit __gc_map(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	template<typename InputIt>
	__gc_map(InputIt first, InputIt last, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, comp, alloc);
	}
	template<typename InputIt>
	__gc_map(InputIt first, InputIt last, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, alloc);
	}

	__gc_map(const __gc_map &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_map(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_map(const __gc_map &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_map(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_map(__gc_map &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_map(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_map(__gc_map &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_map(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_map(std::initializer_list<value_type> init, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, comp, alloc);
	}
	__gc_map(std::initializer_list<value_type> init, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, alloc);
	}

	~__gc_map()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_map &operator=(const __gc_map &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_map &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_map &operator=(__gc_map &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_map &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_map &operator=(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex); // operator[] performs an insertion if key doesn't exist
		return wrapped()[key];
	}
	T &operator[](Key &&key)
	{
		std::lock_guard lock(this->mutex); // operator[] performs an insertion if key doesn't exist
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	std::pair<iterator, bool> insert(const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(value);
	}
	template<typename P>
	std::pair<iterator, bool> insert(P &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::forward<P>(value));
	}

	iterator insert(const_iterator hint, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, value);
	}

	template<typename P>
	iterator insert(const_iterator hint, P &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::forward<P>(value));
	}

	template<typename InputIt>
	void insert(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(first, last);
	}

	void insert(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(ilist);
	}

	std::pair<iterator, bool> insert(value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(value));
	}
	iterator insert(const_iterator hint, value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(value));
	}

	insert_return_type insert(node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(nh));
	}
	iterator insert(const_iterator hint, node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(nh));
	}

	template<typename M>
	std::pair<iterator, bool> insert_or_assign(const key_type &k, M &&obj)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_or_assign(k, std::forward<M>(obj));
	}
	template<typename M>
	std::pair<iterator, bool> insert_or_assign(key_type &&k, M &&obj)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_or_assign(std::move(k), std::forward<M>(obj));
	}

	template<typename M>
	std::pair<iterator, bool> insert_or_assign(const_iterator hint, const key_type &k, M &&obj)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_or_assign(hint, k, std::forward<M>(obj));
	}
	template<typename M>
	std::pair<iterator, bool> insert_or_assign(const_iterator hint, key_type &&k, M &&obj)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_or_assign(hint, std::move(k), std::forward<M>(obj));
	}

	template<typename ...Args>
	std::pair<iterator, bool> try_emplace(const key_type &k, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().try_emplace(k, std::forward<Args>(args)...);
	}
	template<typename ...Args>
	std::pair<iterator, bool> try_emplace(key_type &&k, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().try_emplace(std::move(k), std::forward<Args>(args)...);
	}

	template<typename ...Args>
	iterator try_emplace(const_iterator hint, const key_type &k, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().try_emplace(hint, k, std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator try_emplace(const_iterator hint, key_type &&k, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().try_emplace(hint, std::move(k), std::forward<Args>(args)...);
	}

	template<typename ...Args>
	std::pair<iterator, bool> emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator emplace_hint(const_iterator hint, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

	size_type erase(const key_type &k)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(k);
	}

public: // -- swap -- //

	void swap(__gc_map &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_map &a, __gc_map &b) { a.swap(b); }
	friend void swap(__gc_map &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_map &b) { b.swap(a); }

public: // -- extract -- //

	node_type extract(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(pos);
	}
	node_type extract(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(key);
	}

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

	friend bool operator==(const __gc_map &a, const __gc_map &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_map &a, const __gc_map &b) { return a.wrapped() != b.wrapped(); }
	friend bool operator<(const __gc_map &a, const __gc_map &b) { return a.wrapped() < b.wrapped(); }
	friend bool operator<=(const __gc_map &a, const __gc_map &b) { return a.wrapped() <= b.wrapped(); }
	friend bool operator>(const __gc_map &a, const __gc_map &b) { return a.wrapped() > b.wrapped(); }
	friend bool operator>=(const __gc_map &a, const __gc_map &b) { return a.wrapped() >= b.wrapped(); }
};
template<typename Key, typename T, typename Compare, typename Allocator, typename Lockable>
struct GC::router<__gc_map<Key, T, Compare, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::all_have_trivial_routers<Key, T>::value;

	template<typename F>
	static void route(const __gc_map<Key, T, Compare, Allocator, Lockable> &map, F func)
	{
		std::lock_guard lock(map.mutex);
		GC::route(map.wrapped(), func);
	}
};

template<typename Key, typename T, typename Compare, typename Allocator, typename Lockable>
class __gc_multimap
{
private: // -- data -- //

	typedef std::multimap<Key, T, Compare, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_multimap>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	typedef typename wrapped_t::node_type node_type;

	typedef typename wrapped_t::value_compare value_compare; // alias __gc_multimap's nested value_compare class

public: // -- ctor / dtor -- //

	__gc_multimap()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_multimap(const Compare &comp, const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(comp, alloc);
	}
	explicit __gc_multimap(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	template<typename InputIt>
	__gc_multimap(InputIt first, InputIt last, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, comp, alloc);
	}
	template<typename InputIt>
	__gc_multimap(InputIt first, InputIt last, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, alloc);
	}

	__gc_multimap(const __gc_multimap &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_multimap(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_multimap(const __gc_multimap &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_multimap(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_multimap(__gc_multimap &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_multimap(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_multimap(__gc_multimap &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_multimap(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_multimap(std::initializer_list<value_type> init, const Compare &comp = Compare(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, comp, alloc);
	}
	__gc_multimap(std::initializer_list<value_type> init, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, alloc);
	}

	~__gc_multimap()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_multimap &operator=(const __gc_multimap &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_multimap &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_multimap &operator=(__gc_multimap &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_multimap &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_multimap &operator=(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	iterator insert(const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(value);
	}
	template<typename P>
	iterator insert(P &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::forward<P>(value));
	}

	iterator insert(const_iterator hint, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, value);
	}

	template<typename P>
	iterator insert(const_iterator hint, P &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::forward<P>(value));
	}

	template<typename InputIt>
	void insert(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(first, last);
	}

	void insert(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(ilist);
	}

	iterator insert(value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(value));
	}
	iterator insert(const_iterator hint, value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(value));
	}

	iterator insert(node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(nh));
	}
	iterator insert(const_iterator hint, node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(nh));
	}

	template<typename ...Args>
	iterator emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator emplace_hint(const_iterator hint, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

	size_type erase(const key_type &k)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(k);
	}

public: // -- swap -- //

	void swap(__gc_multimap &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_multimap &a, __gc_multimap &b) { a.swap(b); }
	friend void swap(__gc_multimap &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_multimap &b) { b.swap(a); }

public: // -- extract -- //

	node_type extract(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(pos);
	}
	node_type extract(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(key);
	}

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

	friend bool operator==(const __gc_multimap &a, const __gc_multimap &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_multimap &a, const __gc_multimap &b) { return a.wrapped() != b.wrapped(); }
	friend bool operator<(const __gc_multimap &a, const __gc_multimap &b) { return a.wrapped() < b.wrapped(); }
	friend bool operator<=(const __gc_multimap &a, const __gc_multimap &b) { return a.wrapped() <= b.wrapped(); }
	friend bool operator>(const __gc_multimap &a, const __gc_multimap &b) { return a.wrapped() > b.wrapped(); }
	friend bool operator>=(const __gc_multimap &a, const __gc_multimap &b) { return a.wrapped() >= b.wrapped(); }
};
template<typename Key, typename T, typename Compare, typename Allocator, typename Lockable>
struct GC::router<__gc_multimap<Key, T, Compare, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::all_have_trivial_routers<Key, T>::value;

	template<typename F>
	static void route(const __gc_multimap<Key, T, Compare, Allocator, Lockable> &map, F func)
	{
		std::lock_guard lock(map.mutex);
		GC::route(map.wrapped(), func);
	}
};

template<typename Key, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
class __gc_unordered_set
{
private: // -- data -- //

	typedef std::unordered_set<Key, Hash, KeyEqual, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_unordered_set>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	typedef typename wrapped_t::node_type node_type;
	typedef typename wrapped_t::insert_return_type insert_return_type;

public: // -- ctor / dtor -- //

	__gc_unordered_set()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_unordered_set(size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(bucket_count, hash, equal, alloc);
	}

	__gc_unordered_set(size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(bucket_count, alloc);
	}
	__gc_unordered_set(size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(bucket_count, hash, alloc);
	}

	explicit __gc_unordered_set(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	template<typename InputIt>
	__gc_unordered_set(InputIt first, InputIt last)
	{
		new (buffer) wrapped_t(first, last);
	}
	template<typename InputIt>
	__gc_unordered_set(InputIt first, InputIt last, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, bucket_count, hash, equal, alloc);
	}

	template<typename InputIt>
	__gc_unordered_set(InputIt first, InputIt last, size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, bucket_count, alloc);
	}

	template<typename InputIt>
	__gc_unordered_set(InputIt first, InputIt last, size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, bucket_count, hash, alloc);
	}

	__gc_unordered_set(const __gc_unordered_set &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_unordered_set(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_unordered_set(const __gc_unordered_set &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_unordered_set(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_unordered_set(__gc_unordered_set &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_unordered_set(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_unordered_set(__gc_unordered_set &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_unordered_set(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_unordered_set(std::initializer_list<value_type> init)
	{
		new (buffer) wrapped_t(init);
	}
	__gc_unordered_set(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, bucket_count, hash, equal, alloc);
	}

	__gc_unordered_set(std::initializer_list<value_type> init, size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, bucket_count, alloc);
	}
	__gc_unordered_set(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, bucket_count, hash, alloc);
	}

	~__gc_unordered_set()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_unordered_set &operator=(const __gc_unordered_set &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_unordered_set &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_unordered_set &operator=(__gc_unordered_set &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_unordered_set &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_unordered_set &operator=(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	std::pair<iterator, bool> insert(const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(value);
	}
	std::pair<iterator, bool> insert(value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(value));
	}

	iterator insert(const_iterator hint, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, value);
	}
	iterator insert(const_iterator hint, value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(value));
	}

	template<typename InputIt>
	void insert(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(first, last);
	}

	void insert(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(ilist);
	}

	insert_return_type insert(node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(nh));
	}
	iterator insert(const_iterator hint, node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(nh));
	}

	template<typename ...Args>
	std::pair<iterator, bool> emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator emplace_hint(const_iterator hint, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

	size_type erase(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(key);
	}

public: // -- swap -- //

	void swap(__gc_unordered_set &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_unordered_set &a, __gc_unordered_set &b) { a.swap(b); }
	friend void swap(__gc_unordered_set &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_unordered_set &b) { b.swap(a); }

public: // -- extract -- //

	node_type extract(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(pos);
	}
	node_type extract(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(key);
	}

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
		std::lock_guard lock(this->mutex);
		wrapped().max_load_factor(ml);
	}

	void rehash(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().rehash(count);
	}

	void reserve(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().reserve(count);
	}

public: // -- observers -- //

	hasher hash_function() const { return wrapped().hash_function(); }
	key_equal key_eq() const { return wrapped().key_eq(); }

public: // -- cmp -- //

	friend bool operator==(const __gc_unordered_set &a, const __gc_unordered_set &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_unordered_set &a, const __gc_unordered_set &b) { return a.wrapped() != b.wrapped(); }
};
template<typename Key, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
struct GC::router<__gc_unordered_set<Key, Hash, KeyEqual, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::has_trivial_router<Key>::value;

	template<typename F>
	static void route(const __gc_unordered_set<Key, Hash, KeyEqual, Allocator, Lockable> &set, F func)
	{
		std::lock_guard lock(set.mutex);
		GC::route(set.wrapped(), func);
	}
};

template<typename Key, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
class __gc_unordered_multiset
{
private: // -- data -- //

	typedef std::unordered_multiset<Key, Hash, KeyEqual, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_unordered_multiset>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	typedef typename wrapped_t::node_type node_type;

public: // -- ctor / dtor -- //

	__gc_unordered_multiset()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_unordered_multiset(size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(bucket_count, hash, equal, alloc);
	}

	__gc_unordered_multiset(size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(bucket_count, alloc);
	}
	__gc_unordered_multiset(size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(bucket_count, hash, alloc);
	}

	explicit __gc_unordered_multiset(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	template<typename InputIt>
	__gc_unordered_multiset(InputIt first, InputIt last)
	{
		new (buffer) wrapped_t(first, last);
	}
	template<typename InputIt>
	__gc_unordered_multiset(InputIt first, InputIt last, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, bucket_count, hash, equal, alloc);
	}

	template<typename InputIt>
	__gc_unordered_multiset(InputIt first, InputIt last, size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, bucket_count, alloc);
	}

	template<typename InputIt>
	__gc_unordered_multiset(InputIt first, InputIt last, size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, bucket_count, hash, alloc);
	}

	__gc_unordered_multiset(const __gc_unordered_multiset &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_unordered_multiset(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_unordered_multiset(const __gc_unordered_multiset &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_unordered_multiset(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_unordered_multiset(__gc_unordered_multiset &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_unordered_multiset(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_unordered_multiset(__gc_unordered_multiset &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_unordered_multiset(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_unordered_multiset(std::initializer_list<value_type> init)
	{
		new (buffer) wrapped_t(init);
	}
	__gc_unordered_multiset(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, bucket_count, hash, equal, alloc);
	}

	__gc_unordered_multiset(std::initializer_list<value_type> init, size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, bucket_count, alloc);
	}
	__gc_unordered_multiset(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, bucket_count, hash, alloc);
	}

	~__gc_unordered_multiset()
	{
		wrapped().~wrapped_t();
	}

public: // -- asgn -- //

	__gc_unordered_multiset &operator=(const __gc_unordered_multiset &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_unordered_multiset &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_unordered_multiset &operator=(__gc_unordered_multiset &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_unordered_multiset &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_unordered_multiset &operator=(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	iterator insert(const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(value);
	}
	iterator insert(value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(value));
	}

	iterator insert(const_iterator hint, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, value);
	}
	iterator insert(const_iterator hint, value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(value));
	}

	template<typename InputIt>
	void insert(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(first, last);
	}

	void insert(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(ilist);
	}

	iterator insert(node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(nh));
	}
	iterator insert(const_iterator hint, node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(nh));
	}

	template<typename ...Args>
	iterator emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator emplace_hint(const_iterator hint, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

	size_type erase(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(key);
	}

public: // -- swap -- //

	void swap(__gc_unordered_multiset &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_unordered_multiset &a, __gc_unordered_multiset &b) { a.swap(b); }
	friend void swap(__gc_unordered_multiset &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_unordered_multiset &b) { b.swap(a); }

public: // -- extract -- //

	node_type extract(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(pos);
	}
	node_type extract(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(key);
	}

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
		std::lock_guard lock(this->mutex);
		wrapped().max_load_factor(ml);
	}

	void rehash(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().rehash(count);
	}

	void reserve(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().reserve(count);
	}

public: // -- observers -- //

	hasher hash_function() const { return wrapped().hash_function(); }
	key_equal key_eq() const { return wrapped().key_eq(); }

public: // -- cmp -- //

	friend bool operator==(const __gc_unordered_multiset &a, const __gc_unordered_multiset &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_unordered_multiset &a, const __gc_unordered_multiset &b) { return a.wrapped() != b.wrapped(); }
};
template<typename Key, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
struct GC::router<__gc_unordered_multiset<Key, Hash, KeyEqual, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::has_trivial_router<Key>::value;

	template<typename F>
	static void route(const __gc_unordered_multiset<Key, Hash, KeyEqual, Allocator, Lockable> &set, F func)
	{
		std::lock_guard lock(set.mutex);
		GC::route(set.wrapped(), func);
	}
};

template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
class __gc_unordered_map
{
private: // -- data -- //

	typedef std::unordered_map<Key, T, Hash, KeyEqual, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_unordered_map>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	typedef typename wrapped_t::node_type node_type;
	typedef typename wrapped_t::insert_return_type insert_return_type;

public: // -- ctor / dtor -- //

	__gc_unordered_map()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_unordered_map(size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(bucket_count, hash, equal, alloc);
	}

	__gc_unordered_map(size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(bucket_count, alloc);
	}
	__gc_unordered_map(size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(bucket_count, hash, alloc);
	}

	explicit __gc_unordered_map(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	template<typename InputIt>
	__gc_unordered_map(InputIt first, InputIt last)
	{
		new (buffer) wrapped_t(first, last);
	}
	template<typename InputIt>
	__gc_unordered_map(InputIt first, InputIt last, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, bucket_count, hash, equal, alloc);
	}

	template<typename InputIt>
	__gc_unordered_map(InputIt first, InputIt last, size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, bucket_count, alloc);
	}

	template<typename InputIt>
	__gc_unordered_map(InputIt first, InputIt last, size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, bucket_count, hash, alloc);
	}

	__gc_unordered_map(const __gc_unordered_map &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_unordered_map(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_unordered_map(const __gc_unordered_map &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_unordered_map(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_unordered_map(__gc_unordered_map &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_unordered_map(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_unordered_map(__gc_unordered_map &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_unordered_map(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_unordered_map(std::initializer_list<value_type> init)
	{
		new (buffer) wrapped_t(init);
	}
	__gc_unordered_map(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, bucket_count, hash, equal, alloc);
	}

	__gc_unordered_map(std::initializer_list<value_type> init, size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, bucket_count, alloc);
	}
	__gc_unordered_map(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, bucket_count, hash, alloc);
	}

	~__gc_unordered_map()
	{
		wrapped().~wrapped_t();
	}

public: // -- assign -- //

	__gc_unordered_map &operator=(const __gc_unordered_map &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_unordered_map &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_unordered_map &operator=(__gc_unordered_map &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_unordered_map &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_unordered_map &operator=(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	std::pair<iterator, bool> insert(const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(value);
	}

	template<typename P>
	std::pair<iterator, bool> insert(P &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::forward<P>(value));
	}

	iterator insert(const_iterator hint, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, value);
	}

	template<typename P>
	iterator insert(const_iterator hint, P &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::forward<P>(value));
	}

	template<typename InputIt>
	void insert(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(first, last);
	}

	void insert(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(ilist);
	}

	std::pair<iterator, bool> insert(value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(value));
	}
	iterator insert(const_iterator hint, value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(value));
	}

	insert_return_type insert(node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(nh));
	}
	iterator insert(const_iterator hint, node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(nh));
	}

	template<typename M>
	std::pair<iterator, bool> insert_or_assign(const key_type &k, M &&obj)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_or_assign(k, std::forward<M>(obj));
	}
	template<typename M>
	std::pair<iterator, bool> insert_or_assign(key_type &&k, M &&obj)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_or_assign(std::move(k), std::forward<M>(obj));
	}

	template<typename M>
	iterator insert_or_assign(const_iterator hint, const key_type &k, M &&obj)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_or_assign(hint, k, std::forward<M>(obj));
	}
	template<typename M>
	iterator insert_or_assign(const_iterator hint, key_type &&k, M &&obj)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert_or_assign(hint, std::move(k), std::forward<M>(obj));
	}

	template<typename ...Args>
	std::pair<iterator, bool> try_emplace(const key_type &k, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().try_emplace(k, std::forward<Args>(args)...);
	}
	template<typename ...Args>
	std::pair<iterator, bool> try_emplace(key_type &&k, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().try_emplace(std::move(k), std::forward<Args>(args)...);
	}

	template<typename ...Args>
	iterator try_emplace(const_iterator hint, const key_type &k, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().try_emplace(hint, k, std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator try_emplace(const_iterator hint, key_type &&k, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().try_emplace(hint, std::move(k), std::forward<Args>(args)...);
	}

	template<typename ...Args>
	std::pair<iterator, bool> emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator emplace_hint(const_iterator hint, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

	size_type erase(const key_type &k)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(k);
	}

public: // -- swap -- //

	void swap(__gc_unordered_map &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_unordered_map &a, __gc_unordered_map &b) { a.swap(b); }
	friend void swap(__gc_unordered_map &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_unordered_map &b) { b.swap(a); }

public: // -- extract -- //

	node_type extract(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(pos);
	}
	node_type extract(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(key);
	}

	// !! ADD MERGE FUNCTIONS (C++17)

public: // -- element access -- //

	T &at(const Key &key) { return wrapped().at(key); }
	const T &at(const Key &key) const { return wrapped().at(key); }

	T &operator[](const Key &key)
	{
		std::lock_guard lock(this->mutex); // operator[] performs an insertion if key doesn't exist
		return wrapped()[key];
	}
	T &operator[](Key &&key)
	{
		std::lock_guard lock(this->mutex); // operator[] performs an insertion if key doesn't exist
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
		std::lock_guard lock(this->mutex);
		wrapped().max_load_factor(ml);
	}

	void rehash(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().rehash(count);
	}

	void reserve(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().reserve(count);
	}

public: // -- observers -- //

	hasher hash_function() const { return wrapped().hash_function(); }
	key_equal key_eq() const { return wrapped().key_eq(); }

public: // -- cmp -- //

	friend bool operator==(const __gc_unordered_map &a, const __gc_unordered_map &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_unordered_map &a, const __gc_unordered_map &b) { return a.wrapped() != b.wrapped(); }
};
template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
struct GC::router<__gc_unordered_map<Key, T, Hash, KeyEqual, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::all_have_trivial_routers<Key, T>::value;

	template<typename F>
	static void route(const __gc_unordered_map<Key, T, Hash, KeyEqual, Allocator, Lockable> &map, F func)
	{
		std::lock_guard lock(map.mutex);
		GC::route(map.wrapped(), func);
	}
};

template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
class __gc_unordered_multimap
{
private: // -- data -- //

	typedef std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_unordered_multimap>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

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

	typedef typename wrapped_t::node_type node_type;

public: // -- ctor / dtor -- //

	__gc_unordered_multimap()
	{
		new (buffer) wrapped_t();
	}
	explicit __gc_unordered_multimap(size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(bucket_count, hash, equal, alloc);
	}

	__gc_unordered_multimap(size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(bucket_count, alloc);
	}
	__gc_unordered_multimap(size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(bucket_count, hash, alloc);
	}

	explicit __gc_unordered_multimap(const Allocator &alloc)
	{
		new (buffer) wrapped_t(alloc);
	}

	template<typename InputIt>
	__gc_unordered_multimap(InputIt first, InputIt last)
	{
		new (buffer) wrapped_t(first, last);
	}
	template<typename InputIt>
	__gc_unordered_multimap(InputIt first, InputIt last, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(first, last, bucket_count, hash, equal, alloc);
	}

	template<typename InputIt>
	__gc_unordered_multimap(InputIt first, InputIt last, size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, bucket_count, alloc);
	}

	template<typename InputIt>
	__gc_unordered_multimap(InputIt first, InputIt last, size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(first, last, bucket_count, hash, alloc);
	}

	__gc_unordered_multimap(const __gc_unordered_multimap &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	__gc_unordered_multimap(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	__gc_unordered_multimap(const __gc_unordered_multimap &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other.wrapped(), alloc);
	}
	__gc_unordered_multimap(const wrapped_t &other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(other, alloc);
	}

	__gc_unordered_multimap(__gc_unordered_multimap &&other)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	__gc_unordered_multimap(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	__gc_unordered_multimap(__gc_unordered_multimap &&other, const Allocator &alloc)
	{
		std::lock_guard lock(other.mutex);
		new (buffer) wrapped_t(std::move(other.wrapped()), alloc);
	}
	__gc_unordered_multimap(wrapped_t &&other, const Allocator &alloc)
	{
		new (buffer) wrapped_t(std::move(other), alloc);
	}

	__gc_unordered_multimap(std::initializer_list<value_type> init)
	{
		new (buffer) wrapped_t(init);
	}
	__gc_unordered_multimap(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash = Hash(), const key_equal &equal = key_equal(), const Allocator &alloc = Allocator())
	{
		new (buffer) wrapped_t(init, bucket_count, hash, equal, alloc);
	}

	__gc_unordered_multimap(std::initializer_list<value_type> init, size_type bucket_count, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, bucket_count, alloc);
	}
	__gc_unordered_multimap(std::initializer_list<value_type> init, size_type bucket_count, const Hash &hash, const Allocator &alloc)
	{
		new (buffer) wrapped_t(init, bucket_count, hash, alloc);
	}

	~__gc_unordered_multimap()
	{
		wrapped().~wrapped_t();
	}

public: // -- assign -- //

	__gc_unordered_multimap &operator=(const __gc_unordered_multimap &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_unordered_multimap &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_unordered_multimap &operator=(__gc_unordered_multimap &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_unordered_multimap &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	__gc_unordered_multimap &operator=(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
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
		std::lock_guard lock(this->mutex);
		wrapped().clear();
	}

public: // -- insert / erase -- //

	iterator insert(const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(value);
	}

	template<typename P>
	iterator insert(P &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::forward<P>(value));
	}

	iterator insert(const_iterator hint, const value_type &value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, value);
	}

	template<typename P>
	iterator insert(const_iterator hint, P &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::forward<P>(value));
	}

	template<typename InputIt>
	void insert(InputIt first, InputIt last)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(first, last);
	}

	void insert(std::initializer_list<value_type> ilist)
	{
		std::lock_guard lock(this->mutex);
		wrapped().insert(ilist);
	}

	iterator insert(value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(value));
	}
	iterator insert(const_iterator hint, value_type &&value)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(value));
	}

	iterator insert(node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(std::move(nh));
	}
	iterator insert(const_iterator hint, node_type &&nh)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().insert(hint, std::move(nh));
	}

	template<typename ...Args>
	iterator emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}
	template<typename ...Args>
	iterator emplace_hint(const_iterator hint, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace_hint(hint, std::forward<Args>(args)...);
	}

	iterator erase(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(pos);
	}
	iterator erase(const_iterator first, const_iterator last)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(first, last);
	}

	size_type erase(const key_type &k)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().erase(k);
	}

public: // -- swap -- //

	void swap(__gc_unordered_multimap &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_unordered_multimap &a, __gc_unordered_multimap &b) { a.swap(b); }
	friend void swap(__gc_unordered_multimap &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_unordered_multimap &b) { b.swap(a); }

public: // -- extract -- //

	node_type extract(const_iterator pos)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(pos);
	}
	node_type extract(const key_type &key)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().extract(key);
	}

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
		std::lock_guard lock(this->mutex);
		wrapped().max_load_factor(ml);
	}

	void rehash(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().rehash(count);
	}

	void reserve(size_type count)
	{
		std::lock_guard lock(this->mutex);
		wrapped().reserve(count);
	}

public: // -- observers -- //

	hasher hash_function() const { return wrapped().hash_function(); }
	key_equal key_eq() const { return wrapped().key_eq(); }

public: // -- cmp -- //

	friend bool operator==(const __gc_unordered_multimap &a, const __gc_unordered_multimap &b) { return a.wrapped() == b.wrapped(); }
	friend bool operator!=(const __gc_unordered_multimap &a, const __gc_unordered_multimap &b) { return a.wrapped() != b.wrapped(); }
};
template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
struct GC::router<__gc_unordered_multimap<Key, T, Hash, KeyEqual, Allocator, Lockable>>
{
	// a container's router is trivial if its contents are trivial
	static constexpr bool is_trivial = GC::all_have_trivial_routers<Key, T>::value;

	template<typename F>
	static void route(const __gc_unordered_multimap<Key, T, Hash, KeyEqual, Allocator, Lockable> &map, F func)
	{
		std::lock_guard lock(map.mutex);
		GC::route(map.wrapped(), func);
	}
};

// -------------------------------------------------------------------------

// checks if T is a valid type for value forwarding ctor/asgn for __gc_variant type objects.
// the type checked should already be passed through std::decay.
template<typename T>
struct __gc_variant_valid_forward_type : std::true_type {};

template<typename Lockable, typename ...Types>
struct __gc_variant_valid_forward_type<__gc_variant<Lockable, Types...>> : std::false_type {};
template<typename ...Types>
struct __gc_variant_valid_forward_type<std::variant<Types...>> : std::false_type {};

template<typename T>
struct __gc_variant_valid_forward_type<std::in_place_type_t<T>> : std::false_type {};
template<std::size_t I>
struct __gc_variant_valid_forward_type<std::in_place_index_t<I>> : std::false_type {};

template<typename Lockable, typename ...Types>
class __gc_variant
{
private: // -- data -- //

	typedef std::variant<Types...> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_variant>;
	
	friend struct std::hash<__gc_variant>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

public: // -- ctor / dtor -- //
	
	constexpr __gc_variant()
	{
		new (buffer) wrapped_t();
	}

	constexpr __gc_variant(const __gc_variant &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	constexpr __gc_variant(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	constexpr __gc_variant(__gc_variant &&other)
	{
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	constexpr __gc_variant(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	template<typename T, std::enable_if_t<__gc_variant_valid_forward_type<std::decay_t<T>>::value, int> = 0>
	constexpr __gc_variant(T &&t)
	{
		new (buffer) wrapped_t(std::forward<T>(t));
	}

	template<typename T, typename ...Args>
	constexpr explicit __gc_variant(std::in_place_type_t<T>, Args &&...args)
	{
		new (buffer) wrapped_t(std::in_place_type<T>, std::forward<Args>(args)...);
	}
	template<typename T, typename U, typename ...Args>
	constexpr explicit __gc_variant(std::in_place_type_t<T>, std::initializer_list<U> il, Args &&...args)
	{
		new (buffer) wrapped_t(std::in_place_type<T>, il, std::forward<Args>(args)...);
	}

	template<std::size_t I, typename ...Args>
	constexpr explicit __gc_variant(std::in_place_index_t<I>, Args &&...args)
	{
		new (buffer) wrapped_t(std::in_place_index<I>, std::forward<Args>(args)...);
	}
	template<std::size_t I, typename U, typename ...Args>
	constexpr explicit __gc_variant(std::in_place_index_t<I>, std::initializer_list<U> il, Args &&...args)
	{
		new (buffer) wrapped_t(std::in_place_index<I>, il, std::forward<Args>(args)...);
	}

	~__gc_variant()
	{
		wrapped().~wrapped_t();
	}

public: // -- assign -- //

	__gc_variant &operator=(const __gc_variant &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	__gc_variant &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	__gc_variant &operator=(__gc_variant &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	__gc_variant &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	template<typename T, std::enable_if_t<!std::is_same<std::decay_t<T>, __gc_variant>::value, int> = 0>
	__gc_variant &operator=(T &&t)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::forward<T>(t);
		return *this;
	}

public: // -- observers -- //

	constexpr std::size_t index() const noexcept { return wrapped().index(); }
	constexpr bool valueless_by_exception() const noexcept { return wrapped().valueless_by_exception(); }

public: // -- modifiers -- //

	template<typename T, typename ...Args>
	decltype(auto) emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().template emplace<T>(std::forward<Args>(args)...);
	}
	template<typename T, typename U, typename ...Args>
	decltype(auto) emplace(std::initializer_list<U> il, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().template emplace<T>(il, std::forward<Args>(args)...);
	}

	template<std::size_t I, typename ...Args>
	decltype(auto) emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().template emplace<I>(std::forward<Args>(args)...);
	}
	template<std::size_t I, typename U, typename ...Args>
	decltype(auto) emplace(std::initializer_list<U> il, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().template emplace<I>(il, std::forward<Args>(args)...);
	}

	void swap(__gc_variant &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_variant &a, __gc_variant &b) { a.swap(b); }
	friend void swap(__gc_variant &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_variant &b) { b.swap(a); }

public: // -- helpers -- //

	// equivalent to std::get<I> for the wrapped object.
	// safe to use, but because std::variant doesn't have this i suggest you use GC::get<I> instead for compatibility.
	template<std::size_t I>
	constexpr decltype(auto) get() & { return std::get<I>(wrapped()); }
	template<std::size_t I>
	constexpr decltype(auto) get() const& { return std::get<I>(wrapped()); }
	template<std::size_t I>
	constexpr decltype(auto) get() && { return std::get<I>(std::move(wrapped())); }

	// equivalent to std::get<T> for the wrapped object.
	// safe to use, but because std::variant doesn't have this i suggest you use GC::get<T> instead for compatibility.
	template<typename T>
	constexpr decltype(auto) get() & { return std::get<T>(wrapped()); }
	template<typename T>
	constexpr decltype(auto) get() const& { return std::get<T>(wrapped()); }
	template<typename T>
	constexpr decltype(auto) get() && { return std::get<T>(std::move(wrapped())); }

	// equivalent to std::get_if<I> for the wrapped object (except that this can't be null).
	// safe to use, but because std::variant doesn't have this i suggest you use GC::get_if<I> instead for compatibility.
	template<std::size_t I>
	constexpr decltype(auto) get_if() & { return std::get_if<I>(std::addressof(wrapped())); }
	template<std::size_t I>
	constexpr decltype(auto) get_if() const& { return std::get_if<I>(std::addressof(wrapped())); }

	// equivalent to std::get_if<T> for the wrapped object (except that this can't be null).
	// safe to use, but because std::variant doesn't have this i suggest you use GC::get_if<T> instead for compatibility.
	template<typename T>
	constexpr decltype(auto) get_if() & { return std::get_if<T>(std::addressof(wrapped())); }
	template<typename T>
	constexpr decltype(auto) get_if() const& { return std::get_if<T>(std::addressof(wrapped())); }

	// equivalent to std::holds_alternative<T> for the wrapped object
	template<typename T>
	constexpr bool holds_alternative() const noexcept { return std::holds_alternative<T>(wrapped()); }

public: // -- comparison -- //

	friend constexpr bool operator==(const __gc_variant &a, const __gc_variant &b) { return a.wrapped() == b.wrapped(); }
	friend constexpr bool operator!=(const __gc_variant &a, const __gc_variant &b) { return a.wrapped() != b.wrapped(); }
	friend constexpr bool operator<(const __gc_variant &a, const __gc_variant &b) { return a.wrapped() < b.wrapped(); }
	friend constexpr bool operator<=(const __gc_variant &a, const __gc_variant &b) { return a.wrapped() <= b.wrapped(); }
	friend constexpr bool operator>(const __gc_variant &a, const __gc_variant &b) { return a.wrapped() > b.wrapped(); }
	friend constexpr bool operator>=(const __gc_variant &a, const __gc_variant &b) { return a.wrapped() >= b.wrapped(); }
};
template<typename Lockable, typename ...Types>
struct GC::router<__gc_variant<Lockable, Types...>>
{
	// if all the variant options are trivial, the variant is always trivial
	static constexpr bool is_trivial = GC::all_have_trivial_routers<Types...>::value;

	template<typename F>
	static void route(const __gc_variant<Lockable, Types...> &var, F func)
	{
		std::lock_guard lock(var.mutex);
		GC::route(var.wrapped(), func);
	}
};

namespace std
{
	template<std::size_t I, typename Lockable, typename ...Types>
	constexpr decltype(auto) get(__gc_variant<Lockable, Types...> &var) { return var.template get<I>(); }
	template<std::size_t I, typename Lockable, typename ...Types>
	constexpr decltype(auto) get(const __gc_variant<Lockable, Types...> &var) { return var.template get<I>(); }
	template<std::size_t I, typename Lockable, typename ...Types>
	constexpr decltype(auto) get(__gc_variant<Lockable, Types...> &&var) { return std::move(var).template get<I>(); }

	template<typename T, typename Lockable, typename ...Types>
	constexpr decltype(auto) get(__gc_variant<Lockable, Types...> &var) { return var.template get<T>(); }
	template<typename T, typename Lockable, typename ...Types>
	constexpr decltype(auto) get(const __gc_variant<Lockable, Types...> &var) { return var.template get<T>(); }
	template<typename T, typename Lockable, typename ...Types>
	constexpr decltype(auto) get(__gc_variant<Lockable, Types...> &&var) { return std::move(var).template get<T>(); }

	template<std::size_t I, typename Lockable, typename ...Types>
	constexpr decltype(auto) get_if(__gc_variant<Lockable, Types...> *pvar) noexcept { return pvar ? pvar->template get_if<I>() : nullptr; }
	template<std::size_t I, typename Lockable, typename ...Types>
	constexpr decltype(auto) get_if(const __gc_variant<Lockable, Types...> *pvar) noexcept { return pvar ? pvar->template get_if<I>() : nullptr; }

	template<typename T, typename Lockable, typename ...Types>
	constexpr decltype(auto) get_if(__gc_variant<Lockable, Types...> *pvar) noexcept { return pvar ? pvar->template get_if<T>() : nullptr; }
	template<typename T, typename Lockable, typename ...Types>
	constexpr decltype(auto) get_if(const __gc_variant<Lockable, Types...> *pvar) noexcept { return pvar ? pvar->template get_if<T>() : nullptr; }

	template<typename T, typename Lockable, typename ...Types>
	constexpr bool holds_alternative(const __gc_variant<Lockable, Types...> &var) noexcept { return var.template holds_alternative<T>(); }
}

template<typename Lockable, typename ...Types>
struct std::variant_size<__gc_variant<Lockable, Types...>> : std::variant_size<std::variant<Types...>> {};

template<std::size_t I, typename Lockable, typename ...Types>
struct std::variant_alternative<I, __gc_variant<Lockable, Types...>> : std::variant_alternative<I, std::variant<Types...>> {};

// hash function for gc variant
template<typename Lockable, typename ...Types>
struct std::hash<__gc_variant<Lockable, Types...>>
{
	std::hash<std::variant<Types...>> hasher; // the hasher may be non-trivial, so store it
	std::size_t operator()(const __gc_variant<Lockable, Types...> &var) const { return hasher(var.wrapped()); }
};

// ------------------------------------------------------------------------------------

template<typename T, typename Lockable>
class __gc_optional
{
private: // -- data -- //

	typedef std::optional<T> wrapped_t; // the wrapped type

	alignas(wrapped_t) char buffer[sizeof(wrapped_t)]; // buffer for the wrapped object

	mutable Lockable mutex; // router synchronizer

	friend struct GC::router<__gc_optional>;

	friend struct std::hash<__gc_optional>;

private: // -- data accessors -- //

	// gets the wrapped object from the buffer by reference - und if the buffered object has not yet been constructed
	wrapped_t &wrapped() noexcept { return *reinterpret_cast<wrapped_t*>(buffer); }
	const wrapped_t &wrapped() const noexcept { return *reinterpret_cast<const wrapped_t*>(buffer); }

public: // -- wrapped obj access -- //

	// gets the std::variant wrapped object
	operator const wrapped_t&() const& { return wrapped(); }
	operator wrapped_t() && { return std::move(wrapped()); }

public: // -- types -- //

	typedef T value_type;

public: // -- ctor / dtor -- //

	constexpr __gc_optional() noexcept
	{
		new (buffer) wrapped_t();
	}
	constexpr __gc_optional(std::nullopt_t) noexcept
	{
		new (buffer) wrapped_t(std::nullopt);
	}

	constexpr __gc_optional(const __gc_optional &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	constexpr __gc_optional(const wrapped_t &other)
	{
		new (buffer) wrapped_t(other);
	}

	constexpr __gc_optional(__gc_optional &&other)
	{
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	constexpr __gc_optional(wrapped_t &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	template<typename U, typename ULockable, std::enable_if_t<std::is_convertible<const U&, T>::value, int> = 0>
	__gc_optional(const __gc_optional<U, ULockable> &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	template<typename U, std::enable_if_t<std::is_convertible<const U&, T>::value, int> = 0>
	__gc_optional(const std::optional<U> &other)
	{
		new (buffer) wrapped_t(other);
	}

	template<typename U, typename ULockable, std::enable_if_t<!std::is_convertible<const U&, T>::value, int> = 0>
	explicit __gc_optional(const __gc_optional<U, ULockable> &other)
	{
		new (buffer) wrapped_t(other.wrapped());
	}
	template<typename U, std::enable_if_t<!std::is_convertible<const U&, T>::value, int> = 0>
	explicit __gc_optional(const std::optional<U> &other)
	{
		new (buffer) wrapped_t(other);
	}

	template<typename U, typename ULockable, std::enable_if_t<std::is_convertible<U&&, T>::value, int> = 0>
	__gc_optional(__gc_optional<U, ULockable> &&other)
	{
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	template<typename U, std::enable_if_t<std::is_convertible<U&&, T>::value, int> = 0>
	__gc_optional(std::optional<U> &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	template<typename U, typename ULockable, std::enable_if_t<!std::is_convertible<U&&, T>::value, int> = 0>
	explicit __gc_optional(__gc_optional<U, ULockable> &&other)
	{
		new (buffer) wrapped_t(std::move(other.wrapped()));
	}
	template<typename U, std::enable_if_t<!std::is_convertible<U&&, T>::value, int> = 0>
	explicit __gc_optional(std::optional<U> &&other)
	{
		new (buffer) wrapped_t(std::move(other));
	}

	template<typename ...Args>
	constexpr explicit __gc_optional(std::in_place_t, Args &&...args)
	{
		new (buffer) wrapped_t(std::in_place, std::forward<Args>(args)...);
	}
	template<typename U, typename ...Args>
	constexpr explicit __gc_optional(std::in_place_t, std::initializer_list<U> il, Args &&...args)
	{
		new (buffer) wrapped_t(std::in_place, il, std::forward<Args>(args)...);
	}

	template<typename U = value_type, std::enable_if_t<std::is_convertible<U&&, T>::value, int> = 0>
	constexpr __gc_optional(U &&value)
	{
		new (buffer) wrapped_t(std::forward<U>(value));
	}
	template<typename U = value_type, std::enable_if_t<!std::is_convertible<U&&, T>::value, int> = 0>
	constexpr explicit __gc_optional(U &&value)
	{
		new (buffer) wrapped_t(std::forward<U>(value));
	}

	~__gc_optional()
	{
		wrapped().~wrapped_t();
	}

public: // -- assign -- //

	__gc_optional &operator=(std::nullopt_t)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::nullopt;
		return *this;
	}

	constexpr __gc_optional &operator=(const __gc_optional &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	constexpr __gc_optional &operator=(const wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	constexpr __gc_optional &operator=(__gc_optional &&other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped() = std::move(other.wrapped());
		}
		return *this;
	}
	constexpr __gc_optional &operator=(wrapped_t &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

	template<typename U = T>
	__gc_optional &operator=(U &&value)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::forward<U>(value);
		return *this;
	}

	template<typename U, typename ULockable, std::enable_if_t<!std::is_same<__gc_optional, __gc_optional<U, ULockable>>::value, int> = 0>
	__gc_optional &operator=(const __gc_optional<U, ULockable> &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other.wrapped();
		return *this;
	}
	template<typename U, std::enable_if_t<!std::is_same<wrapped_t, std::optional<U>>::value, int> = 0>
	__gc_optional &operator=(const std::optional<U> &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = other;
		return *this;
	}

	template<typename U, typename ULockable, std::enable_if_t<!std::is_same<__gc_optional, __gc_optional<U, ULockable>>::value, int> = 0>
	__gc_optional &operator=(__gc_optional<U, ULockable> &&other)
	{
		std::scoped_lock locks(this->mutex, other.mutex);
		wrapped() = std::move(other.wrapped());
		return *this;
	}
	template<typename U, std::enable_if_t<!std::is_same<wrapped_t, std::optional<U>>::value, int> = 0>
	__gc_optional &operator=(std::optional<U> &&other)
	{
		std::lock_guard lock(this->mutex);
		wrapped() = std::move(other);
		return *this;
	}

public: // -- observers -- //

	constexpr decltype(auto) operator->() { return wrapped().operator->(); }
	constexpr decltype(auto) operator->() const { return wrapped().operator->(); }

	constexpr decltype(auto) operator*() & { return *wrapped(); }
	constexpr decltype(auto) operator*() const& { return *wrapped(); }
	constexpr decltype(auto) operator*() &&
	{
		std::lock_guard lock(this->mutex); // not sure if this lock is needed
		return *std::move(wrapped());
	}
	constexpr decltype(auto) operator*() const&&
	{
		std::lock_guard lock(this->mutex); // not sure if this lock is needed
		return *std::move(wrapped());
	}

	constexpr explicit operator bool() const noexcept { return static_cast<bool>(wrapped()); }
	constexpr bool has_value() const noexcept { return wrapped().has_value(); }

	constexpr decltype(auto) value() & { return wrapped().value(); }
	constexpr decltype(auto) value() const& { return wrapped().value(); }
	constexpr decltype(auto) value() &&
	{
		std::lock_guard lock(this->mutex); // not sure if this lock is needed
		return std::move(wrapped()).value();
	}
	constexpr decltype(auto) value() const&&
	{
		std::lock_guard lock(this->mutex); // not sure if this lock is needed
		return std::move(wrapped()).value();
	}

	template<typename U>
	constexpr T value_or(U &&default_value) const& { return wrapped().value_or(std::forward<U>(default_value)); }
	template<typename U>
	constexpr T value_or(U &&default_value) &&
	{
		std::lock_guard lock(this->mutex); // not sure if this lock is needed
		return std::move(wrapped()).value_or(std::forward<U>(default_value));
	}

public: // -- modifiers -- //

	void swap(__gc_optional &other)
	{
		if (this != &other)
		{
			std::scoped_lock locks(this->mutex, other.mutex);
			wrapped().swap(other.wrapped());
		}
	}
	void swap(wrapped_t &other)
	{
		std::lock_guard lock(this->mutex);
		wrapped().swap(other);
	}

	friend void swap(__gc_optional &a, __gc_optional &b) { a.swap(b); }
	friend void swap(__gc_optional &a, wrapped_t &b) { a.swap(b); }
	friend void swap(wrapped_t &a, __gc_optional &b) { b.swap(a); }

	void reset()
	{
		std::lock_guard lock(this->mutex);
		wrapped().reset();
	}

	template<typename ...Args>
	T &emplace(Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(std::forward<Args>(args)...);
	}
	template<typename U, typename ...Args>
	T &emplace(std::initializer_list<U> il, Args &&...args)
	{
		std::lock_guard lock(this->mutex);
		return wrapped().emplace(il, std::forward<Args>(args)...);
	}

public: // -- comparison -- //

	friend constexpr bool operator==(const __gc_optional &a, const __gc_optional &b) { return a.wrapped() == b.wrapped(); }
	friend constexpr bool operator!=(const __gc_optional &a, const __gc_optional &b) { return a.wrapped() != b.wrapped(); }
	friend constexpr bool operator<(const __gc_optional &a, const __gc_optional &b) { return a.wrapped() < b.wrapped(); }
	friend constexpr bool operator<=(const __gc_optional &a, const __gc_optional &b) { return a.wrapped() <= b.wrapped(); }
	friend constexpr bool operator>(const __gc_optional &a, const __gc_optional &b) { return a.wrapped() > b.wrapped(); }
	friend constexpr bool operator>=(const __gc_optional &a, const __gc_optional &b) { return a.wrapped() >= b.wrapped(); }
};
template<typename T, typename Lockable>
struct GC::router<__gc_optional<T, Lockable>>
{
	// if the optional type is trivial, so is this
	static constexpr bool is_trivial = GC::has_trivial_router<T>::value;

	template<typename F>
	static void route(const __gc_optional<T, Lockable> &var, F func)
	{
		std::lock_guard lock(var.mutex);
		GC::route(var.wrapped(), func);
	}
};

template<typename T, typename Lockable>
struct std::hash<__gc_optional<T, Lockable>>
{
	std::hash<std::optional<T>> hasher; // the hasher may be non-trivial, so store it
	std::size_t operator()(const __gc_optional<T, Lockable> &var) const { return hasher(var.wrapped()); }
};

// ------------------------ //

// -- wrapper conversion -- //

// ------------------------ //

template<typename T, typename Deleter, typename Lockable>
struct GC::wrapper_traits<__gc_unique_ptr<T, Deleter, Lockable>>
{
	using unwrapped_type = std::unique_ptr<T, Deleter>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unique_ptr<T, Deleter, _Lockable>>;
};
template<typename T, typename Deleter>
struct GC::wrapper_traits<std::unique_ptr<T, Deleter>>
{
	using unwrapped_type = std::unique_ptr<T, Deleter>;
	
	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unique_ptr<T, Deleter, _Lockable>>;
};

template<typename T, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_vector<T, Allocator, Lockable>>
{
	using unwrapped_type = std::vector<T, Allocator>;
	
	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_vector<T, Allocator, _Lockable>>;
};
template<typename T, typename Allocator>
struct GC::wrapper_traits<std::vector<T, Allocator>>
{
	using unwrapped_type = std::vector<T, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_vector<T, Allocator, _Lockable>>;
};

template<typename T, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_deque<T, Allocator, Lockable>>
{
	using unwrapped_type = std::deque<T, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_deque<T, Allocator, _Lockable>>;
};
template<typename T, typename Allocator>
struct GC::wrapper_traits<std::deque<T, Allocator>>
{
	using unwrapped_type = std::deque<T, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_deque<T, Allocator, _Lockable>>;
};

template<typename T, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_forward_list<T, Allocator, Lockable>>
{
	using unwrapped_type = std::forward_list<T, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_forward_list<T, Allocator, _Lockable>>;
};
template<typename T, typename Allocator>
struct GC::wrapper_traits<std::forward_list<T, Allocator>>
{
	using unwrapped_type = std::forward_list<T, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_forward_list<T, Allocator, _Lockable>>;
};

template<typename T, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_list<T, Allocator, Lockable>>
{
	using unwrapped_type = std::list<T, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_list<T, Allocator, _Lockable>>;
};
template<typename T, typename Allocator>
struct GC::wrapper_traits<std::list<T, Allocator>>
{
	using unwrapped_type = std::list<T, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_list<T, Allocator, _Lockable>>;
};

template<typename Key, typename Compare, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_set<Key, Compare, Allocator, Lockable>>
{
	using unwrapped_type = std::set<Key, Compare, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_set<Key, Compare, Allocator, _Lockable>>;
};
template<typename Key, typename Compare, typename Allocator>
struct GC::wrapper_traits<std::set<Key, Compare, Allocator>>
{
	using unwrapped_type = std::set<Key, Compare, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_set<Key, Compare, Allocator, _Lockable>>;
};

template<typename Key, typename Compare, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_multiset<Key, Compare, Allocator, Lockable>>
{
	using unwrapped_type = std::multiset<Key, Compare, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_multiset<Key, Compare, Allocator, _Lockable>>;
};
template<typename Key, typename Compare, typename Allocator>
struct GC::wrapper_traits<std::multiset<Key, Compare, Allocator>>
{
	using unwrapped_type = std::multiset<Key, Compare, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_multiset<Key, Compare, Allocator, _Lockable>>;
};

template<typename Key, typename T, typename Compare, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_map<Key, T, Compare, Allocator, Lockable>>
{
	using unwrapped_type = std::map<Key, T, Compare, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_map<Key, T, Compare, Allocator, _Lockable>>;
};
template<typename Key, typename T, typename Compare, typename Allocator>
struct GC::wrapper_traits<std::map<Key, T, Compare, Allocator>>
{
	using unwrapped_type = std::map<Key, T, Compare, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_map<Key, T, Compare, Allocator, _Lockable>>;
};

template<typename Key, typename T, typename Compare, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_multimap<Key, T, Compare, Allocator, Lockable>>
{
	using unwrapped_type = std::multimap<Key, T, Compare, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_multimap<Key, T, Compare, Allocator, _Lockable>>;
};
template<typename Key, typename T, typename Compare, typename Allocator>
struct GC::wrapper_traits<std::multimap<Key, T, Compare, Allocator>>
{
	using unwrapped_type = std::multimap<Key, T, Compare, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_multimap<Key, T, Compare, Allocator, _Lockable>>;
};

template<typename Key, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_unordered_set<Key, Hash, KeyEqual, Allocator, Lockable>>
{
	using unwrapped_type = std::unordered_set<Key, Hash, KeyEqual, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unordered_set<Key, Hash, KeyEqual, Allocator, _Lockable>>;
};
template<typename Key, typename Hash, typename KeyEqual, typename Allocator>
struct GC::wrapper_traits<std::unordered_set<Key, Hash, KeyEqual, Allocator>>
{
	using unwrapped_type = std::unordered_set<Key, Hash, KeyEqual, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unordered_set<Key, Hash, KeyEqual, Allocator, _Lockable>>;
};

template<typename Key, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_unordered_multiset<Key, Hash, KeyEqual, Allocator, Lockable>>
{
	using unwrapped_type = std::unordered_multiset<Key, Hash, KeyEqual, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unordered_multiset<Key, Hash, KeyEqual, Allocator, _Lockable>>;
};
template<typename Key, typename Hash, typename KeyEqual, typename Allocator>
struct GC::wrapper_traits<std::unordered_multiset<Key, Hash, KeyEqual, Allocator>>
{
	using unwrapped_type = std::unordered_multiset<Key, Hash, KeyEqual, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unordered_multiset<Key, Hash, KeyEqual, Allocator, _Lockable>>;
};

template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_unordered_map<Key, T, Hash, KeyEqual, Allocator, Lockable>>
{
	using unwrapped_type = std::unordered_map<Key, T, Hash, KeyEqual, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unordered_map<Key, T, Hash, KeyEqual, Allocator, _Lockable>>;
};
template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
struct GC::wrapper_traits<std::unordered_map<Key, T, Hash, KeyEqual, Allocator>>
{
	using unwrapped_type = std::unordered_map<Key, T, Hash, KeyEqual, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unordered_map<Key, T, Hash, KeyEqual, Allocator, _Lockable>>;
};

template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator, typename Lockable>
struct GC::wrapper_traits<__gc_unordered_multimap<Key, T, Hash, KeyEqual, Allocator, Lockable>>
{
	using unwrapped_type = std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unordered_multimap<Key, T, Hash, KeyEqual, Allocator, _Lockable>>;
};
template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
struct GC::wrapper_traits<std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator>>
{
	using unwrapped_type = std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_unordered_multimap<Key, T, Hash, KeyEqual, Allocator, _Lockable>>;
};

template<typename Lockable, typename ...Types>
struct GC::wrapper_traits<__gc_variant<Lockable, Types...>>
{
	using unwrapped_type = std::variant<Types...>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_variant<_Lockable, Types...>>;
};
template<typename ...Types>
struct GC::wrapper_traits<std::variant<Types...>>
{
	using unwrapped_type = std::variant<Types...>;

	template<typename _Lockable = GC::default_lockable_t>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_variant<_Lockable, Types...>>;
};

template<typename T, typename Lockable>
struct GC::wrapper_traits<__gc_optional<T, Lockable>>
{
	using unwrapped_type = std::optional<T>;

	template<typename _Lockable = Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_optional<T, _Lockable>>;
};
template<typename T>
struct GC::wrapper_traits<std::optional<T>>
{
	using unwrapped_type = std::optional<T>;

	template<typename _Lockable>
	using wrapped_type = std::conditional_t<GC::has_trivial_router<unwrapped_type>::value, unwrapped_type, __gc_optional<T, _Lockable>>;
};

// ------------------ //

// -- usage guards -- //

// ------------------ //

static struct __gc_primary_usage_guard_t
{
	__gc_primary_usage_guard_t() { GC::disjoint_module::primary_handle(); }
} __gc_primary_usage_guard;

static thread_local struct __gc_local_usage_guard_t
{
	__gc_local_usage_guard_t() { GC::disjoint_module::local_handle(); }
} __gc_local_usage_guard;

#endif
