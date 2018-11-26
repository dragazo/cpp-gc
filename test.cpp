#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstddef>
#include <cassert>

#include "GarbageCollection.h"

struct alignas(16) sse_t { char d[16]; };

struct ptr_set
{
	GC::ptr<int> a, b, c, d, e, f, g, h;
	const int val;

	std::unique_ptr<int> thingy;

	std::tuple<int, int, int, long, long, int, GC::ptr<int>, int> tuple_thing;

	GC::ptr<std::unordered_map<std::string, unsigned int>> mapydoo1;
	GC::ptr<std::unordered_map<std::string, unsigned int>> mapydoo2;

	ptr_set() : val(0)
	{
		auto ptr = new std::unordered_map<std::string, unsigned int>;
		ptr->emplace("hello", 67);
		ptr->emplace("world", 4765);

		mapydoo1 = GC::adopt(ptr);
		mapydoo2 = GC::adopt<std::unordered_map<std::string, unsigned int>>(nullptr);
	}
};
template<> struct GC::router<ptr_set>
{
	static void route(const ptr_set &set, router_fn func)
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

	}
};

struct ListNode
{
	GC::ptr<ListNode> prev;
	GC::ptr<ListNode> next;

	const ptr_set set;

	// show a message that says we called ctor
	ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(4)); }

	// show a message that says we called dtor
	~ListNode() { std::this_thread::sleep_for(std::chrono::microseconds(4)); }
};
template<> struct GC::router<ListNode>
{
	static void route(const ListNode &node, router_fn func)
	{
		GC::route(node.prev, func);
		GC::route(node.next, func);

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

		//GC::collect();
		//std::cerr << "\n\n";
	}
}

template<typename T>
struct wrap
{
	GC::ptr<T> ptr;
};
template<typename T>
struct GC::router<wrap<T>>
{
	static void route(const wrap<T> &w, router_fn fn)
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
    static void route(const TreeNode &node, GC::router_fn func)
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
    void update(std::string name, GC::ptr<TreeNode> new_value)
    {
        // modification of the mutable collection of GC::ptr and router must be mutually exclusive
        std::lock_guard<std::mutex> lock(symbols_mutex);
        
        symbols[name] = new_value;
    }
};
template<> struct GC::router<SymbolTable>
{
    static void route(const SymbolTable &table, GC::router_fn func)
    {
        // modification of the mutable collection of GC::ptr and router must be mutually exclusive
        std::lock_guard<std::mutex> lock(table.symbols_mutex);

        GC::route(table.symbols, func);
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
    static void route(const MaybeTreeNode &maybe, GC::router_fn func)
    {
        // this must be synchronized with constructing/destucting the buffer object.
        std::lock_guard<std::mutex> lock(maybe.buf_mutex);
        
        // because TreeNode contains GC::ptr objects, we need to route to it,
        // but only if the MaybeTreeNode actually has it constructed in the buffer.
        if (maybe.contains_tree_node) GC::route(*reinterpret_cast<const TreeNode*>(maybe.buf), func);
    }
};





int main()
{
	GC::strategy(GC::strategies::manual);

	//GC::ptr<int[]> arr_test = GC::make<int[]>(16);

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

	/*merp->push_back(GC::make<ListNode>());
	merp->push_back(GC::make<ListNode>());
	merp->emplace_back(GC::make<ListNode>());
	merp->emplace_back(GC::make<ListNode>());
	merp->push_back(GC::make<ListNode>());
	merp->push_back(GC::make<ListNode>());
	merp->emplace_back(GC::make<ListNode>());
	merp->emplace_back(GC::make<ListNode>());*/
	GC::collect();
	std::cerr << "\n\n";

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

	//auto _ii = std::make_unique<int[]>(56);
	//auto _jj = std::make_shared<int[]>(56);
	//auto _kk = GC::make<int[]>(56);

	//assert(std::is_same<int,int>::value);

	//GC::ptr<derived> wrong_dp = bp;

	//dp_as_b = dp;
	//dp_as_b = 67;

	std::cin.get();

	/**/
	{
		std::thread t1([]() { while (1) foo(); });
		std::thread t2([]() { int i = 0; while (1) { std::cerr << "collecting pass " << ++i << '\n'; GC::collect(); } });
		
		t1.join();
		t2.join();
	}
	/**/

	std::cin.get();
	return 0;
}
