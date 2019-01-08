# cpp-gc

One big complaint I've seen from C++ initiates is that the language doesn't have automatic garbage collection. Although on most days I'd argue that's actually a feature, let's face it: sometimes it's just inconvenient. I mean sure, there are standard C++ utilities to help out, but as soon as cycles are involved even the mighty `std::shared_ptr` ceases to function. You could always refactor your entire data structure to use `std::weak_ptr`, but for anything as or more complicated than a baked potato that means doing all the work manually anyway.

`cpp-gc` is a **self-managed**, **thread-safe**, **non-blocking** garbage collection library written in **standard C++14**.

With `cpp-gc` in place, all you'd need to do to fix the above example is change all your `std::shared_ptr<T>` to `GC::ptr<T>`. The rest of the logic will take care of itself automatically *(with one exception - see below)*.

Contents:

* [How It Works](#how-it-works)
* [Guarantees](#guarantees)
* [GC Strategy](#gc-strategy)
* [Formal Definitions](#formal-definitions)
* [Router Functions](#router-functions)
* [Built-in Router Functions](#built-in-router-functions)
* [Example Structs and Router Functions](#example-structs-and-router-functions)
* [Undefined Behavior](#undefined-behavior)
* [Usage Examples](#usage-examples)
* [Best Practices](#best-practices)

## How it Works

`cpp-gc` is designed to be **streamlined and minimalistic**. Very few things are visible to the user. The most important things to know are:
* `GC` - Static class containing types and functions that help you **manage your memory conveniently**.
* `GC::ptr<T>` - The shining star of `cpp-gc` - represents an **autonomous garbage-collected pointer**.
* `GC::atomic_ptr<T>` - An atomic version of `GC::ptr<T>` that's safe to read/write from several threads. Equivalent to `std::atomic<GC::ptr<T>>`.
* `GC::make<T>(Args&&...)` - Constructs a new dynamic object and puts it in gc control. Used like `std::make_shared`.
* `GC::adopt<T>(T*)` - Adopts a pre-existing object into gc control. Like the `T*` constructor of e.g. `std::shared_ptr<T>`.
* `GC::collect()` - Triggers a full garbage collection pass *(see below)*.

When you allocate an object via `GC::make<T>` or bind a pre-existing object with `GC::adopt<T>` it creates a new garbage-collected object with a reference count of 1. Just like `std::shared_ptr`, it will automatically manage the reference count and delete the object **immediately** when the reference count hits zero *(except in one case - see [Guarantees](#guarantees))*. What does this mean? Well this means if `std::shared_ptr` worked for you before, it'll work for you now exactly the same *(though a bit slower due to having extra things to manage)*.

`GC::collect()` triggers a full garbage collection pass, which accounts for cycles using the typical mark-and-sweep algorithm. This is rather slow compared to the other method `cpp-gc` uses to manage non-cyclic references, but is required if you do in fact have cycles. So when should you call it? Probably never. I'll explain:

## Guarantees

The following guarantees are made for all objects under gc control assuming all objects present have cpp-gc compliant router functions (see below) and are not destroyed by external code:

* Adding an object to gc control (i.e. `GC::make<T>` or `GC::adopt<T>`) offers the strong exception guarantee and is O(1).
* Once under gc control, the object shall not be relocated - i.e. raw pointers to said object will never be invalidated.
* The allocating form of gc object insertion (i.e. `GC::make<T>`) shall allocate a block of memory suitably aligned for type `T` even if `T` is an overaligned type.
* Any function or utility exposed by the cpp-gc interface is written in such a way that deadlocks are impossible under any circumstance.
* Invoking a garbage collection (i.e. `GC::collect()`) while another garbage collection is running in any thread is non-blocking and indeed no-op.
* A reference count shall be maintained for each object under gc control. When this reference count reaches zero the object is immediately deleted unless it is currently under collection consideration by an active call to `GC::collect()`, in which case the object is guaranteed to be destroyed before the end of said call to `GC::collect()`.

Given the same assumptions of objects under gc control, the following (non-)guarantees are made by cpp-gc:

* The thread that destroys an object under gc control is undefined. If your object requires the same thread that made it to destroy it (e.g. `std::unique_lock<std::mutex>`), it should not be used directly by cpp-gc.

## GC Strategy

`cpp-gc` has several "strategy" options for automatically deciding when to perform a full garbage collect pass. This is controlled by a bitfield enum called `GC::strategies`.

The available strategy options are:
* `manual` - No automatic collection (except non-cyclic dependencies, which are always handled automatically and immediately).
* `timed` - Collect from a background thread on a regular basis.
* `allocfail` - Collect every time a call to `GC::make<T>(Args...)` fails to allocate space.

`GC::strategy()` allows you to read/write the strategy to use.

`GC::sleep_time()` allows you to change the sleep time duration for timed collection.

The default strategy is `timed | allocfail`, with the time set to 60 seconds.

Typically, if you want to use non-default settings, you should set them up as soon as possible on program start and not modify them again.

## Formal Definitions

**This section is extremely-important. If you plan to use `cpp-gc` in any capacity, read this section in its entirety.**

A type `T` is defined to be "gc" if it owns an object that is itself considered to be gc.
By definition, `GC::ptr`, `GC::atomic_ptr`, and `std::atomic<GC::ptr>` are gc types.
Ownership means said "owned" object's lifetime is entirely controlled by the "owner".
An object may only have one owner at any point in time - shared ownership is considered non-owning.
Thus global variables and static member variables are never considered to be owned objects.
At any point in time, the owned object is considered to be part of its owner (i.e. as if it were a by-value member).

The simplest form of ownership is a by-value member.
Another common category of owned object is a by-value container (e.g. the contents of `std::vector`, `std::list`, `std::set`, etc.).
Another case is a uniquely-owning pointer or reference - e.g. pointed-to object of `std::unique_ptr`, or any other (potentially-smart) pointer/reference to an object you know you own uniquely.
Of course, these cases can be mixed - e.g. a by-value member `std::unique_ptr` which is a uniquely-owning pointer to a by-value container `std::vector` of a gc type.

It is important to remember that a container type like `std::vector<T>` is a gc type if `T` is a gc type (because it thus contains or "owns" gc objects).

Object reachability traversals are performed by router functions.
For a gc type `T`, its router functions route a function-like object to its owned gc objects recursively.
Because object ownership cannot be cyclic, this will never degrade into infinite recursion.

Routing to anything you don't own is undefined behavior.
Routing to the same object twice is likewise undefined behavior.
Thus, although it is legal to route to the "contents" of an owned object (i.e. owned object of an owned object), it is typically dangerous to do so.
In general, you should just route to all your by-value members - it is their responsibility to properly route the message the rest of the way.
Thus, if you own a `std::vector<T>` where `T` is a gc type, you should just route to the vector itself, which has its own router functions to route the message to its contents.

If you are the owner of a gc type object, it is undefined behavior not to route to it (except in the special case of a mutable router function - see below).

For a gc type `T`, a "mutable" owned object is defined to be an owned object for which you could legally route to its contents but said contents can be changed after construction.
e.g. `std::vector`, `std::list`, `std::set`, and `std::unique_ptr` are all examples of "mutable" gc types because you own them (and their contents), but their contents can change.
An "immutable" or "normal" owned object is defined as any owned object that is not "mutable".

More formally, consider an owned object x:
Suppose you examine the set of all objects routed-to through x recursively.
x is a "mutable" owned gc object iff for any two such router invocation sets taken over the lifetime of x the two sets are different. 

It should be noted that re-pointing a `GC::ptr`, `GC::atomic_ptr`, or `std::atomic<GC::ptr>` is not a mutating action for the purposes of classifying a "mutable" owned gc object.
This is due to the fact that you would never route to its pointed-to contents due to it being a shared resource.

The following is critical and easily forgotten:
All mutating actions in a mutable gc object (e.g. adding items to a `std::vector<GC::ptr<T>>`) must occur in a manner mutually exclusive with the object's router function.
This is because any thread may at any point make routing requests to any number of objects under gc management in any order and for any purpose.
Thus, if you have e.g. a `std::vector<GC::ptr<T>>`, you should also have a mutex to guard it on insertion/deletion/reordering of elements and for the router function.
This would likely need to be encapsulated by methods of your class to ensure external code can't violate this requirement (it is undefined behavior to violate this).

On the bright side, cpp-gc has wrappers for all standard containers that internally apply all of this logic without the need to remember it.
So, you could use a `GC::vector<GC::ptr<T>>` instead of a `std::vector<GC::ptr<T>>` and avoid the need to be careful or encapsulate anything.

The following requirements pertain to router functions:
For a normal router function (i.e. `GC::router_fn`) this must at least route to all owned gc objects.
For a mutable router function (i.e. `GC::mutable_router_fn`) this must at least route to all owned "mutable" gc objects.
The mutable router function system is entirely a method for optimization and can safely be ignored if desired.

## Router Functions

**This section is extremely-important. If you plan to use `cpp-gc` in any capacity, read this section in its entirety.**

Other languages that implement garbage collection have it built right into the language and the compiler handles all the nasty bits for you. For instance, one piece of information a garbage collector needs to know is the set of all outgoing garbage-collected pointers from an object.

However, something like this is impossible in C++. After all, C++ allows you to do very low-level things like create a buffer as `alignas(T) char buffer[sizeof(T)]` and then refer to an object that you conditionally may or may not have constructed at any point in runtime via `reinterpret_cast<T*>(buffer)`. Garbage-collected languages like Java and C# would curl up into a little ball at the sight of this, but in C++ it's rather common *(in library code)*.

And so, if you want to create a proper garbage-collected type for use in `cpp-gc` you must do a tiny bit of extra work to specify such things explicitly. So how do you do this?

The following struct represents the router function set for objects of type `T`.
The `T` in `router<T>` must not be cv-qualified.
Router functions must be static, named "route", return void, and take two args: a reference to (possibly cv-qualified) `T` and a by-value router function object (i.e. `GC::router_fn` or `GC::mutable_router_fn`).
If you don't care about the efficiency mechanism of mutable router functions, you can define the function type as a template type paramter, but it must be deducible.
The default implementation is no-op, which is suitable for any non-gc type.
This should not be used directly for routing to owned objects - use the helper functions `GC::route()` and `GC::route_range()` instead.

```cpp
template<typename T>
struct router
{
    template<typename F>
    static void route(const T &obj, F func) {}
};
```

This is the means by which `cpp-gc` polls your object for outgoing arcs. Here's a short example of how it works - consider the following type definition and its correct `cpp-gc` router specialization:

```cpp
struct MyType
{
    GC::ptr<int>    foo;
    GC::ptr<double> bar;
    
    int    some_data;
    double some_other_data;
    
    std::vector<bool> flags;
};
template<> struct GC::router<MyType>
{
    // when we want to route to MyType
    template<typename F>
    static void route(const MyType &obj, F func)
    {
        // also route to its children (only ones that may own GC::ptr objects are required)
        GC::route(obj.foo, func);
        GC::route(obj.bar, func);
    }
};
```

So here's what's happening: `cpp-gc` will send a message *(the 'func' object)* to your type. Your type will route that to all its children. Recursively, this will eventually reach leaf types *(which have no children)* via `GC::route()` or `GC::route_range()`. Because object ownership cannot be cyclic, this will never degrade into an infinite loop.

Additionally, you only need to route to children that may own (directly or indirectly) `GC::ptr` objects. Routing to anything else is a glorified no-op that may be slow if your optimizer isn't smart enough to elide it. However, because optimizers are generally pretty clever, you may wish to route to some objects just for future-proofing. Feel free.

The following section summarizes built-in specializations of `GC::router`:

## Built-in Router Functions

The following types have well-formed `GC::router` specializations pre-defined for your convenience. As mentioned in the above section, you should not route to one of these types and also to its contents, as this would result in routing to the same object twice, which is undefined bahavior.

* `T[N]`
* `std::array<T, N>`
* `std::atomic<GC::ptr<T>>`
* `std::deque<T, Allocator>`
* `std::forward_list<T, Allocator>`
* `std::list<T, Allocator>`
* `std::map<Key, T, Compare, Allocator>`
* `std::multimap<Key, T, Compare, Allocator>`
* `std::multiset<Key, Compare, Allocator>`
* `std::pair<T1, T2>`
* `std::priority_queue<T, Container, Compare>`
* `std::queue<T, Container>`
* `std::set<Key, Compare, Allocator>`
* `std::stack<T, Container>`
* `std::tuple<Types...>`
* `std::unique_ptr<T, Deleter>`
* `std::unordered_map<Key, T, Hash, KeyEqual, Allocator>`
* `std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator>`
* `std::unordered_multiset<Key, Hash, KeyEqual, Allocator>`
* `std::unordered_set<Key, Hash, KeyEqual, Allocator>`
* `std::vector<T, Allocator>`

Additionally, the equivalent wrappers for the above standard library containers defined in the `GC` class likewise have well-formed router functions (which perform their own synchronization logic - see [Formal Definitions](#formal-definitions).

The following types have ill-formed `GC::router` specializations pre-defined for safety. This is typically because there is no way to route to said type's contents or to hint that routing to such an object won't have the desired effect. It is a compile error to use any of these, which should help limit confusion on usage.

* `T[]`
* `std::atomic<T>` (where T is not a `GC::ptr`)

## Example Structs and Router Functions

This section will consist of several examples of possible structs/classes you might write and their accompanying router specializations, including all necessary safety features in the best practices section. Only the relevant pieces of code are included. Everything else is assumed to be exactly the same as normal C++ class writing with no tricks involved.

This example demonstrates the most common use case: having `GC::ptr` objects by value in the object. Note that this type doesn't contain any "mutable" gc objects, so we can optionally make the mutable router function no-op.

```cpp
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
    // the "normal" router function
    static void route(const TreeNode &node, GC::router_fn func)
    {
        // route to our GC::ptr instances
        GC::route(node.left, func);
        GC::route(node.right, func);
        // no need to route to anything else
    }
    // the "mutable" router function
    static void route(const TreeNode &node, GC::mutable_router_fn) {}
};
```

Now we'll use the tree type we just created to make a garbage-collected symbol table. This demonstrates the (less-frequent) use case of having a mutable container of `GC::ptr` objects. This requires special considerations in terms of thread safety, as mentioned in the above section on router functions.

Note that in this case we have "mutable" gc children. Thus, we'll elect to merge the router functions together by making it a template.

```cpp
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
    // serves as both the "normal" and the "mutable" router function
    template<typename F>
    static void route(const SymbolTable &table, F func)
    {
        // modification of the mutable collection of GC::ptr and router must be mutually exclusive
        std::lock_guard<std::mutex> lock(table.symbols_mutex);

        GC::route(table.symbols, func);
    }
};
```

Of course, the above seems rather bloated. Having to wrap all that mutex logic is super annoying. Fortunately, *(mentioned above)* `cpp-gc` defines wrappers for all the standard library containers that apply this mutex logic internally and offer the same interface to the user.

So we could *(and should)* have written the above as this:

```cpp
class SymbolTable
{
public:

    // like std::unordered_map but performs the router mutex logic internally
    GC::unordered_map<std::string, GC::ptr<TreeNode>> symbols;
    
    void update(std::string name, GC::ptr<TreeNode> new_value)
    {
        symbols[name] = new_value;
    }
};
template<> struct GC::router<SymbolTable>
{
    // serves as both the "normal" and the "mutable" router function
    template<typename F>
    static void route(const SymbolTable &table, F func)
    {
        GC::route(table.symbols, func);
    }
};

Note that even though we used the `cpp-gc` wrapper type `GC::unordered_map` it's still considered a "mutable" gc object, and thus we still need to route to it in the mutable router. All it does for us is make usage easier and cleaner by not having to explicitly lock mutexes.

The next example demonstrates a rather uncommon use case: having a memory buffer that may at any time either contain nothing or a constructed object of a type we need to route to. This requires a bit more effort on your part. This is uncommon because said buffer could just be replaced with a `GC::ptr` object to avoid the headache, with null being the no-object state. Never-the-less, it is shown here as an example in case you need such behavior.

```cpp
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
    // serves as both the "normal" and "mutable" router functions via templating
    template<typename F>
    static void route(const MaybeTreeNode &maybe, F func)
    {
        // this must be synchronized with constructing/destucting the buffer object.
        std::lock_guard<std::mutex> lock(maybe.buf_mutex);
        
        // because TreeNode contains GC::ptr objects, we need to route to it,
        // but only if the MaybeTreeNode actually has it constructed in the buffer.
        if (maybe.contains_tree_node) GC::route(*reinterpret_cast<const TreeNode*>(maybe.buf), func);
    }
};
```

## Undefined Behavior

In this section, we'll cover all the cases that result in undefined behavior and summarize the logic behind these decisions.

* Dereferencing/Indexing a null `GC::ptr` - obvious.
* Not routing to all your owned objects - messes up reachability traversal and can result in prematurely-deleted objects.
* Routing to the same object twice during the same router event - depending on what the router event is trying to do, this could cause all sorts of problems.
* Not making your router function mutually explusive with re-pointing or adding/removing/etc things you would route to - explained in immense detail above.
* Accessing the pointed-to object of a `ptr<T>` in the destructor of its (potentially-indirect) owner - you and the pointed-to object might have been scheduled for garbage collection together and the order of destruction by the garbage collector is undefined, so it might already have been destroyed.
* Using `GC::adopt(T *obj)` where obj is not an instance of `T` - e.g. obj must not be pointer to base. This is because `GC::adopt()` needs the true type of the object to get its router functions. Thus if `T` is not the true type it would be using the wrong router functions and result in undefined behavior.

## Usage Examples

For our example, we'll make a doubly-linked list that supports garbage collection. We'll begin by defining our node type `ListNode`.

```cpp
struct ListNode
{
    GC::ptr<ListNode> prev;
    GC::ptr<ListNode> next;
    
    // show a message that says we called ctor
    ListNode() { std::cerr << "i'm alive!!\n"; }
    
    // show a message that says we called dtor
    ~ListNode() { std::cerr << "i died!!\n"; }
};
```

Because this type owns `GC::ptr` instances, we need to specialize `GC::router` for our type.

```cpp
template<> struct GC::router<ListNode>
{
    // "normal" router function
    static void route(const ListNode &node, router_fn func)
    {
        // call GC::route() for each of our GC::ptr values
        GC::route(node.prev, func);
        GC::route(node.next, func);
    }
    // "mutable" router function
    static void route(const ListNode &node, mutable_router_fn)
    {
        // we don't have any mutable children, so this can be no-op
    }
};
```

That's all the setup we need - from here on it's smooth sailing. Let's construct the doubly-linked list.

```cpp
// creates a linked list that thus contains cycles
void foo()
{
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
}
```

If you run this, you'll find none of the objects are deallocated immediately. This is because we created a bunch of cycles due to making a *doubly*-linked list. As stated above, `cpp-gc` cleans this up via `GC::collect()`. Let's do that now:

```cpp
// the function that called foo()
void bar()
{
    // we need to call foo()
    foo();
    
    std::cerr << "\n\ncalling collect():\n\n";
    
    // the default collect settings will clean this up automatically after a while
    // but let's say we need this to happen immediately for some reason...
    GC::collect();
}
```

As soon as `GC::collect()` is executed, all the cycles will be dealt with and you should get a lot of messages from destructors. It's that easy. But remember, the default auto-collect strategy is time based. You typically won't ever need to call `GC::collect()` explicitly.

## Best Practices

Here we'll go through a couple of tips for getting the maximum performance out of `cpp-gc` while minimizing the chance for error:

1. **Performance** - Don't call `GC::collect()` explicitly. If you start calling `GC::collect()` explicitly, there's a pretty good chance you could be calling it in rapid succession. This will do little more than cripple your performance. The only time you should ever call it explicitly is if you for some reason **need** the objects to be destroyed immediately *(which is unlikely)*.

1. **Performance** - When possible, use raw pointers. Let's say you have a `GC::ptr<std::vector<int>>` that you need to pass to a function. Does the function really need to **own** the value or does it just need access to it? In the vast majority of cases, you'll find you only need access to the object. In these cases, you're much better off performance-wise to have the function take a raw pointer instead. This also has the effect of being less restrictive (i.e. you don't need to pass the object as a specific type of smart pointer). *(this same rule applies to other smart pointers like `std::shared_ptr` as well)*.

1. **Performance** - If you find you only use a `GC::ptr` instance to point to another object for normal pointer logic (and if you know that reference isn't isn't the only reference to said object) you should use `GC::ptr<T>*` or `GC::ptr<T>&` instead. This still lets you refer to the `GC::ptr<T>` object (and what it points to) but doesn't require unnecessary increments/decrements on each and every assignment to/from it. This is demonstrated in the example above, where the end-of-list pointer was a raw pointer to a gc pointer.

1. **Safety** - As mentioned in the section on router functions, if your type owns an object that you would route to but that can be re-pointed or modified in some way (e.g. `std::vector<GC::ptr<int>>`, `std::unique_ptr<GC::ptr<int>>` etc.), re-pointing or adding/removing etc. must be atomic with respect to the router function routing to its contents. Because of this, you'll generally need to use a mutex to synchronize access to the object's contents. To make sure no one else messes up this safety, such an object should be made private and given atomic accessors if necessary.

1. **Safety** - Continuing off the last point, do not create a lone (i.e. not in a struct/class) `GC::ptr<T>` where `T` is a mutable container of `GC::ptr`. Modifying a lone instance of it could be rather dangerous due to not having a built-in mutex and encupsulation with atomic accessors.

1. **Safety** - If you're unsure if an owned object is "mutable" or not, assume it is. A false positive is slightly inefficient, but a false negative is undefined behavior.
