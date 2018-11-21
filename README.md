# cpp-gc

One big complaint I've seen from C++ initiates is that the language doesn't have automatic garbage collection. Although on most days I'd argue that's actually a feature, let's face it: sometimes it's just inconvenient. I mean sure, there are standard C++ utilities to help out, but as soon as cycles are involved even the mighty `std::shared_ptr` ceases to function. You could always refactor your entire data structure to use `std::weak_ptr`, but for anything as or more complicated than a baked potato that means doing all the work manually anyway.

`cpp-gc` is a **self-managed**, **thread-safe** garbage collection library written in **standard C++14**.

With `cpp-gc` in place, all you'd need to do to fix the above example is change all your `std::shared_ptr<T>` to `GC::ptr<T>`. The rest of the logic will take care of itself automatically *(with one exception - see below)*.

Contents:

* [How It Works](#how-it-works)
* [GC Strategy](#gc-strategy)
* [Router Functions](#router-functions)
* [Built-in Router Functions](#built-in-router-functions)
* [Examples](#examples)
* [Best Practices](#best-practices)

## How it Works

`cpp-gc` is designed to be **streamlined and minimalistic**. Very few things are visible to the user. The most important things to know are:
* `GC` - Static class containing types and functions that help you **manage your memory conveniently**.
* `GC::ptr<T>` - The shining star of `cpp-gc` - represents an **autonomous garbage-collected pointer**.
* `GC::make<T>(Args...)` - This is how you create an instance of `GC::ptr<T>` - it's used exactly like `std::make_shared`.
* `GC::collect()` - Triggers a full garbage collection pass *(see below)*.

When you allocate an object via `GC::make<T>(Args...)` it creates a new garbage-collected object with a reference count of 1. Just like `std::shared_ptr`, it will automatically manage the reference count and delete the object **immediately** when the reference count hits zero. What does this mean? Well this means if `std::shared_ptr` worked for you before, it'll work for you now exactly the same *(though a bit slower due to having extra things to manage)*.

`GC::collect()` triggers a full garbage collection pass, which accounts for cycles using the typical mark-and-sweep algorithm. This is rather slow compared to the other method `cpp-gc` uses to manage non-cyclic references, but is required if you do in fact have cycles. So when should you call it? Probably never. I'll explain:

## GC Strategy ##

`cpp-gc` has several "strategy" options for automatically deciding when to perform a full garbage collect pass. This is controlled by a bitfield enum called `GC::strategies`.

The available strategy options are:
* `manual` - No automatic collection (except non-cyclic dependencies, which are always handled automatically and immediately).
* `timed` - Collect from a background thread on a regular basis.
* `allocfail` - Collect every time a call to `GC::make<T>(Args...)` fails to allocate space.

`GC::strategy()` allows you to read/write the strategy to use.

`GC::sleep_time()` allows you to change the sleep time duration for timed collection.

The default strategy is `timed | allocfail`, with the time set to 60 seconds.

Typically, if you want to use non-default settings, you should set them up as soon as possible on program start and not modify them again.

## Router Functions

**This section is extremely-important. If you plan to use `cpp-gc` in any capacity, read this section in its entirety.**

Other languages that implement garbage collection have it built right into the language and the compiler handles all the nasty bits for you. For instance, one piece of information a garbage collector needs to know is the set of all outgoing garbage-collected pointers from an object.

However, something like this is impossible in C++; after all, C++ allows you to do very low-level things like create a buffer as `char buffer[sizeof(T)]` and then refer to an object that you conditionally may or may not have constructed at any point in runtime via `reinterpret_cast<T*>(buffer)`. Garbage collected languages like Java and C# would curl up into a little ball at the sight of this.

And so, if you want to create a proper garbage-collected type for use in `cpp-gc` you must do a tiny bit of extra work to specify such things explicitly. So how do you do this?

`cpp-gc` declares a template struct called `router` with the following form:

```cpp
template<typename T>
struct router
{
    static void route(const T &obj, router_fn func) {}
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
    static void route(const MyType &obj, GC::router_fn func)
    {
        // also route to its children (only ones that may own GC::ptr objects are required)
        GC::route(obj.foo, func);
        GC::route(obj.bar, func);
    }
};
```

So here's what's happening: `cpp-gc` will send a message *(the router_fn object)* to your type. Your type will route that to all its children. Recursively, this will eventually reach leaf types, which have no children. Because object ownership cannot be cyclic, this will never degrade into an infinite loop.

Additionally, you only need to route to children that may own (directly or indirectly) `GC::ptr` objects. Routing to anything else is a glorified no-op that may be slow if your optimizer isn't smart enough to elide it. However, because optimizers are generally pretty clever, you may wish to route to some objects just for future-proofing. Feel free.

To be absolutely explicit: you "own" any object for which you **alone** control the lifetime. Such an "owned" object can only have one owner at a time. Thus, you would never route to a shared resource, even if you happen to be the only owner at the moment of invocation.

Thus, you would route to the contents of a `std::vector<T>` or to the pointed-to object of `std::unique_ptr<T>`, but not to the pointed-to object of `T*` unless you know that you own it in the same way that `std::unique_ptr<T>` owns its pointed-to object. Although the same rules apply, you should probably never route to the pointed-to object of `std::shared_ptr<T>` or `GC::ptr<T>`, as these imply it's a shared resource. You can still route to these objects (and must in the case of `GC::ptr<T>`), just not to what they point to.

If it is possible to re-point or add/remove something you would route to, such an operation must be atomic with respect to your router specialization. So, if you have a `std::vector<GC::ptr<int>>`, you would need to ensure adding/removing to/from this container and your router function iterating through it are mutually exclusive (i.e. mutex). This would also apply for re-pointing e.g. a `std::unique_ptr<GC::ptr<int>>`. However, re-pointing a `GC::ptr` is already atomic with respect to all router functions, so there's no need to do anything in this last case.

Additionally, it is undefined behavior to route to the same object twice. Because of this, you should not route to some object and also to something inside it - e.g. you can route to a `std::vector<T>` or to each of its elements, but you should never do both.

The default implementation for `router<T>::route()` does nothing, which is ok if `T` does not own any `GC::ptr` instances directly or indirectly, but otherwise you need to specialize it for your type.

This is extremely-important to get correct, as anything you don't route to is considered not to be a part of your object, which could result in premature deallocation of resources. So take extra care to make sure this is correct.

The following section summarizes built-in specializations of `GC::router`:

## Built-in Router Functions

The following types have well-formed `GC::router` specializations pre-defined for your convenience. As mentioned in the above section, you should not route to one of these types and also to its contents, as this would result in routing to the same object twice, which is undefined bahavior.

* `T[N]`
* `std::array<T, N>`
* `std::deque<T, Allocator>`
* `std::forward_list<T, Allocator>`
* `std::list<T, Allocator>`
* `std::map<Key, T, Compare, Allocator>`
* `std::multimap<Key, T, Compare, Allocator>`
* `std::multiset<Key, Compare, Allocator>`
* `std::pair<T1, T2>`
* `std::set<Key, Compare, Allocator>`
* `std::tuple<Types...>`
* `std::unique_ptr<T, Deleter>`
* `std::unordered_map<Key, T, Hash, KeyEqual, Allocator>`
* `std::unordered_multimap<Key, T, Hash, KeyEqual, Allocator>`
* `std::unordered_multiset<Key, Hash, KeyEqual, Allocator>`
* `std::unordered_set<Key, Hash, KeyEqual, Allocator>`
* `std::vector<T, Allocator>`

The following types have ill-formed `GC::router` specializations pre-defined for safety. This is typically because there is no way to route to said type's contents. It is a compile error to use any of these, which should help limit confusion.

* `std::priority_queue<T, Container, Compare>`
* `std::queue<T, Container>`
* `std::stack<T, Container>`

## Examples

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
    static void route(ListNode &node, router_fn func)
    {
        // call GC::route() for each of our GC::ptr values
        GC::route(node.prev, func);
        GC::route(node.next, func);
    }
};
```

That's all the setup we need - from here on it's smooth sailing. Let's construct the doubly-linked list.

```cpp
// creates a linked list that has a cycle
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

Here we'll go through a couple of tips for getting the maximum performance out of `cpp-gc`:

1. Don't call `GC::collect()` explicitly. If you start calling `GC::collect()` explicitly, there's a pretty good chance you could be calling it in rapid succession. This will do little more than cripple your performance. The only time you should ever call it explicitly is if you for some reason **need** the objects to be destroyed immediately *(which is unlikely)*.

1. When possible, use raw pointers. Let's say you have a `GC::ptr<std::vector<int>>` that you need to pass to a function. Does the function really need to **own** the value or does it just need access to it? In the vast majority of cases, you'll find you only need access to the object. In these cases, you're much better off performance-wise to have the function take a raw pointer instead. This also has the effect of being less restrictive (i.e. you don't need to pass the object as a specific type of smart pointer). *(this same rule applies to other smart pointers like std::shared_ptr as well)*.

1. If you find you only use a `GC::ptr` instance to point to another object for normal pointer logic (and if you know that reference isn't isn't the only reference to said object) you should use `GC::ptr<T>*` or `GC::ptr<T>&` instead. This still lets you refer to the `GC::ptr<T>` object (and what it points to) but doesn't require unnecessary increments/decrements on each and every assignment to/from it. This is demonstrated in the example above, where the end-of-list pointer was a raw pointer to gc pointer.

