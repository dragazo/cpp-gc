#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <cstddef>
#include <cassert>
#include <sstream>
#include <utility>
#include <cmath>
#include <initializer_list>

#include "GarbageCollection.h"


std::mutex __io_mutex;
// prints stuff to cerr in a thread-safe fashion
template<typename ...Args>
void sync_err_print(Args &&...args)
{
	std::lock_guard<std::mutex> io_lock(__io_mutex);
	(std::cout << ... << std::forward<Args>(args));
}


struct alignas(16) sse_t { char d[16]; };

struct ptr_set
{
	GC::ptr<int> a, b, c, d, e, f, g, h;
	int val;

	std::shared_ptr<int> thingy;

	std::tuple<int, int, int, long, long, int, GC::ptr<int>, int> tuple_thing;

	GC::ptr<GC::unordered_map<std::string, unsigned int>> mapydoo1;
	GC::ptr<GC::unordered_map<std::string, unsigned int>> mapydoo2;

	GC::ptr<int[]> doodle_dud_0;
	GC::ptr<int[]> doodle_dud_1;

	ptr_set() : val(0)
	{
		auto ptr = new std::unordered_map<std::string, unsigned int>;
		ptr->emplace("hello", 67);
		ptr->emplace("world", 4765);

		mapydoo1 = GC::adopt(ptr);
		mapydoo2 = GC::adopt<std::unordered_map<std::string, unsigned int>>(nullptr);

		doodle_dud_0 = GC::adopt(new int[1024], 1024);
		doodle_dud_0 = GC::adopt(new int[2048], 2048);
	}
};
template<> struct GC::router<ptr_set>
{
	static void route(const ptr_set &set, GC::router_fn func)
	{
		GC::route(set.a, func);
		GC::route(set.b, func);
		GC::route(set.c, func);
		GC::route(set.d, func);
		GC::route(set.e, func);
		GC::route(set.f, func);
		GC::route(set.g, func);
		GC::route(set.h, func);

		GC::route(set.val, func);

		GC::route(set.thingy, func);

		GC::route(set.tuple_thing, func);

		GC::route(set.mapydoo1, func);
		GC::route(set.mapydoo2, func);

		GC::route(set.doodle_dud_0, func);
		GC::route(set.doodle_dud_1, func);

		//GC::route(set.a, set.a);
	}
	static void route(const ptr_set &set, GC::mutable_router_fn func) {} // no mutable things to route to
};

struct ListNode
{
	GC::ptr<ListNode> prev;
	GC::ptr<ListNode> next;

	ptr_set set;

	// show a message that says we called ctor
	ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(4)); }

	// show a message that says we called dtor
	~ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(4)); }
};
template<> struct GC::router<ListNode>
{
	static void route(const ListNode &node, GC::router_fn func)
	{
		GC::route(node.prev, func);
		GC::route(node.next, func);

		GC::route(node.set, func);
	}
	static void route(const ListNode &node, GC::mutable_router_fn func) // this is correct
	//static void route(ListNode node, GC::mutable_router_fn func) // this is wrong - by-value T could cause deadlocking
	{
		// only routing to set for future-proofing
		GC::route(node.set, func);
	}
};

// creates a linked list that has a cycle
void foo()
{
	{
		//GC::collect();
		
		// create the first node
		GC::ptr<ListNode> root = GC::make<ListNode>();

		// we'll make 10 links in the chain
		GC::ptr<ListNode> *prev = &root;
		for (int i = 0; i < 10; ++i)
		{
			(*prev)->next = GC::make<ListNode>();
			(*prev)->next->prev = *prev;

			prev = &(*prev)->next;
		}

		// then we'll merege the ends into a cycle
		root->prev = *prev;
		(*prev)->next = root;

		using std::swap;

		root.swap(*prev);
		swap(root, *prev);

		//GC::collect();
		//std::cerr << "\n\n";
	}
}

template<typename T>
struct wrap
{
	GC::ptr<T> ptr;
};
template<typename T> struct GC::router<wrap<T>>
{
	template<typename F> static void route(const wrap<T> &w, F fn)
	{
		GC::route(w.ptr, fn);
	}
};

struct virtual_type
{
	virtual ~virtual_type() {}
};
struct non_virtual_type
{

};

struct base1
{
	int a;

	virtual ~base1() {}
};
struct base2
{
	int b;

	virtual ~base2() {}
};
struct derived : base1, base2
{

};




struct TreeNode
{
    // left and right sub-trees
    GC::ptr<TreeNode> left;
    GC::ptr<TreeNode> right;
    
    double value;
    
    enum op_t { val, add, sub, mul, div } op;
};
template<> struct GC::router<TreeNode>
{
    template<typename F> static void route(const TreeNode &node, F func)
    {
        // route to our GC::ptr instances
        GC::route(node.left, func);
        GC::route(node.right, func);
        // no need to route to anything else
    }
};


class SymbolTable
{
private:
    std::unordered_map<std::string, GC::ptr<TreeNode>> symbols;
    
    // we need to route to the contents of symbols, but symbols is a mutable collection.
    // we therefore need insert/delete to be synchronous with respect to the router:
    mutable std::mutex symbols_mutex;
    
    // make the particular router class a friend so it can use our private data
    friend struct GC::router<SymbolTable>;
    
public:

	// wrapped symbols table type that is safe to modify directly
	GC::unordered_map<std::string, GC::ptr<TreeNode>> better_symbols;

public:

	SymbolTable() = default;

	SymbolTable(const SymbolTable &other) : symbols(other.symbols), better_symbols(other.better_symbols) {}

	SymbolTable &operator=(const SymbolTable &other)
	{
		if (this != &other)
		{
			{
				std::lock_guard lock(symbols_mutex);
				symbols = other.symbols;
			}
			better_symbols = other.better_symbols;
		}
		return *this;
	}
	SymbolTable &operator=(SymbolTable &&other)
	{
		if (this != &other)
		{
			{
				std::scoped_lock locks(symbols_mutex, other.symbols_mutex);
				symbols = std::move(other.symbols);
			}
			better_symbols = std::move(other.better_symbols);
		}
		return *this;
	}

public:
    void update(std::string name, GC::ptr<TreeNode> new_value)
    {
		{
			// modification of the mutable collection of GC::ptr and router must be mutually exclusive
			std::lock_guard<std::mutex> lock(symbols_mutex);
			symbols[name] = new_value;
		}
    }
	void clear()
	{
		{
			std::lock_guard<std::mutex> lock(symbols_mutex);
			symbols.clear();
		}
		better_symbols.clear();
	}
};
template<> struct GC::router<SymbolTable>
{
    template<typename F> static void route(const SymbolTable &table, F func)
    {
		{
			// modification of the mutable collection of GC::ptr and router must be mutually exclusive
			std::lock_guard<std::mutex> lock(table.symbols_mutex);
			GC::route(table.symbols, func);
		}
		GC::route(table.better_symbols, func);
    }
};




class MaybeTreeNode
{
private:
    // the buffer for the object
    alignas(TreeNode) char buf[sizeof(TreeNode)];
    bool contains_tree_node = false;
    
    // because we'll be constructing destucting it on the fly,
    // buf is a mutable container of a type we need to route to.
    // thus we need to synchronize "re-pointing" it and the router function.
    mutable std::mutex buf_mutex;
    
    // friend the particular router class so it can access our private data
    friend struct GC::router<MaybeTreeNode>;
    
public:
    void construct()
    {
        if (contains_tree_node) throw std::runtime_error("baka");
        
        // we need to synchronize with the router
        std::lock_guard<std::mutex> lock(buf_mutex);
        
        // construct the object
        new (buf) TreeNode;
        contains_tree_node = true;
    }
    void destruct()
    {
        if (!contains_tree_node) throw std::runtime_error("baka");
        
        // we need to synchronize with the router
        std::lock_guard<std::mutex> lock(buf_mutex);
        
        // destroy the object
        reinterpret_cast<TreeNode*>(buf)->~TreeNode();
        contains_tree_node = false;
    }
};
template<> struct GC::router<MaybeTreeNode>
{
    template<typename F> static void route(const MaybeTreeNode &maybe, F func)
    {
        // this must be synchronized with constructing/destucting the buffer object.
        std::lock_guard<std::mutex> lock(maybe.buf_mutex);
        
        // because TreeNode contains GC::ptr objects, we need to route to it,
        // but only if the MaybeTreeNode actually has it constructed in the buffer.
        if (maybe.contains_tree_node) GC::route(*reinterpret_cast<const TreeNode*>(maybe.buf), func);
    }
};


#define COMMA ,


struct alert_t
{
	alert_t() { std::cerr << "ctor\n"; }
	~alert_t() { std::cerr << "dtor\n"; }
};





struct atomic_container
{
	GC::atomic_ptr<double> atomic_1;
	GC::atomic_ptr<double> atomic_2;
	GC::atomic_ptr<double> atomic_3;
	GC::atomic_ptr<double> atomic_4;
};
template<>
struct GC::router<atomic_container>
{
	template<typename F> static void route(const atomic_container &atomic, F func)
	{
		GC::route(atomic.atomic_1, func);
		GC::route(atomic.atomic_2, func);
		GC::route(atomic.atomic_3, func);
		GC::route(atomic.atomic_4, func);
	}
};


GC::vector<GC::ptr<int>> global_vec_ptr;
GC::atomic_ptr<atomic_container> global_atomic_ptr;

thread_local GC::vector<GC::ptr<int>> thread_local_vec_ptr;

template<typename T>
std::string tostr(T &&v)
{
	std::ostringstream ostr;
	ostr << std::forward<T>(v);
	return ostr.str();
}



struct self_ptr
{
	std::unique_ptr<GC::ptr<self_ptr>> p;

	self_ptr() { std::cerr << "self_ptr ctor\n"; }
	~self_ptr() { std::cerr << "self_ptr dtor\n"; }
};
template<>
struct GC::router<self_ptr>
{
	template<typename F>
	static void route(const self_ptr &p, F func)
	{
		// we should really be using a mutex here for the std::unique_ptr but this is just a test and i know it's safe ...
		GC::route(p.p, func);
	}
};

struct gc_self_ptr
{
	GC::unique_ptr<GC::ptr<gc_self_ptr>> p;

	gc_self_ptr() { std::cerr << "gc_self_ptr ctor\n"; }
	~gc_self_ptr() { std::cerr << "gc_self_ptr dtor\n"; }
};
template<>
struct GC::router<gc_self_ptr>
{
	template<typename F>
	static void route(const gc_self_ptr &p, F func)
	{
		GC::route(p.p, func);
	}
};


struct bool_alerter
{
	std::atomic<bool> &flag;

	explicit bool_alerter(std::atomic<bool> &d) : flag(d) { flag = false; }
	~bool_alerter() { flag = true; }
};

struct bool_alerter_self_ptr
{
	bool_alerter alerter;
	GC::ptr<bool_alerter_self_ptr> self_p;

	explicit bool_alerter_self_ptr(std::atomic<bool> &d) : alerter(d) {}
};
template<>
struct GC::router< bool_alerter_self_ptr>
{
	template<typename F>
	static void route(const bool_alerter_self_ptr &obj, F func)
	{
		GC::route(obj.self_p, func);
	}
};

// runs statement and asserts that it throws the right type of exception
#define assert_throws(statement, exception) \
try { statement; std::cerr << "did not throw\n"; assert(false); } \
catch (const exception&) {} \
catch (...) { std::cerr << "threw wrong type\n"; assert(false); }

// runs statement and asserts that it throws (anything)
#define assert_throws_any(statement) \
try { statement; std::cerr << "did no throw\n"; assert(false); } \
catch (...) {}

// runs statement and asserts that it throws (anything)
#define assert_throws_disjunction(statement) \
try { statement; std::cerr << "did no throw\n"; assert(false); } \
catch (const GC::disjunction_error&) {} \
catch (...) { std::cerr << "threw wrong type\n"; assert(false); }

// runs statement and asserts that it doesn't throw (anything)
#define assert_nothrow(statement) \
try { statement; } \
catch (...) { std::cerr << "threw an exception\n"; assert(false); }



// wraps type T but doesn't allow copy/move
template<typename T>
class stationary
{
public:
	
	T value;

	template<typename ...Args>
	stationary(Args &&...args) : value(std::forward<Args>(args)...) {}

	stationary(const stationary&) = delete;
	stationary &operator=(const stationary&) = delete;
};
template<typename T>
struct GC::router<stationary<T>>
{
	static constexpr bool is_trivial = GC::has_trivial_router<T>::value;

	template<typename F>
	static void route(const stationary<T> &stat, F func) { GC::route(stat.value, func); }
};






void spooky_scary_dont_do_this()
{
	GC::collect();
	#if DRAGAZO_GARBAGE_COLLECT_USE_IGNORE_COLLECT_IN_WRAPPERS
	std::thread([] { GC::collect(); }).join();
	#endif
}

struct ctor_collect_t
{
	ctor_collect_t() { spooky_scary_dont_do_this(); }
};
template<>
struct GC::router<ctor_collect_t>
{
	template<typename F>
	static void route(const ctor_collect_t &v, F f) {}
};

struct dtor_collect_t
{
	~dtor_collect_t() { spooky_scary_dont_do_this(); }
};
template<>
struct GC::router<dtor_collect_t>
{
	template<typename F>
	static void route(const dtor_collect_t &v, F f) {}
};

struct ctor_dtor_collect_t
{
	ctor_dtor_collect_t() { spooky_scary_dont_do_this(); }
	~ctor_dtor_collect_t() { spooky_scary_dont_do_this(); }
};
template<>
struct GC::router<ctor_dtor_collect_t>
{
	template<typename F>
	static void route(const ctor_dtor_collect_t &v, F f) {}
};


struct cpy_mov_intrin
{
	bool src = false;

	cpy_mov_intrin() { std::cerr << "ctor\n"; }
	~cpy_mov_intrin() { std::cerr << (src ? "SRC dtor\n" : "dtor\n"); }

	cpy_mov_intrin(const cpy_mov_intrin &other) { std::cerr << "cpy ctor\n"; }
	cpy_mov_intrin(cpy_mov_intrin &&other) { std::cerr << "mov ctor\n"; }

	cpy_mov_intrin &operator=(const cpy_mov_intrin &other) { std::cerr << "cpy asgn\n"; return *this; }
	cpy_mov_intrin &operator=(cpy_mov_intrin &&other) { std::cerr << "mov asgn\n"; return *this; }
};
void intrin_printer(cpy_mov_intrin, cpy_mov_intrin)
{
	std::cerr << "in printer\n";
}

void vector_printer(std::vector<int> vec)
{
	for (int i : vec) std::cerr << i << ' ';
	std::cerr << '\n';
}


void print_str(const GC::ptr<std::string> &p)
{
	if (p) std::cerr << *p << '\n';
}


struct access_gc_at_ctor_t
{
	GC::ptr<access_gc_at_ctor_t> p;

	access_gc_at_ctor_t() { std::cerr << "@@@@ ctor gc accessor\n"; }
	~access_gc_at_ctor_t() { std::cerr << "@@@@ dtor gc accessor\n"; GC::collect(); std::cerr << "  -- safe\n"; }
};
template<>
struct GC::router<access_gc_at_ctor_t>
{
	template<typename F>
	static void route(const access_gc_at_ctor_t &obj, F func)
	{
		GC::route(obj.p, func);
	}
};
GC::ptr<access_gc_at_ctor_t> access_gc_at_ctor;


struct router_allocator
{
	mutable GC::ptr<int> p;
	GC::ptr<router_allocator> self;
};
template<>
struct GC::router<router_allocator>
{
	template<typename F>
	static void route(const router_allocator &r, F func)
	{
		// simulate router locking a mutex and a mutator in the class locking and allocating
		r.p = GC::make<int>(45);

		GC::route(r.p, func);
		GC::route(r.self, func);
	}
};

struct thread_func_t
{
	int v;
	void foo() {}
};


// begins a timer in a new scope - requires a matching timer end point.
#define TIMER_BEGIN() { const auto __timer_begin = std::chrono::high_resolution_clock::now();
// ends the timer in the current scope - should not be used if there is no timer in the current scope.
// units is the (unquoted) name of a std::chrono time unit. name is the name of the timer (a c-style string).
#define TIMER_END(units, name) const auto __timer_end = std::chrono::high_resolution_clock::now(); \
	std::cerr << "\ntimer elapsed - " name ": " << std::chrono::duration_cast<std::chrono::units>(__timer_end - __timer_begin).count() << " " #units "\n"; }

// represents a trivial gc type - used for forcing wrapped type conversions
struct gc_t {};
template<> struct GC::router<gc_t> { static void route(const gc_t&) {} };
constexpr bool operator==(const gc_t&, const gc_t&) { return true; }
constexpr bool operator!=(const gc_t&, const gc_t&) { return true; }
constexpr bool operator<(const gc_t&, const gc_t&) { return true; }
constexpr bool operator<=(const gc_t&, const gc_t&) { return true; }
constexpr bool operator>(const gc_t&, const gc_t&) { return true; }
constexpr bool operator>=(const gc_t&, const gc_t&) { return true; }

int main() try
{
	std::cerr << "\nstart main: " << std::this_thread::get_id() << "\n\n";
	struct _end_logger_t
	{
		~_end_logger_t() { std::cerr << "\nend main: " << std::this_thread::get_id() << "\n\n"; }
	} _end_logger;

	TIMER_BEGIN();

	// -- tests that require no background collector to work -- //

	// these tests make the assumption that no other thread is currently performing a collection action.
	// these aren't considered binding tests - i.e. these test conditions aren't actually defined to work properly.
	// they're merely to make sure that, given no interference, the desired behavior is taking effect.

	GC::strategy(GC::strategies::manual);

	// make sure that ref count decrements to zero result in the object being deleted.
	// additionally, make sure that if a collection is happening in another thread the ref count guarantee is satisfied.
	// i.e. if the ref count decs to zero during a collection the object will be destroyed at least by the end of collection.
	// this is in the no-background collect section so we can join the parallel collector thread before the assertion.
	{
		std::atomic<bool> flag;
		for (int i = 0; i < 4096; ++i)
		{
			std::thread test_thread([]()
			{
				try { GC::collect(); }
				catch (...) { std::cerr << "\n\nFLAG TESTER EXCEPTION!!\n\n"; assert(false); }
			});

			{
				GC::ptr<bool_alerter> a = GC::make<bool_alerter>(flag);
			}

			test_thread.join();
			assert(flag);
		}
	}

	// -- all other tests -- //

	GC::strategy(GC::strategies::timed);
	GC::sleep_time(std::chrono::milliseconds(0));

	GC::thread(GC::new_disjunction, [] {
		GC::ptr<router_allocator> p_router_allocator = GC::make<router_allocator>();
		p_router_allocator->self = p_router_allocator;

		std::this_thread::sleep_for(std::chrono::seconds(2));
	}).detach();

	access_gc_at_ctor = GC::make<access_gc_at_ctor_t>();
	access_gc_at_ctor->p = access_gc_at_ctor;

	{
		thread_func_t tt;

		std::invoke(&thread_func_t::foo, tt);

		std::thread(&thread_func_t::foo, tt).join();
		GC::thread(GC::inherit_disjunction, &thread_func_t::foo, tt).join();
	}

	#if DRAGAZO_GARBAGE_COLLECT_DISJUNCTION_SAFETY_CHECKS

	GC::thread(GC::new_disjunction, []
	{
		try
		{
			std::cerr << "\nstarting disjunction exception checks\n";

			auto ptr_a = GC::make<int>();
			auto ptr_b = GC::make<int>();

			// -------------------------------------------------

			std::cerr << "starting asgn test\n";

			GC::thread(GC::primary_disjunction, [](GC::ptr<int> &a, GC::ptr<int> &b)
			{
				assert_nothrow(a = b);
			}, std::ref(ptr_a), std::ref(ptr_b)).join();
			GC::thread(GC::inherit_disjunction, [](GC::ptr<int> &a, GC::ptr<int> &b)
			{
				assert_nothrow(a = b);
			}, std::ref(ptr_a), std::ref(ptr_b)).join();
			GC::thread(GC::new_disjunction, [](GC::ptr<int> &a, GC::ptr<int> &b)
			{
				assert_nothrow(a = b);
			}, std::ref(ptr_a), std::ref(ptr_b)).join();

			// -------------------------------------------------

			std::cerr << "starting asgn new obj test\n";

			GC::thread(GC::primary_disjunction, [](GC::ptr<int> &a)
			{
				assert_throws_disjunction(a = GC::make<int>());
			}, std::ref(ptr_a)).join();
			GC::thread(GC::inherit_disjunction, [](GC::ptr<int> &a)
			{
				assert_nothrow(a = GC::make<int>());
			}, std::ref(ptr_a)).join();
			GC::thread(GC::new_disjunction, [](GC::ptr<int> &a)
			{
				assert_throws_disjunction(a = GC::make<int>());
			}, std::ref(ptr_a)).join();

			// --------------------------------------------------

			std::cerr << "starting swap test a\n";

			GC::thread(GC::primary_disjunction, [](GC::ptr<int> &a)
			{
				auto b = GC::make<int>();
				assert_throws_disjunction(a.swap(b));
			}, std::ref(ptr_a)).join();
			GC::thread(GC::inherit_disjunction, [](GC::ptr<int> &a)
			{
				auto b = GC::make<int>();
				assert_nothrow(a.swap(b));
			}, std::ref(ptr_a)).join();
			GC::thread(GC::new_disjunction, [](GC::ptr<int> &a)
			{
				auto b = GC::make<int>();
				assert_throws_disjunction(a.swap(b));
			}, std::ref(ptr_a)).join();

			// --------------------------------------------------

			std::cerr << "starting swap test b\n";

			GC::thread(GC::primary_disjunction, [](GC::ptr<int> &a)
			{
				auto b = GC::make<int>();
				assert_throws_disjunction(b.swap(a));
			}, std::ref(ptr_a)).join();
			GC::thread(GC::inherit_disjunction, [](GC::ptr<int> &a)
			{
				auto b = GC::make<int>();
				assert_nothrow(b.swap(a));
			}, std::ref(ptr_a)).join();
			GC::thread(GC::new_disjunction, [](GC::ptr<int> &a)
			{
				auto b = GC::make<int>();
				assert_throws_disjunction(b.swap(a));
			}, std::ref(ptr_a)).join();

			// --------------------------------------------------

			std::cerr << "starting ctor alias test\n";

			GC::thread(GC::primary_disjunction, [](GC::ptr<int> &a)
			{
				assert_throws_disjunction(GC::ptr<int> temp(a));
			}, std::ref(ptr_a)).join();
			GC::thread(GC::inherit_disjunction, [](GC::ptr<int> &a)
			{
				assert_nothrow(GC::ptr<int> temp(a));
			}, std::ref(ptr_a)).join();
			GC::thread(GC::new_disjunction, [](GC::ptr<int> &a)
			{
				assert_throws_disjunction(GC::ptr<int> temp(a));
			}, std::ref(ptr_a)).join();

			// --------------------------------------------------

			std::cerr << "starting value to reference thread pass - expecting 3:\n";

			GC::thread(GC::primary_disjunction, [](const GC::ptr<std::string> &a)
			{
				assert_nothrow(print_str(a));
			}, GC::make<std::string>("  -- primary disj")).join();
			GC::thread(GC::inherit_disjunction, [](const GC::ptr<std::string> &a)
			{
				assert_nothrow(print_str(a));
			}, GC::make<std::string>("  -- inherit disj")).join();
			GC::thread(GC::new_disjunction, [](const GC::ptr<std::string> &a)
			{
				assert_nothrow(print_str(a));
			}, GC::make<std::string>("  -- new disj")).join();

			// --------------------------------------------------

			std::cerr << "starting value to value thread pass - expecting 1:\n";

			GC::thread(GC::inherit_disjunction, [](GC::ptr<std::string> a)
			{
				assert_nothrow(print_str(a));
			}, GC::make<std::string>("  -- inherit disj")).join();
		}
		catch (...)
		{
			std::cerr << "\n\nAN EXCEPTION SHOULD NOT HAVE GOTTEN HERE!!\n\n";
			assert(false);
		}
	}).join();

	#endif

	{
		std::cerr << "starting disjunction deletion test\n";
		std::atomic<bool> disjunction_deletion_flag;

		for (int i = 0; i < 16; ++i)
		{
			GC::thread(GC::new_disjunction, [](std::atomic<bool> &flag)
			{
				try
				{
					auto p = GC::make<bool_alerter_self_ptr>(flag);
					p->self_p = p;
				}
				catch (...) { std::cerr << "DISJUNCTION DEL TEST EXCEPTION!!\n"; assert(false); }
			}, std::ref(disjunction_deletion_flag)).join();

			assert(disjunction_deletion_flag);
		}
	}

	// -----------------------------------------------------------------------------------------------------

	static_assert(GC::has_trivial_router<int>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<char>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<double>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<std::string>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<std::pair<int, std::string>>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<std::tuple<int, std::string>>::value, "trivial assumption failure");

	static_assert(GC::has_trivial_router<std::unique_ptr<int>>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<std::unique_ptr<self_ptr>>::value, "trivial assumption failure");

	static_assert(GC::has_trivial_router<int[16]>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<int[16][7]>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<self_ptr[16][7][1]>::value, "trivial assumption failure");

	static_assert(GC::has_trivial_router<std::array<int, 12>>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<std::array<std::array<int, 12>, 12>>::value, "trivial assumption failure");

	static_assert(GC::has_trivial_router<std::vector<int>>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<GC::vector<int>>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<std::unordered_map<int, char*>>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<GC::unordered_map<int, char*>>::value, "trivial assumption failure");

	static_assert(!GC::has_trivial_router<self_ptr>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<GC::ptr<int>>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<GC::ptr<self_ptr>>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<GC::ptr<GC::ptr<self_ptr>>>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<std::pair<int, GC::ptr<int>>>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<std::tuple<int, GC::ptr<int>>>::value, "trivial assumption failure");
	
	// -----------------------------------------------------------------------------------------------------

	static_assert(GC::has_trivial_router<int>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<int&>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<int&&>::value, "trivial assumption failure");

	static_assert(GC::has_trivial_router<const int>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<const int&>::value, "trivial assumption failure");
	static_assert(GC::has_trivial_router<const int&&>::value, "trivial assumption failure");

	static_assert(!GC::has_trivial_router<self_ptr>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<self_ptr&>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<self_ptr&&>::value, "trivial assumption failure");

	static_assert(!GC::has_trivial_router<const self_ptr>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<const self_ptr&>::value, "trivial assumption failure");
	static_assert(!GC::has_trivial_router<const self_ptr&&>::value, "trivial assumption failure");
	
	// -----------------------------------------------------------------------------------------------------

	static_assert(GC::all_have_trivial_routers<>::value, "trivial assumption failure");

	// -----------------------------------------------------------------------------------------------------

	static_assert(std::is_same<GC::vector<bool>, std::vector<bool>>::value, "smart wrapper opt check");
	static_assert(std::is_same<GC::vector<bool_alerter>, std::vector<bool_alerter>>::value, "smart wrapper opt check");
	static_assert(std::is_same<GC::unique_ptr<long double>, std::unique_ptr<long double>>::value, "smart wrapper opt check");
	static_assert(std::is_same<GC::unique_ptr<bool_alerter>, std::unique_ptr<bool_alerter>>::value, "smart wrapper opt check");
	
	static_assert(std::is_same<GC::stack<bool_alerter>, std::stack<bool_alerter>>::value, "smart wrapper opt check");
	static_assert(std::is_same<GC::queue<std::pair<double, int>>, std::queue<std::pair<double, int>>>::value, "smart wrapper opt check");
	static_assert(std::is_same<GC::priority_queue<std::tuple<double, int>>, std::priority_queue<std::tuple<double, int>>>::value, "smart wrapper opt check");

	static_assert(!std::is_same<GC::unique_ptr<TreeNode>, std::unique_ptr<TreeNode>>::value, "smart wrapper opt check");
	static_assert(!std::is_same<GC::list<SymbolTable>, std::list<SymbolTable>>::value, "smart wrapper opt check");

	static_assert(!std::is_same<GC::stack<TreeNode>, std::stack<TreeNode>>::value, "smart wrapper opt check");
	static_assert(!std::is_same<GC::queue<std::pair<double, TreeNode>>, std::queue<std::pair<double, TreeNode>>>::value, "smart wrapper opt check");
	static_assert(!std::is_same<GC::priority_queue<std::tuple<SymbolTable>>, std::priority_queue<std::tuple<SymbolTable>>>::value, "smart wrapper opt check");

	// -----------------------------------------------------------------------------------------------------

	static_assert(std::is_same<const GC::make_wrapped_t<GC::vector<int>>, GC::make_wrapped_t<const std::vector<int>>>::value, "wrapped const test");
	static_assert(std::is_same<const GC::make_wrapped_t<std::vector<TreeNode>>, GC::make_wrapped_t<const std::vector<TreeNode>>>::value, "wrapped const test");
	static_assert(std::is_same<volatile GC::make_wrapped_t<std::vector<TreeNode>>, GC::make_wrapped_t<volatile GC::vector<TreeNode>>>::value, "wrapped const test");
	static_assert(std::is_same<const volatile GC::make_wrapped_t<std::vector<TreeNode>>, const GC::make_wrapped_t<volatile std::vector<TreeNode>>>::value, "wrapped const test");
	static_assert(std::is_same<const GC::make_wrapped_t<volatile std::vector<TreeNode>>, const volatile GC::make_wrapped_t<volatile GC::vector<TreeNode>>>::value, "wrapped const test");
	static_assert(std::is_same<const GC::make_wrapped_t<const volatile std::vector<TreeNode>>, const volatile GC::make_wrapped_t<const volatile std::vector<TreeNode>>>::value, "wrapped const test");
	static_assert(std::is_same<GC::make_wrapped_t<const volatile GC::vector<TreeNode>>, GC::make_wrapped_t<const volatile std::vector<TreeNode>>>::value, "wrapped const test");

	// -----------------------------------------------------------------------------------------------------

	static_assert(std::is_same<int, GC::make_unwrapped_t<int>>::value, "unwrapped primitive test");
	static_assert(std::is_same<const int, GC::make_unwrapped_t<const int>>::value, "unwrapped primitive test");
	static_assert(std::is_same<volatile int, GC::make_unwrapped_t<volatile int>>::value, "unwrapped primitive test");
	static_assert(std::is_same<const volatile int, GC::make_unwrapped_t<const volatile int>>::value, "unwrapped primitive test");
	
	static_assert(std::is_same<int, GC::make_wrapped_t<int>>::value, "wrapped primitive test");
	static_assert(std::is_same<const int, GC::make_wrapped_t<const int>>::value, "wrapped primitive test");
	static_assert(std::is_same<volatile int, GC::make_wrapped_t<volatile int>>::value, "wrapped primitive test");
	static_assert(std::is_same<const volatile int, GC::make_wrapped_t<const volatile int>>::value, "wrapped primitive test");

	static_assert(std::is_same<float, GC::make_unwrapped_t<float>>::value, "unwrapped primitive test");
	static_assert(std::is_same<const float, GC::make_unwrapped_t<const float>>::value, "unwrapped primitive test");
	static_assert(std::is_same<volatile float, GC::make_unwrapped_t<volatile float>>::value, "unwrapped primitive test");
	static_assert(std::is_same<const volatile float, GC::make_unwrapped_t<const volatile float>>::value, "unwrapped primitive test");

	static_assert(std::is_same<bool, GC::make_wrapped_t<bool>>::value, "wrapped primitive test");
	static_assert(std::is_same<const bool, GC::make_wrapped_t<const bool>>::value, "wrapped primitive test");
	static_assert(std::is_same<volatile bool, GC::make_wrapped_t<volatile bool>>::value, "wrapped primitive test");
	static_assert(std::is_same<const volatile bool, GC::make_wrapped_t<const volatile bool>>::value, "wrapped primitive test");

	static_assert(std::is_same<std::pair<float, std::string>, GC::make_unwrapped_t<std::pair<float, std::string>>>::value, "unwrapped trivial test");
	static_assert(std::is_same<const std::pair<float, std::string>, GC::make_unwrapped_t<const std::pair<float, std::string>>>::value, "unwrapped trivial test");
	static_assert(std::is_same<volatile std::pair<float, std::string>, GC::make_unwrapped_t<volatile std::pair<float, std::string>>>::value, "unwrapped trivial test");
	static_assert(std::is_same<const volatile std::pair<float, std::string>, GC::make_unwrapped_t<const volatile std::pair<float, std::string>>>::value, "unwrapped trivial test");

	static_assert(std::is_same<std::tuple<void*, const char*, float, float, double>, GC::make_wrapped_t<std::tuple<void*, const char*, float, float, double>>>::value, "wrapped trivial test");
	static_assert(std::is_same<const std::tuple<void*, const char*, float, float, double>, GC::make_wrapped_t<const std::tuple<void*, const char*, float, float, double>>>::value, "wrapped trivial test");
	static_assert(std::is_same<volatile std::tuple<void*, const char*, float, float, double>, GC::make_wrapped_t<volatile std::tuple<void*, const char*, float, float, double>>>::value, "wrapped trivial test");
	static_assert(std::is_same<const volatile std::tuple<void*, const char*, float, float, double>, GC::make_wrapped_t<const volatile std::tuple<void*, const char*, float, float, double>>>::value, "wrapped trivial test");

	// -----------------------------------------------------------------------------------------------------

	static_assert(GC::has_trivial_router<GC::variant<int>>::value, "trivial test");
	static_assert(GC::has_trivial_router<GC::variant<int, float>>::value, "trivial test");
	static_assert(GC::has_trivial_router<GC::variant<int, float, double>>::value, "trivial test");
	static_assert(GC::has_trivial_router<GC::variant<int, float, double, void*>>::value, "trivial test");

	static_assert(!GC::has_trivial_router<SymbolTable>::value, "trivial test");
	static_assert(!GC::has_trivial_router<std::variant<SymbolTable>>::value, "trivial test");
	static_assert(!GC::has_trivial_router<std::variant<int, SymbolTable>>::value, "trivial test");
	static_assert(!GC::has_trivial_router<std::variant<SymbolTable, int>>::value, "trivial test");
	static_assert(!GC::has_trivial_router<std::variant<int, SymbolTable, float>>::value, "trivial test");
	static_assert(!GC::has_trivial_router<std::variant<int, double, SymbolTable, void*>>::value, "trivial test");
	static_assert(!GC::has_trivial_router<std::variant<int, char, volatile int*, SymbolTable, long>>::value, "trivial test");

	static_assert(std::is_same<std::variant<int, double>, GC::make_wrapped_t<std::variant<int, double>>>::value, "wrapped variant equivalence");
	static_assert(std::is_same<std::variant<int, double>, GC::make_unwrapped_t<std::variant<int, double>>>::value, "wrapped variant equivalence");
	static_assert(!std::is_same<std::variant<int, SymbolTable>, GC::make_wrapped_t<std::variant<int, SymbolTable>>>::value, "wrapped variant equivalence");
	static_assert(std::is_same<std::variant<SymbolTable, std::string>, GC::make_unwrapped_t<GC::variant<SymbolTable, std::string>>>::value, "wrapped variant equivalence");

	static_assert(std::is_same<std::variant<int, double>, GC::variant<int, double>>::value, "wrapped variant equivalence");
	static_assert(!std::is_same<std::variant<SymbolTable>, GC::variant<SymbolTable>>::value, "wrapped variant equivalence");
	static_assert(!std::is_same<std::variant<std::string, SymbolTable>, GC::variant<std::string, SymbolTable>>::value, "wrapped variant equivalence");

	// -----------------------------------------------------------------------------------------------------

	static_assert(GC::has_trivial_router<GC::optional<int>>::value, "trivial test");
	static_assert(GC::has_trivial_router<GC::optional<float>>::value, "trivial test");
	static_assert(GC::has_trivial_router<GC::optional<sse_t>>::value, "trivial test");
	static_assert(GC::has_trivial_router<GC::optional<SymbolTable*>>::value, "trivial test");
	static_assert(GC::has_trivial_router<GC::optional<const SymbolTable*>>::value, "trivial test");

	static_assert(!GC::has_trivial_router<SymbolTable>::value, "trivial test");
	static_assert(!GC::has_trivial_router<std::optional<SymbolTable>>::value, "trivial test");
	static_assert(!GC::has_trivial_router<std::optional<TreeNode>>::value, "trivial test");

	static_assert(std::is_same<std::optional<int>, GC::optional<int>>::value, "wrapped optional equivalence");
	static_assert(std::is_same<std::optional<double>, GC::optional<double>>::value, "wrapped optional equivalence");
	static_assert(std::is_same<std::optional<std::string>, GC::optional<std::string>>::value, "wrapped optional equivalence");
	static_assert(!std::is_same<std::optional<SymbolTable>, GC::optional<SymbolTable>>::value, "wrapped optional equivalence");

	// -----------------------------------------------------------------------------------------------------

	std::cerr << "\n-------- ctors --------\n";
	{
		GC::ptr<self_ptr> sp = GC::make<self_ptr>();
		GC::ptr<gc_self_ptr> gcsp = GC::make<gc_self_ptr>();

		sp->p = std::make_unique<decltype(sp)>(sp);
		gcsp->p = std::make_unique<decltype(gcsp)>(gcsp);
	}
	std::cerr << "-------- collect 1 --------\n";
	GC::collect();
	std::cerr << "-------- collect 2 - SHOULD BE EMPTY --------\n";
	GC::collect();
	std::cerr << "-------- end --------\n\n";


	#if DRAGAZO_GARBAGE_COLLECT_EXTRA_UND_CHECKS
	{
		derived *d = new derived;
		base1 *b1 = new base1;
		base2 *b2 = new base2;

		assert_nothrow(GC::adopt(d));
		assert_nothrow(GC::adopt(b1));
		assert_nothrow(GC::adopt(b2));

		base1 *d_b1 = new derived;
		base2 *d_b2 = new derived;

		assert_throws(GC::adopt(d_b1), std::invalid_argument);
		assert_throws(GC::adopt(d_b2), std::invalid_argument);
	}
	#endif


	GC::ptr<stationary<int>> stationary_test = GC::make<stationary<int>>(65);
	stationary_test->value = 75;


	GC::ptr<atomic_container> atomic_container_obj = GC::make<atomic_container>();

	atomic_container_obj->atomic_1 = GC::make<double>(1.1);
	atomic_container_obj->atomic_2 = GC::make<double>(2.2);
	atomic_container_obj->atomic_3 = GC::make<double>(3.3);
	atomic_container_obj->atomic_4 = GC::make<double>(4.4);

	GC::collect();

	global_atomic_ptr = nullptr;
	for (int i = 0; i < 8; ++i) global_vec_ptr.emplace_back();

	for (int i = 0; i < 8; ++i) thread_local_vec_ptr.emplace_back();

	GC::thread(GC::new_disjunction, []
	{
		for (int i = 0; i < 8; ++i) thread_local_vec_ptr.emplace_back();
	}).detach();

	GC::ptr<int[16]> arr_ptr_new = GC::make<int[16]>();
	assert(arr_ptr_new != nullptr);

	GC::collect();

	std::cerr << "------ SHOULD BE SELF-CONTAINED ------\n";
	{
		GC::ptr<alert_t> holder;

		{
			auto arr = GC::make<alert_t[]>(2);
			holder = arr.alias(2);
		}
		std::cerr << "destroyed array - element dtors should follow:\n";
	}
	std::cerr << "----------------- end ----------------\n\n";

	static_assert(std::is_same<int(*)[6], decltype(std::declval<GC::ptr<int[6]>>().get())>::value, "ptr type error");
	static_assert(std::is_same<int(*)[6][8], decltype(std::declval<GC::ptr<int[6][8]>>().get())>::value, "ptr type error");
	static_assert(std::is_same<int(*)[8], decltype(std::declval<GC::ptr<int[][8]>>().get())>::value, "ptr type error");
	static_assert(std::is_same<int(*)[6][8], decltype(std::declval<GC::ptr<int[][6][8]>>().get())>::value, "ptr type error");


	GC::ptr<int[6]> arr_test_0 = GC::make<int[6]>();
	GC::ptr<int[5][6]> arr_test_1 = GC::make<int[5][6]>();
	GC::ptr<int[][6]> arr_test_2 = GC::make<int[][6]>(12);
	GC::ptr<int[][5][6]> arr_test_3 = GC::make<int[][5][6]>(14);
	GC::ptr<int[]> arr_test_4 = GC::make<int[]>(16);

	GC::ptr<virtual_type> arr_test_5;

	GC::ptr<int> arr_sub_0_0 = GC::alias(*arr_test_0 + 4, arr_test_0);

	assert(arr_sub_0_0 != nullptr);

	GC::ptr<int[6]> arr_sub_1_0 = GC::alias(*arr_test_1 + 2, arr_test_0);
	GC::ptr<int[6]> arr_sub_1_1 = GC::alias(&(*arr_test_1)[2], arr_test_0);

	assert(arr_sub_1_0 != nullptr);
	assert(arr_sub_1_0 == arr_sub_1_1);

	GC::ptr<int> arr_sub_1_2 = GC::alias(*(*arr_test_1 + 2) + 3, arr_test_1);
	GC::ptr<int> arr_sub_1_3 = GC::alias((*arr_test_1)[2] + 3, arr_test_1);
	GC::ptr<int> arr_sub_1_4 = GC::alias(&(*arr_test_1)[2][3], arr_test_1);
	
	assert(arr_sub_1_2 != nullptr);
	assert(arr_sub_1_2 == arr_sub_1_3 && arr_sub_1_3 == arr_sub_1_4);

	GC::ptr<int> sc_test_0;

	GC::reinterpretCast<int[]>(arr_test_0);
	GC::reinterpretCast<double[]>(arr_test_0);
	GC::constCast<const int[6]>(arr_test_0);
	GC::constCast<const int[]>(arr_test_4);
	GC::staticCast<const int[]>(arr_test_4);

	GC::staticCast<const int>(sc_test_0);

	assert(std::is_same<decltype(arr_test_0.get()) COMMA int(*)[6]>::value);
	assert(std::is_same<decltype(arr_test_1.get()) COMMA int(*)[5][6]>::value);
	assert(std::is_same<decltype(arr_test_2.get()) COMMA int(*)[6]>::value);
	assert(std::is_same<decltype(arr_test_3.get()) COMMA int(*)[5][6]>::value);
	assert(std::is_same<decltype(arr_test_4.get()) COMMA int(*)>::value);


	GC::ptr<std::stack<int>> stack_ptr = GC::make<std::stack<int>>();
	GC::ptr<std::queue<int>> queue_ptr = GC::make<std::queue<int>>();
	GC::ptr<std::priority_queue<int>> priority_queue_ptr = GC::make<std::priority_queue<int>>();
	
	assert(stack_ptr == stack_ptr.get());
	assert(queue_ptr == queue_ptr.get());
	assert(priority_queue_ptr == priority_queue_ptr.get());

	for (int i = 0; i < 8; ++i)
	{
		stack_ptr->push(i);
		queue_ptr->push(i);
		priority_queue_ptr->push(i);
	}

	GC::unique_ptr<int> gc_uint(new int(77));

	GC::ptr<int[]> non_const_arr = GC::make<int[]>(16);
	GC::ptr<const int[]> const_arr = non_const_arr;

	assert(GC::constCast<const int[]>(non_const_arr) == const_arr);
	assert(GC::constCast<int[]>(const_arr) == non_const_arr);

	assert(gc_uint != nullptr);

	GC::ptr<void> void_p_test_1 = non_const_arr;
	GC::ptr<const void> void_p_test_2 = non_const_arr;
	GC::ptr<volatile void> void_p_test_3 = non_const_arr;
	GC::ptr<const volatile void> void_p_test_4 = non_const_arr;

	static_assert(std::is_same<GC::ptr<void>::element_type, void>::value, "ptr<void> elem type wrong");
	static_assert(std::is_same<decltype(void_p_test_1.get()), void*>::value, "ptr<void> get type wrong");
	assert(void_p_test_1.get() == non_const_arr.get());

	assert(GC::reinterpretCast<int[]>(void_p_test_1) == non_const_arr);

	GC::ptr<const void> void_p_test_5 = const_arr;
	GC::ptr<const volatile void> void_p_test_6 = const_arr;

	GC::vector<int> gc_vec;
	gc_vec.emplace_back(17);

	assert(gc_vec == gc_vec);

	std::allocator<int> int_alloc;

	GC::vector<int> gc_vec2(std::move(gc_vec), int_alloc);

	GC::vector<float> gc_float_vec = { 1, 2, 3, 4 };
	GC::deque<float> gc_float_deq = { 1, 2, 3, 4 };

	gc_float_deq.push_front(178);
	gc_vec2.pop_back();

	GC::ptr<GC::unique_ptr<int>> ptr_unique_ptr = GC::make<GC::unique_ptr<int>>(new int(69));
	**ptr_unique_ptr = 36;
	*ptr_unique_ptr = std::make_unique<int>(345);

	int *pint = ptr_unique_ptr.get()->get();
	*pint = 123;

	assert(**ptr_unique_ptr == 123);

	auto gc_flist = new GC::forward_list<float>(gc_float_deq.begin(), gc_float_deq.end());

	GC::ptr<GC::forward_list<float>> ptr_gc_flist = GC::adopt<GC::forward_list<float>>(gc_flist);

	ptr_gc_flist->push_front(111222333.444f);

	GC::ptr<GC::list<float>> ptr_gc_list = GC::make<GC::list<float>>();
	ptr_gc_list->emplace_front(17.f);
	ptr_gc_list->push_back(12.f);

	GC::ptr<GC::set<float>> pset = GC::make<GC::set<float>>(gc_float_vec.begin(), gc_float_vec.end());

	pset->insert(3.14159f);

	GC::ptr<GC::unordered_set<float>> puset = GC::make<GC::unordered_set<float>>(pset->begin(), pset->end());

	puset->insert(2.71828f);

	GC::ptr<GC::multiset<float>> pmset = GC::make<GC::multiset<float>>(puset->begin(), puset->end());

	pmset->insert(std::sqrt(2.0f));

	GC::ptr<GC::unordered_multiset<float>> pumset = GC::make<GC::unordered_multiset<float>>(pmset->begin(), pmset->end());

	pumset->insert(std::sqrt(10.0f));

	{ // -- variant tests -- //

		std::variant<int, float, SymbolTable> std_variant = 5.6f;
		GC::variant<int, float, SymbolTable> gc_variant(12);
		std_variant = 6;

		auto std_variant_2 = std_variant;
		auto std_variant_3 = std::move(std_variant);

		gc_variant = std_variant;
		gc_variant = std::move(std_variant);

		std_variant = 5;
		gc_variant = std_variant;

		assert(std::get<0>(std_variant) == 5);
		assert(std::get<0>(gc_variant) == 5);

		assert(std::get<int>(std_variant) == 5);
		assert(std::get<int>(gc_variant) == 5);

		assert(gc_variant.get<0>() == 5);
		assert(gc_variant.get<int>() == 5);

		assert(std::get_if<0>(&gc_variant) != nullptr);
		assert(*std::get_if<0>(&gc_variant) == 5);
		assert(std::get_if<1>(&gc_variant) == nullptr);
		assert(std::get_if<2>(&gc_variant) == nullptr);

		assert(std::get_if<int>(&gc_variant) != nullptr);
		assert(*std::get_if<int>(&gc_variant) == 5);
		assert(std::get_if<float>(&gc_variant) == nullptr);
		assert(std::get_if<SymbolTable>(&gc_variant) == nullptr);

		std::get<int>(gc_variant) = std::get<0>(gc_variant);
		std::get<0>(std_variant) = std::get<int>(std::move(gc_variant));
		std::get<0>(gc_variant) = std::get<int>(std::move(std_variant));

		assert(std::get_if<0>((decltype(std_variant)*)nullptr) == nullptr);
		assert(std::get_if<int>((decltype(std_variant)*)nullptr) == nullptr);

		assert(std::get_if<0>((decltype(gc_variant)*)nullptr) == nullptr);
		assert(std::get_if<int>((decltype(gc_variant)*)nullptr) == nullptr);

		assert(std::holds_alternative<int>(std_variant));
		assert(std::holds_alternative<int>(gc_variant));

		assert(!std::holds_alternative<float>(std_variant));
		assert(!std::holds_alternative<float>(gc_variant));

		assert(!std::holds_alternative<SymbolTable>(std_variant));
		assert(!std::holds_alternative<SymbolTable>(gc_variant));

		static_assert(!GC::has_trivial_router<gc_t>::value, "trivial router error");
		static_assert(std::is_same<std::variant<int, int>, GC::variant<int, int>>::value, "wrapped variant error");
		static_assert(!std::is_same<std::variant<int, int, gc_t>, GC::variant<int, int, gc_t>>::value, "wrapped variant error");

		GC::variant<int, int, gc_t> var_cmp_1(std::in_place_index<0>, 42);
		GC::variant<int, int, gc_t> var_cmp_2(std::in_place_index<1>, 42);

		assert(var_cmp_1.index() == 0);
		assert(var_cmp_2.index() == 1);

		assert(std::get<0>(var_cmp_1) == 42);
		assert(std::get<1>(var_cmp_2) == 42);

		assert(var_cmp_1 != var_cmp_2);
		assert(var_cmp_1 < var_cmp_2);
		assert(var_cmp_1 <= var_cmp_2);

		assert(var_cmp_2 != var_cmp_1);
		assert(var_cmp_2 > var_cmp_1);
		assert(var_cmp_2 >= var_cmp_1);

		static_assert(std::variant_size_v<std::variant<int, float, SymbolTable>> == 3, "variant size error");
		static_assert(std::variant_size_v<const std::variant<int, float, SymbolTable>> == 3, "variant size error");
		static_assert(std::variant_size_v<volatile std::variant<int, float, SymbolTable>> == 3, "variant size error");
		static_assert(std::variant_size_v<const volatile std::variant<int, float, SymbolTable>> == 3, "variant size error");

		static_assert(std::variant_size_v<GC::variant<int, float, SymbolTable>> == 3, "variant size error");
		static_assert(std::variant_size_v<const GC::variant<int, float, SymbolTable>> == 3, "variant size error");
		static_assert(std::variant_size_v<volatile GC::variant<int, float, SymbolTable>> == 3, "variant size error");
		static_assert(std::variant_size_v<const volatile GC::variant<int, float, SymbolTable>> == 3, "variant size error");

		static_assert(std::is_same<int, std::variant_alternative_t<0, GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");
		static_assert(std::is_same<float, std::variant_alternative_t<1, GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");
		static_assert(std::is_same<SymbolTable, std::variant_alternative_t<2, GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");

		static_assert(std::is_same<const int, std::variant_alternative_t<0, const GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");
		static_assert(std::is_same<const float, std::variant_alternative_t<1, const GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");
		static_assert(std::is_same<const SymbolTable, std::variant_alternative_t<2, const GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");

		static_assert(std::is_same<volatile int, std::variant_alternative_t<0, volatile GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");
		static_assert(std::is_same<volatile float, std::variant_alternative_t<1, volatile GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");
		static_assert(std::is_same<volatile SymbolTable, std::variant_alternative_t<2, volatile GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");

		static_assert(std::is_same<const volatile int, std::variant_alternative_t<0, const volatile GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");
		static_assert(std::is_same<const volatile float, std::variant_alternative_t<1, const volatile GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");
		static_assert(std::is_same<const volatile SymbolTable, std::variant_alternative_t<2, const volatile GC::variant<int, float, SymbolTable>>>::value, "variant alternative error");

		__gc_variant<std::mutex, int, float, void*> mgf;
		std::hash<decltype(mgf)> variant_hasher;
		variant_hasher(mgf);

		const decltype(std_variant) &gc_variant_ref = gc_variant;
		assert(std::holds_alternative<int>(gc_variant_ref));

		auto dyn_gc_variant = GC::make<GC::variant<SymbolTable>>();
		assert(dyn_gc_variant->index() == 0);
		*dyn_gc_variant = *dyn_gc_variant;
		*dyn_gc_variant = std::move(*dyn_gc_variant);
	}

	{ // -- optional tests -- //
		auto sym_1 = GC::make<GC::optional<SymbolTable>>();
		auto sym_2 = GC::make<std::optional<SymbolTable>>();

		assert(!sym_1->has_value());
		assert(!sym_2->has_value());

		sym_1->emplace();
		sym_2->emplace();

		assert(sym_1->has_value());
		assert(sym_2->has_value());

		sym_1->reset();
		sym_2->reset();

		assert(!sym_1->has_value());
		assert(!sym_2->has_value());

		auto sym_3 = GC::make<GC::optional<int>>(16);
		auto sym_4 = GC::make<std::optional<int>>(32);

		assert(sym_3->has_value());
		assert(sym_4->has_value());

		assert(sym_3->value() == 16);
		assert(sym_4->value() == 32);

		assert(*sym_3 < *sym_4);
		assert(*sym_3 <= *sym_4);
		assert(*sym_4 > *sym_3);
		assert(*sym_4 >= *sym_3);
		assert(*sym_4 != *sym_3);
		assert(!(*sym_4 == *sym_3));

		*sym_3 = 5;
		*sym_4 = 5;

		assert(sym_3->has_value());
		assert(sym_4->has_value());

		assert(sym_3->value() == 5);
		assert(sym_4->value() == 5);

		assert(!(*sym_3 < *sym_4));
		assert(*sym_3 <= *sym_4);
		assert(!(*sym_4 > *sym_3));
		assert(*sym_4 >= *sym_3);
		assert(*sym_4 == *sym_3);
		assert(!(*sym_4 != *sym_3));

		*sym_3 = std::nullopt;
		*sym_4 = std::nullopt;

		assert(!sym_1->has_value());
		assert(!sym_2->has_value());
	}

	GC::ptr<GC::map<int, std::string>> pmap = GC::make<GC::map<int, std::string>>();
	pmap->emplace(0, "zero");
	pmap->emplace(1, "one");
	pmap->emplace(2, "two");
	pmap->emplace(3, "three");
	pmap->emplace(4, "four");
	pmap->emplace(5, "five");
	pmap->emplace(6, "six");
	pmap->emplace(7, "seven");
	pmap->emplace(8, "eight");
	pmap->emplace(8, "eight-repeat");
	pmap->emplace(9, "nine");
	pmap->emplace(10, "ten");

	GC::ptr<GC::multimap<int, std::string>> pmmap = GC::make<GC::multimap<int, std::string>>();
	pmmap->emplace(0, "zero");
	pmmap->emplace(1, "one");
	pmmap->emplace(2, "two");
	pmmap->emplace(3, "three");
	pmmap->emplace(4, "four");
	pmmap->emplace(5, "five");
	pmmap->emplace(6, "six");
	pmmap->emplace(7, "seven");
	pmmap->emplace(8, "eight");
	pmmap->emplace(8, "eight-repeat");
	pmmap->emplace(9, "nine");
	pmmap->emplace(10, "ten");

	GC::ptr<GC::unordered_map<int, std::string>> pumap = GC::make<GC::unordered_map<int, std::string>>();
	pumap->emplace(0, "zero");
	pumap->emplace(1, "one");
	pumap->emplace(2, "two");
	pumap->emplace(3, "three");
	pumap->emplace(4, "four");
	pumap->emplace(5, "five");
	pumap->emplace(6, "six");
	pumap->emplace(7, "seven");
	pumap->emplace(8, "eight");
	pumap->emplace(8, "eight-repeat");
	pumap->emplace(9, "nine");
	pumap->emplace(10, "ten");

	GC::ptr<GC::unordered_multimap<int, std::string>> pummap = GC::make<GC::unordered_multimap<int, std::string>>();
	pummap->emplace(0, "zero");
	pummap->emplace(1, "one");
	pummap->emplace(2, "two");
	pummap->emplace(3, "three");
	pummap->emplace(4, "four");
	pummap->emplace(5, "five");
	pummap->emplace(6, "six");
	pummap->emplace(7, "seven");
	pummap->emplace(8, "eight");
	pummap->emplace(8, "eight-repeat");
	pummap->emplace(9, "nine");
	pummap->emplace(10, "ten");

	{
		auto i1 = GC::make<TreeNode>();
		auto i2 = GC::make<SymbolTable>();
		auto i3 = GC::make<MaybeTreeNode>();

		assert(i1 && i2 && i3);

		i1->left = i1;

		GC::collect();

		GC::ptr<const TreeNode> ci1 = i1;
		GC::ptr<const SymbolTable> ci2 = i2;
		GC::ptr<const MaybeTreeNode> ci3 = i3;

		GC::ptr<const TreeNode> _ci1 = GC::constCast<const TreeNode>(i1);
		GC::ptr<const SymbolTable> _ci2 = GC::constCast<const SymbolTable>(i2);
		GC::ptr<const MaybeTreeNode> _ci3 = GC::constCast<const MaybeTreeNode>(i3);

		GC::ptr<TreeNode> nci1 = GC::constCast<TreeNode>(ci1);
		GC::ptr<SymbolTable> nci2 = GC::constCast<SymbolTable>(ci2);
		GC::ptr<MaybeTreeNode> nci3 = GC::constCast<MaybeTreeNode>(ci3);
	}
	GC::collect();

	GC::ptr<std::unordered_multimap<int, GC::ptr<ListNode>>> merp = GC::make<std::unordered_multimap<int, GC::ptr<ListNode>>>();

	GC::ptr<ListNode> node_no_val;

	GC::ptr<ptr_set> ptr_set_test = GC::make<ptr_set>();
	GC::ptr<ListNode> node_test = GC::make<ListNode>();

	merp->emplace(1, GC::make<ListNode>());
	merp->emplace(2, GC::make<ListNode>());
	merp->emplace(3, GC::make<ListNode>());
	merp->emplace(4, GC::make<ListNode>());

	GC::ptr<int> not_arr = GC::make<int>();
	GC::ptr<int[][16][16][16]> yes_arr = GC::make<int[][16][16][16]>(16);

	(**yes_arr)[1][5] = 7;
	yes_arr[2][6][1][5] = 7;
	****yes_arr = 7;

	auto bound_array = GC::make<int[5]>();
	auto unbound_array = GC::make<int[]>(5);

	(*bound_array)[0] = 12;
	unbound_array[0] = 12;

	auto v_test1 = std::make_unique<std::vector<int>>(16);
	auto v_test2 = std::make_shared<std::vector<int>>(16);
	auto v_test3 = GC::make<std::vector<int>>(16);

	assert(v_test1->size() == 16);
	assert(v_test2->size() == 16);
	assert(v_test3->size() == 16);

	GC::atomic_ptr<float> atomic_test_0;
	GC::atomic_ptr<float> atomic_test_1;

	atomic_test_0 = GC::make<float>(2.718f);
	atomic_test_1 = GC::make<float>(3.141f);

	using std::swap;
	atomic_test_0.swap(atomic_test_1);
	swap(atomic_test_0, atomic_test_1);

	GC::collect();

	GC::ptr<int> arr_of_ptr[16];

	assert(std::is_default_constructible<GC::ptr<int>>::value);

	{
		GC::ptr<GC::ptr<int>[][2][2]> arr_ptr = GC::make<GC::ptr<int>[][2][2]>(2);

		for (int i = 0; i < 2; ++i)
			for (int j = 0; j < 2; ++j)
				for (int k = 0; k < 2; ++k)
					arr_ptr[i][j][k] = GC::make<int>((i + j)*j + i * j*k + k * i + k + 1);

		for (int i = 0; i < 2; ++i)
			for (int j = 0; j < 2; ++j)
				for (int k = 0; k < 2; ++k)
				{
					assert(*arr_ptr[i][j][k] == (i + j)*j + i * j*k + k * i + k + 1);
				}

		GC::collect();
	}

	auto sse_t_obj = GC::make<sse_t>();

	GC::ptr<int> ip = GC::make<int>(46);
	GC::ptr<int> ip_self = ip;

	GC::ptr<const int> const_ip = GC::make<const int>(47);

	GC::ptr<const int> const_2 = ip;
	GC::ptr<const int> const_self = const_2;

	GC::ptr<wrap<wrap<int>>> p = GC::make<wrap<wrap<int>>>();
	p->ptr = GC::make<wrap<int>>();
	p->ptr->ptr = GC::make<int>();

	ListNode n;

	GC::ptr<double> _null_ctor_test1{};
	GC::ptr<double> null_ctor_test1;
	GC::ptr<double> _null_ctor_test2(nullptr);
	GC::ptr<double> null_ctor_test2 = nullptr;

	std::shared_ptr<double> _otest1;
	std::shared_ptr<double> otest1 = nullptr;
	std::unique_ptr<double> _otest2;
	std::unique_ptr<double> otest2 = nullptr;

	GC::ptr<derived> dp = GC::make<derived>();
	GC::ptr<base1> bp1 = GC::make<base1>();
	GC::ptr<base2> bp2 = GC::make<base2>();

	assert(dp && bp1 && bp2);

	auto interp_1 = GC::reinterpretCast<non_virtual_type>(dp);
	auto interp_2 = GC::reinterpretCast<base1>(dp);
	auto interp_3 = GC::reinterpretCast<derived>(dp);
	auto interp_4 = GC::reinterpretCast<int>(dp);
	auto interp_5 = GC::reinterpretCast<sse_t>(dp);

	assert(interp_1.get() == (void*)dp.get());
	assert(interp_2.get() == (void*)dp.get());
	assert(interp_3.get() == (void*)dp.get());
	assert(interp_4.get() == (void*)dp.get());
	assert(interp_5.get() == (void*)dp.get());

	auto dyn_cast1 = GC::dynamicCast<virtual_type>(bp1);
	auto dyn_cast2 = GC::dynamicCast<non_virtual_type>(bp1);

	assert(dyn_cast1 == nullptr);
	assert(dyn_cast2 == nullptr);

	dp->a = 123;
	dp->b = 456;

	GC::ptr<base1> dp_as_b1 = dp;
	GC::ptr<base2> dp_as_b2 = dp;

	GC::ptr<base1> _dp_as_b1 = GC::staticCast<base1>(dp);
	GC::ptr<base2> _dp_as_b2 = GC::staticCast<base2>(dp);

	assert(dp_as_b1 == _dp_as_b1);
	assert(dp_as_b2 == _dp_as_b2);

	assert(dynamic_cast<derived*>(dp_as_b1.get()) == dp.get());

	GC::ptr<derived> back_to_derived_1 = GC::dynamicCast<derived>(dp_as_b1);
	GC::ptr<derived> back_to_derived_2 = GC::dynamicCast<derived>(dp_as_b2);

	assert(back_to_derived_1 == back_to_derived_2);
	assert(back_to_derived_1 == dp);

	assert(dp_as_b1->a == dp->a);
	assert(dp_as_b2->b == dp->b);

	std::unique_ptr<int[]> raw_up(new int[16]);
	GC::unique_ptr<int[]> gc_up(new int[16]);

	raw_up.reset();
	gc_up.reset();

	raw_up.reset(new int[16]);
	gc_up.reset(new int[16]);

	{
		std::thread t1([]()
		{
			try
			{
				GC::ptr<SymbolTable> table = GC::make<SymbolTable>();

				for (int pass = 0; pass < 1024; ++pass)
				{
					for (int i = 0; i < 128; ++i)
					{
						GC::ptr<TreeNode> tree = GC::make<TreeNode>();
						tree->left = GC::make<TreeNode>();
						tree->right = GC::make<TreeNode>();

						std::string key = tostr(i);

						table->update(key, tree); // the one with explicit locks
						table->better_symbols[key] = tree; // the one with implicit locks (wrapper type)
					}
					table->clear();
				}

				GC::ptr<int> pi;

				for (int i = 0; i < 8192; ++i)
				{
					GC::ptr<int> temp = GC::make<int>(i);
					pi = temp;
				}
			}
			catch (...) { std::cerr << "\n\nSYMTAB TEST EXCEPTION!!\n\n"; assert(false); }
		});

		GC::collect();

		t1.join();
	}

	try
	{
		std::cerr << "beginning ctor collect vec\n";
		auto ctor_vec = GC::make<GC::vector<ctor_collect_t>>();
		ctor_vec->reserve(20);
		for (int i = 0; i < 10; ++i) ctor_vec->emplace_back();

		std::cerr << "beginning dtor collect vec\n";
		auto dtor_vec = GC::make<GC::vector<dtor_collect_t>>((std::size_t)10);
		while (!dtor_vec->empty()) dtor_vec->pop_back();

		std::cerr << "beginning ctor/dtor collect vec\n";
		auto ctor_dtor_vec = GC::make<GC::vector<ctor_dtor_collect_t>>();
		ctor_dtor_vec->reserve(20);
		for (int i = 0; i < 10; ++i) ctor_dtor_vec->emplace_back();
		std::cerr << "beginning cleanup\n";
		while (!ctor_dtor_vec->empty()) ctor_dtor_vec->pop_back();

		std::cerr << "completed ctor/dtor deadlock tests\n\n";
	}
	catch (const std::exception &ex)
	{
		std::cerr << "CTOR/DTOR VEC TEST EXCEPTION!!\n" << ex.what() << "\n\n";
		assert(false);
	}

	GC::collect();

	{
		TIMER_BEGIN();
		
		GC::thread threads[4];

		//for (auto &i : threads) i = GC::thread(GC::primary_disjunction, []()
		//for (auto &i : threads) i = GC::thread(GC::inherit_disjunction, []()
		for (auto &i : threads) i = GC::thread(GC::new_disjunction, []()
		{
			try
			{
				GC::ptr<std::size_t> x, y, z, w;

				for (std::size_t i = 0; i < 1000000; ++i)
				{
					// make 3 unique objects
					x = GC::make<std::size_t>(i + 0);
					y = GC::make<std::size_t>(i + 1);
					z = GC::make<std::size_t>(i + 2);

					assert(*x == i + 0);
					assert(*y == i + 1);
					assert(*z == i + 2);

					// rotate them 3 times
					for (std::size_t j = 1; j <= 3; ++j)
					{
						w = x; x = y; y = z; z = w;
						assert(*x == i + ((0 + j) % 3));
						assert(*y == i + ((1 + j) % 3));
						assert(*z == i + ((2 + j) % 3));
					}
				}
			}
			catch (...) { std::cerr << "\n\nINTERFERENCE EXCEPTION!!\n\n"; assert(false); }

			sync_err_print("finished interference thread with no errors\n");
		});

		for (auto &i : threads) i.join();

		TIMER_END(milliseconds, "interference test");
	}

	TIMER_END(milliseconds, "all tests");

	return 0;
}
catch (const std::exception &ex)
{
	std::cerr << "\nEXCEPTION!!\n\n" << ex.what() << "\n\n\n";
	assert(false);
}
