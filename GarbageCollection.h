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

// ------------------------ //

// -- Garbage Collection -- //

// ------------------------ //

class GC
{
public: // -- outgoing arcs -- //

	// the type used to represent the outgoing garbage-collected pointers from an object.
	// this is a pair of begin/end iterators to an array of byte offsets for said gc pointers in the object.
	// only GC::ptr counts as a gc pointer - including any other type is undefined behavior.
	// it is undefined behavior for a type to have a gc pointer that is not represented in this range.
	typedef std::pair<const std::size_t*, const std::size_t*> outgoing_t;

	// standardized way of getting gc_outgoing_t values for the ptr wrapper class.
	// default implementation is sufficient for any type that does not contain a gc qualified pointer.
	// if this is not the case, you must specialize this template function to work for your type.
	template<typename T>
	static outgoing_t outgoing() { return {nullptr, nullptr}; }

private: // -- private types -- //

	// represents a single garbage-collected object's allocation info.
	// this is used internally by the garbage collector's logic - DO NOT MANUALLY MODIFY THIS.
	// ANY POINTER OF THIS TYPE MUST AT ALL TIMES POINT TO A VALID OBJECT OR NULL.
	struct info
	{
		void *const obj;             // pointer to the managed object
		void(*const deleter)(void*); // a deleter function to eventually delete obj

		outgoing_t(*const outgoing)(); // offsets of outgoing gc-qualified pointers from obj
		std::size_t ref_count;         // the reference count for this allocation

		info *prev; // the std::list iterator contract isn't quite what we want
		info *next; // so we need to manage a linked list on our own

		// populates info
		info(void *_obj, void(*_deleter)(void*), outgoing_t(*_outgoing)(), std::size_t _ref_count, info *_prev, info *_next)
			: obj(_obj), deleter(_deleter), outgoing(_outgoing), ref_count(_ref_count), prev(_prev), next(_next)
		{}
	};

public: // -- public interface -- //
	template<typename T> struct ptr;
	// wraps the gc management functions into a self-managed system that requires no thought to use.
	template<typename T>
	struct ptr
	{
	private: // -- data -- //

		// the handle to use for gc management functions.
		// THIS MUST BE THE FIRST MEMBER VARIABLE IN THE OBJECT (uses C-style pointer to first field equivalence).
		GC::info *handle;

		friend class GC;

	private: // -- helpers -- //

		void reset(GC::info *_handle = nullptr)
		{
			// we only need to do anything if we refer to different gc allocations
			if (handle != _handle)
			{
				// drop our object
				if (handle) GC::delref(handle);

				// take on other's object
				handle = _handle;
				if (handle) GC::addref(handle);
			}
		}

		// constructs a new ptr that manages the specified handle.
		// the handle must be either null (creates an empty ptr) or refer to a valid gc_info object.
		// the handle must have been allocated via gc_create().
		// the handle must not be modified in any way or passed to any gc functions before or after this call.
		// the handle must not have already been given to a different ptr's constructor.
		// the handle's referenced object must refer to a valid object of type T.
		explicit ptr(GC::info *_handle) : handle(_handle)
		{
			static_assert(offsetof(ptr, handle) == 0, "compiler violated C-style pointer-to-first-field equivalence");

			// register handle as a root
			GC::root(handle);
		}

	public: // -- ctor / dtor / asgn -- //

		// creates an empty ptr (null)
		ptr() : handle(nullptr)
		{
			// register handle as a root
			GC::root(handle);
		}

		~ptr()
		{
			// if we have a handle, dec reference count
			if (handle) GC::delref(std::exchange(handle, nullptr));

			// unregister handle as a root
			GC::unroot(handle);
		}

		ptr(const ptr &other)
		{
			// register handle as a root
			GC::root(handle);

			// we'll point at the same object
			handle = other.handle;

			// we're new - inc ref count
			if (handle) GC::addref(handle);
		}
		ptr(ptr&&) = delete;

		ptr &operator=(const ptr &other)
		{
			reset(other.handle);
			return *this;
		}
		ptr &operator=(ptr&&) = delete;

		// equivalent to reset(nullptr)
		ptr &operator=(std::nullptr_t)
		{
			reset(nullptr);
			return *this;
		}

	public: // -- obj access -- //

		T &operator*() const { return *(T*)handle->obj; }
		T *operator->() const { return (T*)handle->obj; }

		// returns the current number of gc-qualified pointers referencing the same object as this instance.
		// if this object is not pointing at any object, returns 0.
		std::size_t use_count() const { return handle ? handle->ref_count : 0; }

		// gets a pointer to the pointed-to managed object.
		// if this ptr does not point at a managed object, returns null.
		// DO NOT DELETE THIS
		T *get() const { return handle ? (T*)handle->obj : nullptr; }

		// reutrns true iff this ptr points to a managed object (non-null)
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

	// creates a new dynamic instance of T that is bound to a ptr.
	// this is the preferred method of creating ptr instances and minimizes error.
	// throws any exception resulting from T's constructor but does not leak resources.
	template<typename T, typename ...Args>
	static ptr<T> make(Args &&...args)
	{
		// allocate the object
		std::unique_ptr<T> obj = std::make_unique<T>(std::forward<Args>(args)...);

		// create a handle for it
		GC::info *handle = GC::create(obj.release(), [](void *ptr) { delete (T*)ptr; }, GC::outgoing<T>);

		// return it as a ptr
		return ptr<T>(handle);
	}

	// triggers a full garbage collection pass.
	// objects that are not in use will be deleted.
	// objects that are in use will not be moved (i.e. pointers will still be valid).
	static void collect();

private: // -- data -- //

	static std::mutex mutex; // used to support thread-safety of gc operations

	static info *first; // pointer to the first gc allocation
	static info *last; // pointer to the last gc allocation (not the same as the end iterator)

	static std::unordered_set<info**> roots; // a database of all gc root handles - (references - do not delete)

	GC() = delete; // not instantiatable

private: // -- private interface -- //

	// registers a gc_info* as a root.
	// if it's already a root, does nothing.
	static void root(info *&handle);
	// unregisters a gc_info* as a root.
	// if it's not currently a root, does nothing.
	static void unroot(info *&handle);

	// adds a pre-existing (non-garbage-collected) object to the garbage-collection database.
	// returns a handle that must be used to control the gc allocation - DO NOT LOSE THIS - DO NOT MODIFY THIS IN ANY WAY.
	// the aliased object begins with a reference count of 1 - YOU SHOULD NOT CALL ADDREF ~BECAUSE~ OF CREATE.
	// it is undefined behavior to call this function on an object that already exists in the gc database.
	// <obj> is the address of the actual object that was allocated dynamically that should now be managed.
	// <deleter> is a function that will be used to deallocate <obj>.
	// <outgoing> is a function that returns the begin/end range of outgoing gc-qualified pointer offsets from <obj>.
	static info *create(void *obj, void(*deleter)(void*), outgoing_t(*outgoing)());

	// adds/removes a reference count to/from a garbage-collected object.
	// <handle> is the address of an object currently under garbage collection.
	// if <handle> does not refer to a pre-existing gc allocation, calls std::exit(1).
	static void addref(info *handle);
	static void delref(info *handle);
};

// -------------------- //

// -- misc functions -- //

// -------------------- //

// if T is a polymorphic type, returns a pointer to the most-derived object pointed by <ptr>; otherwise, returns <ptr>.
template<typename T, std::enable_if_t<std::is_polymorphic_v<T>, int> = 0>
void *get_polymorphic_root(T *ptr) { return dynamic_cast<void*>(ptr); }
template<typename T, std::enable_if_t<!std::is_polymorphic_v<T>, int> = 0>
void *get_polymorphic_root(T *ptr) { return ptr; }

// outputs the stored pointer to the stream - equivalent to ostr << ptr.get()
template<typename T, typename U, typename V>
std::basic_ostream<U, V> &operator<<(std::basic_ostream<U, V> &ostr, const GC::ptr<T> &ptr)
{
	ostr << ptr.get();
	return ostr;
}

#endif
