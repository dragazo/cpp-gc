# cpp-gc

One big complaint I've seen from C++ newbies is that the language doesn't have automatic garbage collection *(though I'd argue that's actually a feature)*. Of course, there's always [`std::unique_ptr`](https://en.cppreference.com/w/cpp/memory/unique_ptr) for *(arguably)* most cases. There's even [`std::shared_ptr`](https://en.cppreference.com/w/cpp/memory/shared_ptr) if you need shared-access to managed resources. However, even `std::shared_ptr` fails to handle referential cycles. You could always refactor your entire data structure to use [`std::weak_ptr`](https://en.cppreference.com/w/cpp/memory/weak_ptr), but at that point you're doing all the work manually anyway. I wonder if all this is leading up to a solution?

`cpp-gc` is a **self-managed**, **thread-safe** garbage collection library written in **standard C++14**.

With `cpp-gc` in place, all you'd need to do to fix the above example is change all your `std::shared_ptr<T>` to `GC::ptr<T>`. The rest of the logic will take care of itself automatically *(with one exception - see below)*.

## How it Works

`cpp-gc` is designed to be **streamlined and minimalistic**. Very few things are visible to the user. The most important things to know are:
* `GC` - Static class containing types and functions that help you **manage your memory conveniently**.
* `GC::ptr<T>` - The shining star of `cpp-gc` - represents an **autonomous garbage-collected pointer**.
* `GC::make<T>(Args...)` - This is how you create an instance of `GC::ptr<T>` - it's used exactly like `std::make_shared`.
* `GC::collect()` - Triggers a full garbage collection pass *(see below)*.

When you allocate an object via `GC::make<T>(Args...)` it creates a new garbage-collected object with a reference count of 1. Just like `std::shared_ptr`, it will automatically manage the reference count and delete the object **immediately** when the reference count hits zero. What does this mean? Well this means if `std::shared_ptr` worked for you before, it'll work for you now exactly the same *(though a bit slower due to having extra things to manage)*.

`GC::collect()` triggers a full garbage collection pass, which accounts for cycles using the typical mark-and-sweep algorithm. This is rather slow compared to the other method `cpp-gc` uses to manage non-cyclic references, but is required if you do in fact have cycles. So when should you call it? Probably never. I'll explain:

## GC Strategy ##

`cpp-gc` has several "strategy" settings for automatically deciding when to perform a full garbage collect pass. This is controlled by a bitfield enum called `GC::strategy`. The default strategy is time-based garbage collection, which *(by default)* will call `GC::collect()` once per minute in a background thread.

Here's a list of strategy helpers:
* `GC::get_strategy()` / `GC::set_strategy()` - Gets or sets the current strategy. Mutiple strategy options can be bitwise-or'd together and all included options will be used.
* `GC::get_sleep_time()` / `GC::set_sleep_time()` - Gets or sets the amount of time to wait after the completion of one timed collect and the start of the next. This is ignored if the timed collect strategy flag is not set.

Typically, if you want to use custom settings, you should set these options up as soon as possible on program start and not modify them again.

## Limitations and Requirements

Other languages that implement garbage collection have it built right into the language and the compiler handles all the nasty bits for you. For instance, one piece of information a garbage collector needs to know is the set of all outgoing garbage-collected pointers from a struct. Because this is a *library* and not a compiler extension, I don't have the luxury of peeking inside your struct and poking around for the right types. Because of this, if you have a struct that owns a `GC::ptr` instance, you need to specify that explicitly. So how do you do this?

When `cpp-gc` wants to poll your object for `GC::ptr` instances, it calls `GC::router<T>::route()`, passing the object in question and a function object. What you need to do is call `GC::route()` with every data element you own that either is or may itself own a `GC::ptr` instance. Think of this as your object's reachability: i.e. everything you route to is reachable, and anything you leave out is unreachable.

The default implementation of `GC::router<T>::route()` is sufficient for any type that does not own (directly or indirectly) a `GC::ptr` instance.

This will be demonstrated in our examples:

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
