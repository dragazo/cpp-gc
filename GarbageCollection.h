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

// ------------------------ //

// -- Garbage Collection -- //

// ------------------------ //

class GC
{
public: // -- outgoing arcs -- //

	// the type used to represent the outgoing GC::ptr instances from a given object.
	// this is a pair of begin/end iterators to an array of byte offsets for said GC::ptr instances in the object.
	typedef std::pair<const std::size_t*, const std::size_t*> outgoing_t;

	// standardized way of getting GC::outgoing_t values for any type T.
	// this default implementation is sufficient for any type that does not contain a GC::ptr by value.
	// if this is not the case, you must specialize this template to work for your type prior to using any gc utilities.
	// it is undefined behavior for a type to hold a GC::ptr by value that is not represented in this range.
	// it is undefined behavior for any type other than GC::ptr to be represented in this range.
	template<typename T>
	struct outgoing { static outgoing_t get() { return {nullptr, nullptr}; } };

private: // -- private types -- //

	// represents a single garbage-collected object's allocation info.
	// this is used internally by the garbage collector's logic - DO NOT MANUALLY MODIFY THIS.
	// ANY POINTER OF THIS TYPE MUST AT ALL TIMES POINT TO A VALID OBJECT OR NULL.
	struct info
	{
		void *const obj;             // pointer to the managed object
		void(*const deleter)(void*); // a deleter function to eventually delete obj

		outgoing_t(*const outgoing)(); // offsets of outgoing GC::ptr from obj
		std::size_t ref_count;         // the reference count for this allocation

		bool marked; // only used for GC::collect() - otherwise undefined

		bool destroying = false; // marks if the object is currently in the process of being destroyed (multi-delete safety flag)

		info *prev; // the std::list iterator contract isn't quite what we want
		info *next; // so we need to manage a linked list on our own

		// populates info
		info(void *_obj, void(*_deleter)(void*), outgoing_t(*_outgoing)(), std::size_t _ref_count, info *_prev, info *_next)
			: obj(_obj), deleter(_deleter), outgoing(_outgoing), ref_count(_ref_count), prev(_prev), next(_next)
		{}
	};

public: // -- public interface -- //
	
	// a self-managed garbage-collected pointer
	template<typename T>
	struct ptr
	{
	private: // -- data -- //

		// the handle to use for gc management functions.
		// THIS MUST BE THE FIRST MEMBER VARIABLE IN THE OBJECT (uses C-style pointer to first field equivalence).
		GC::info *handle;

		friend class GC;

	private: // -- helpers -- //

		// changes what handle we should use, properly unlinking ourselves from the old one and linking to the new one.
		void reset(GC::info *_handle = nullptr)
		{
			static_assert(offsetof(ptr, handle) == 0, "compiler violated C-style pointer-to-first-field equivalence");

			// we only need to do anything if we refer to different gc allocations
			if (handle != _handle)
			{
				bool call_handle_del_list = handle != nullptr;

				{
					std::lock_guard<std::mutex> lock(GC::mutex);

					// drop our object
					if (handle) GC::__delref(handle);

					// take on other's object
					handle = _handle;
					if (handle) GC::__addref(handle);
				}

				// after a call to __delref we must call handle_del_list()
				if (call_handle_del_list) GC::handle_del_list();
			}
		}

		// creates an empty ptr but DOES NOT ROOT THE INTERNAL HANDLE
		struct no_rooting_t {};
		explicit ptr(no_rooting_t) : handle(nullptr) {}
		// meant to be used after the no_rooting_t ctor - SETS HANDLE AND ROOTS INTERNAL HANDLE
		void __init(GC::info *_handle)
		{
			handle = _handle;
			GC::__root(handle);
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
			bool call_handle_del_list = handle != nullptr;

			{
				std::lock_guard<std::mutex> lock(GC::mutex);

				// if we have a handle, dec reference count
				if (handle) GC::__delref(handle);

				// set it to null just to be safe
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

	// specialization of outgoing for ptr<T> (i.e. ptr<T> can be thought of as a struct containing a ptr<T>)
	template<typename T>
	struct outgoing<ptr<T>>
	{
		static outgoing_t get()
		{
			static const std::size_t outs[] = {0};
			return {std::begin(outs), std::end(outs)};
		}
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
		ptr<T> res(ptr<T>::no_rooting_t{});

		{
			std::lock_guard<std::mutex> lock(mutex);

			// for each outgoing arc from obj
			for (outgoing_t outs = GC::outgoing<T>::get(); outs.first != outs.second; ++outs.first)
			{
				// get reference to this arc
				GC::info *&arc = *(GC::info**)((char*)raw + *outs.first);

				// mark this arc as not being a root (because obj owns it by value)
				GC::__unroot(arc);
			}

			// create a handle for it
			GC::info *handle = GC::__create(raw, [](void *ptr) { delete (T*)ptr; }, GC::outgoing<T>::get);

			// initialize ptr with handle
			res.__init(handle);
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

private: // -- data -- //

	static std::mutex mutex; // used to support thread-safety of gc operations

	static info *first; // pointer to the first gc allocation
	static info *last;  // pointer to the last gc allocation (not the same as the end iterator)

	static std::unordered_set<info**> roots; // a database of all gc root handles - (references - do not delete)

	static std::unordered_set<info*> del_list; // list of handles that are scheduled for deletion (from __delref async calls)

	GC() = delete; // not instantiatable

private: // -- private interface -- //

	// -----------------------------------------------------------------
	// functions in this block that start with "__" are not thread safe.
	// all other functions in this block are thread safe.
	// -----------------------------------------------------------------

	// registers a gc_info* as a root.
	// if it's already a root, does nothing.
	static void __root(info *&handle);
	static void root(info *&handle);

	// unregisters a gc_info* as a root.
	// if it's not currently a root, does nothing.
	static void __unroot(info *&handle);
	static void unroot(info *&handle);

	// adds a pre-existing (non-garbage-collected) object to the garbage-collection database.
	// returns a handle that must be used to control the gc allocation - DO NOT LOSE THIS - DO NOT MODIFY THIS IN ANY WAY.
	// the aliased object begins with a reference count of 1 - YOU SHOULD NOT CALL ADDREF ~BECAUSE~ OF CREATE.
	// it is undefined behavior to call this function on an object that already exists in the gc database.
	// <obj> is the address of the actual object that was allocated dynamically that should now be managed.
	// <deleter> is a function that will be used to deallocate <obj>.
	// <outgoing> is a function that returns the begin/end range of outgoing gc-qualified pointer offsets from <obj>.
	static info *__create(void *obj, void(*deleter)(void*), outgoing_t(*outgoing)());
	static info *create(void *obj, void(*deleter)(void*), outgoing_t(*outgoing)());

	// unlinks handle from the gc database.
	// it is undefined behavior if handle is not currently in the gc database.
	// the prev/next pointers of handle are not modified by this operation (though others in the gc database are).
	static void __unlink(info *handle);
	// invokes the stored deleter for handle's object and deletes the handle itself.
	// this is meant to be used on a handle after it has been unlinked from the gc database.
	static void __destroy(info *handle);

	// adds/removes a reference count to/from a garbage-collected object.
	// <handle> is the address of an object currently under garbage collection.
	// if <handle> does not refer to a pre-existing gc allocation, calls std::exit(1).
	static void __addref(info *handle);
	static void addref(info *handle);
	static void delref(info *handle);

	// a non-threadsafe variant of delref().
	// instead of destroying the object immediately when the ref count reaches zero, adds it to del_list.
	// YOU MUST CALL handle_del_list() AFTER THIS (and after unlocking the mutex).
	static void __delref(info *handle);
	static void handle_del_list();

	// performs a mark sweep operation from the given handle.
	static void __mark_sweep(info *handle);
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
