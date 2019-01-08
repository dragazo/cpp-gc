#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstddef>
#include <cassert>
#include <sstream>
#include <utility>
#include <cmath>
#include <initializer_list>

#include "GarbageCollection.h"

struct alignas(16) sse_t { char d[16]; };

struct ptr_set
{
	GC::ptr<int> a, b, c, d, e, f, g, h;
	int val;

	std::shared_ptr<int> thingy;

	std::tuple<int, int, int, long, long, int, GC::ptr<int>, int> tuple_thing;

	GC::ptr<std::unordered_map<std::string, unsigned int>> mapydoo1;
	GC::ptr<std::unordered_map<std::string, unsigned int>> mapydoo2;

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
	std::atomic<GC::ptr<double>> atomic_1;
	std::atomic<GC::ptr<double>> atomic_2;
	std::atomic<GC::ptr<double>> atomic_3;
	std::atomic<GC::ptr<double>> atomic_4;
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

std::atomic<GC::ptr<atomic_container>> *atomic_gc_ptr;



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



// runs statement and asserts that it throws the right type of exception
#define assert_throws(statement, exception) \
try { statement; std::cerr << "did not throw\n"; assert(false); } \
catch (const exception&) {} \
catch (...) { std::cerr << "threw wrong type\n"; assert(false); }

// runs statement and asserts that it throws (anything)
#define assert_throws_any(statement) \
try { statement; std::cerr << "did no throw\n"; assert(false); } \
catch (...) {}

// runs statement and asserts that it doesn't throw (anything)
#define assert_nothrow(statement) \
try { statement; } \
catch (...) { std::cerr << "threw an exception\n"; assert(false); }



int main() try
{
	GC::strategy(GC::strategies::manual);
	
	{
		std::atomic<bool> flag;
		for (int i = 0; i < 4096; ++i)
		{
			std::thread test_thread([]()
			{
				GC::collect();
			});

			{
				GC::ptr<bool_alerter> a = GC::make<bool_alerter>(flag);
			}

			test_thread.join();
			assert(flag);
		}
	}

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

	std::cerr << "-------- ctors --------\n";
	{
		GC::ptr<self_ptr> sp = GC::make<self_ptr>();
		GC::ptr<gc_self_ptr> gcsp = GC::make<gc_self_ptr>();

		sp->p = std::make_unique<decltype(sp)>(sp);
		gcsp->p = std::make_unique<decltype(gcsp)>(gcsp);
	}
	std::cerr << "-------- collect 1 --------\n";
	GC::collect();
	std::cerr << "-------- collect 2 --------\n";
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


	GC::ptr<atomic_container> atomic_container_obj = GC::make<atomic_container>();

	atomic_container_obj->atomic_1 = GC::make<double>(1.1);
	atomic_container_obj->atomic_2 = GC::make<double>(2.2);
	atomic_container_obj->atomic_3 = GC::make<double>(3.3);
	atomic_container_obj->atomic_4 = GC::make<double>(4.4);

	GC::collect();
	std::cerr << '\n';

	atomic_gc_ptr = new std::atomic<GC::ptr<atomic_container>>;

	GC::ptr<int[16]> arr_ptr_new = GC::make<int[16]>();
	assert(arr_ptr_new != nullptr);

	GC::collect();

	std::cerr << "\n----------------\n";
	{
		GC::ptr<alert_t> holder;

		{
			auto arr = GC::make<alert_t[]>(4);
			holder = arr.get(2);
		}
		std::cerr << "destroyed array\n";
	}
	std::cerr << "----------------\n\n";


	GC::ptr<int[6]> arr_test_0;
	GC::ptr<int[5][6]> arr_test_1;
	GC::ptr<int[][6]> arr_test_2;
	GC::ptr<int[][5][6]> arr_test_3;
	GC::ptr<int[]> arr_test_4;
	GC::ptr<virtual_type> arr_test_5;

	GC::ptr<int> sc_test_0;

	GC::reinterpretCast<int[]>(arr_test_0);
	GC::reinterpretCast<double[]>(arr_test_0);
	GC::constCast<const int[6]>(arr_test_0);
	GC::constCast<const int[]>(arr_test_4);
	GC::staticCast<const int[]>(arr_test_4);
	//GC::staticCast<const double[]>(arr_test_4);
	//GC::dynamicCast<const int>(arr_test_4);
	//GC::dynamicCast<int[]>(arr_test_5);

	GC::staticCast<const int>(sc_test_0);

	{
		int *_t = nullptr;
		const int *c_t = static_cast<const int*>(_t);
	}

	assert(std::is_same<decltype(arr_test_0.get()) COMMA int(*)[6]>::value);
	assert(std::is_same<decltype(arr_test_1.get()) COMMA int(*)[5][6]>::value);
	assert(std::is_same<decltype(arr_test_2.get()) COMMA int(*)[6]>::value);
	assert(std::is_same<decltype(arr_test_3.get()) COMMA int(*)[5][6]>::value);
	assert(std::is_same<decltype(arr_test_4.get()) COMMA int(*)>::value);

	
	GC::ptr<std::stack<int>> stack_ptr = GC::make<std::stack<int>>();
	GC::ptr<std::queue<int>> queue_ptr = GC::make<std::queue<int>>();
	GC::ptr<std::priority_queue<int>> priority_queue_ptr = GC::make<std::priority_queue<int>>();

	for (int i = 0; i < 8; ++i)
	{
		stack_ptr->push(i);
		queue_ptr->push(i);
		priority_queue_ptr->push(i);
	}
	
	GC::unique_ptr<int> gc_uint(new int(77));
	
	if (gc_uint != nullptr)
	{
		std::cerr << "non null gc unique ptr\n";
	}

	GC::vector<int> gc_vec;
	gc_vec.emplace_back(17);

	assert(gc_vec == gc_vec);

	//GC::ptr<int[]> arr_test = GC::make<int[]>(16);

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
	for (auto i : *pset) std::cerr << i << ' ';
	std::cerr << '\n';

	GC::ptr<GC::unordered_set<float>> puset = GC::make<GC::unordered_set<float>>(pset->begin(), pset->end());

	puset->insert(2.71828f);
	for (auto i : *puset) std::cerr << i << ' ';
	std::cerr << '\n';

	GC::ptr<GC::multiset<float>> pmset = GC::make<GC::multiset<float>>(puset->begin(), puset->end());

	pmset->insert(std::sqrt(2.0f));
	for (auto i : *pmset) std::cerr << i << ' ';
	std::cerr << '\n';

	GC::ptr<GC::unordered_multiset<float>> pumset = GC::make<GC::unordered_multiset<float>>(pmset->begin(), pmset->end());

	pumset->insert(std::sqrt(10.0f));
	for (auto i : *pumset) std::cerr << i << ' ';
	std::cerr << '\n';

	std::cerr << '\n';

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
	for (const auto &i : *pmap) std::cerr << i.first << " -> " << i.second << '\n';

	std::cerr << '\n';

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
	for (const auto &i : *pmmap) std::cerr << i.first << " -> " << i.second << '\n';

	std::cerr << '\n';

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
	for (const auto &i : *pumap) std::cerr << i.first << " -> " << i.second << '\n';

	std::cerr << '\n';

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
	for (const auto &i : *pummap) std::cerr << i.first << " -> " << i.second << '\n';

	std::cerr << '\n';

	{
		std::mutex mutex1, mutex2, mutex3, mutex4;

		{
			GC::scoped_lock<std::mutex, std::mutex, std::mutex, std::mutex> scoped_loq(mutex1, mutex2, mutex3, mutex4);
		}
		{
			GC::scoped_lock<std::mutex, std::mutex, std::mutex> scoped_loq(mutex1, mutex2, mutex3);
		}
		{
			GC::scoped_lock<std::mutex, std::mutex> scoped_loq(mutex1, mutex2);
		}
		{
			GC::scoped_lock<std::mutex> scoped_loq(mutex1);
		}
		{
			GC::scoped_lock<> scoped_loq;
		}
	}



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

		//auto i1_ = GC::staticCast<int>(i1);
    }
    GC::collect();
    std::cerr << "\n\n";

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

	//not_arr[0] = 9;
	(**yes_arr)[1][5] = 7;
	yes_arr[2][6][1][5] = 7;
	****yes_arr = 7;

	//GC::ptr<int[5]> wrong_t;
	GC::ptr<int[]> right_t;

	typedef int what_is_this[];
	typedef what_is_this *what_is_this_2;

	auto v_test1 = std::make_unique<std::vector<int>>(16);
	auto v_test2 = std::make_shared<std::vector<int>>(16);
	auto v_test3 = GC::make<std::vector<int>>(16);

	assert(v_test1->size() == 16);
	assert(v_test2->size() == 16);
	assert(v_test3->size() == 16);

	std::cerr << "convertible: " << std::is_convertible<int[1], int[1]>::value << '\n';

	std::cerr << "what is this? " << std::is_same<what_is_this_2, int(*)[]>::value << "\n\n";

	GC::atomic_ptr<float> atomic_test_0;
	GC::atomic_ptr<float> atomic_test_1;

	atomic_test_0 = GC::make<float>(2.718f);
	atomic_test_1 = GC::make<float>(3.141f);

	using std::swap;
	atomic_test_0.swap(atomic_test_1);
	swap(atomic_test_0, atomic_test_1);


	GC::collect();
	std::cerr << "\n\n";

	GC::ptr<int> arr_of_ptr[16];

	assert(std::is_default_constructible<GC::ptr<int>>::value);

	{
		std::cerr << "ptr<ptr<int>[2][2][2]>:\n";
		GC::ptr<GC::ptr<int>[][2][2]> arr_ptr = GC::make<GC::ptr<int>[][2][2]>(2);

		for (int i = 0; i < 2; ++i)
			for (int j = 0; j < 2; ++j)
				for (int k = 0; k < 2; ++k)
					arr_ptr[i][j][k] = GC::make<int>((i + j)*j + i*j*k + k*i + k + 1);

		for (int i = 0; i < 2; ++i)
			for (int j = 0; j < 2; ++j)
				for (int k = 0; k < 2; ++k)
					std::cerr << "elem (" << i << ", " << j << ", " << k << ") = " << *arr_ptr[i][j][k] << '\n';

		GC::collect();
		std::cerr << "\n\n";
	}
	
	sse_t t;
	t.d[4] = '6';
	std::cerr << "max align: " << alignof(std::max_align_t) << "\n\n";

	auto sse_t_obj = GC::make<sse_t>();

	GC::ptr<int> ip = GC::make<int>(46);
	GC::ptr<int> ip_self = ip;
	std::cerr << "      int val:  " << *ip << '\n';

	GC::ptr<const int> const_ip = GC::make<const int>(47);
	std::cerr << "const int val:  " << *const_ip << '\n';

	GC::ptr<const int> const_2 = ip;
	GC::ptr<const int> const_self = const_2;
	//GC::ptr<int> non_const_ref = const_2;
	std::cerr << "const int cast: " << *const_2 << '\n';

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

	//GC::dynamicCast<void>(bp1);
	//GC::dynamicCast<int>(bp1);
	
	//GC::dynamicCast<const int>(const_2);
	
	auto dyn_cast1 = GC::dynamicCast<virtual_type>(bp1);
	auto dyn_cast2 = GC::dynamicCast<non_virtual_type>(bp1);

	assert(dyn_cast1 == nullptr);
	assert(dyn_cast1 == nullptr);

	std::cerr << "\ndynamicCast():\nthese should be null: " << dyn_cast1 << ' ' << dyn_cast2 << "\n";

	dp->a = 123;
	dp->b = 456;

	std::cerr << '\n';

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

	std::cerr << "a: " << dp_as_b1->a << '\n';
	std::cerr << "b: " << dp_as_b2->b << '\n';

	std::unique_ptr<int[]> raw_up(new int[16]);
	GC::unique_ptr<int[]> gc_up(new int[16]);

	raw_up.reset();
	gc_up.reset();

	raw_up.reset(new int[16]);
	gc_up.reset(new int[16]);

	//auto _ii = std::make_unique<int[]>(56);
	//auto _jj = std::make_shared<int[]>(56);
	//auto _kk = GC::make<int[]>(56);

	//assert(std::is_same<int,int>::value);

	//GC::ptr<derived> wrong_dp = bp;

	//dp_as_b = dp;
	//dp_as_b = 67;

	//std::cin.get();

	/**/
	{
		std::thread t1([]()
		{
			//while (1)
			{
				/*
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();


				std::cerr << "--------------------------------------begin segment\n";
				GC::ptr<atomic_container> _subptr = atomic_gc_ptr->load();
				std::cerr << "--------------------------------------       subptr get\n";
				_subptr->atomic_1 = GC::make<double>(1.1);
				_subptr->atomic_2 = GC::make<double>(2.2);
				_subptr->atomic_3 = GC::make<double>(3.3);
				_subptr->atomic_4 = GC::make<double>(4.4);

				using std::swap;

				swap(_subptr->atomic_1, _subptr->atomic_2);

				//(*atomic_gc_ptr).load()->atomic_1 = GC::make<double>(1.1);
				//(*atomic_gc_ptr).load()->atomic_2 = GC::make<double>(2.2);
				//(*atomic_gc_ptr).load()->atomic_3 = GC::make<double>(3.3);
				//(*atomic_gc_ptr).load()->atomic_4 = GC::make<double>(4.4);
				std::cerr << "--------------------------------------        end segment\n";

				foo();

				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*atomic_gc_ptr = GC::make<atomic_container>();
				*/
				
				/**/
				
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

					std::cerr << "pass " << pass << '\n';
				}

				std::cerr << "phase 1 completed\n";

				/**/

				GC::ptr<int> pi;

				for (int i = 0; ; ++i)
				{
					GC::ptr<int> temp = GC::make<int>(i);
					pi = temp;

					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}

				std::cerr << "printy\n";
			}
		});
		//std::thread t2([]()
		//{

		int i = 0;
		//while (1)
		{
			//*atomic_gc_ptr = GC::make<atomic_container>();

			//std::cerr << "collecting pass " << ++i << '\n';
			GC::collect();

			//std::this_thread::sleep_for(std::chrono::seconds(2));

			//*atomic_gc_ptr = GC::make<atomic_container>();
		}
		//});
		
		t1.join();
		//t2.join();
	}
	/**/

	std::cin.get();
	return 0;
}
catch (const std::exception &ex)
{
	std::cerr << "\nEXCEPTION!!\n\n" << ex.what() << "\n\n\n";
	std::cin.get();
}
