# cpp-gc

One big complaint I've seen from C++ newbies is that the language doesn't have automatic garbage collection *(though I'd argue that's actually a feature)*. Of course, there's always [`std::unique_ptr`](https://en.cppreference.com/w/cpp/memory/unique_ptr) for *(arguably)* most cases. There's even [`std::shared_ptr`](https://en.cppreference.com/w/cpp/memory/shared_ptr) if you need shared-access to managed resources. Of course, even `std::shared_ptr` fails to handle referential cycles. You could always refactor your entire data structure to use [`std::weak_ptr`](https://en.cppreference.com/w/cpp/memory/weak_ptr), but at that point you're doing all the work manually anyway. I wonder if all this is leading up to a solution?

`cpp-gc` is a **self-managed**, **thread-safe** garbage collection library written in **standard C++**.

With `cpp-gc` in place, all you'd need to do to fix the above example is change all your `std::shared_ptr<T>` to `GC::ptr<T>`. The rest of the logic will take care of itself automatically *(with one exception - see below)*.

# How it Works

`cpp-gc` is designed to be **streamlined and minimalistic**. Very few things are visible to the user. The most important things to know are:
* `GC` - static class containing types and functions that help you **manage your memory conveniently**.
* `GC::ptr<T>` - the shining star of `cpp-gc` - represents an **autonomous garbage-collected pointer**.
* `GC::make<T>(Args...)` - this is how you create an instance of `GC::ptr<T>` - it's used exactly like `std::make_shared`.
* `GC::collect()` - triggers a full garbage collection pass *(see below)*.

When you allocate an object via `GC::make<T>(Args...)` it creates a garbage-collected object with a reference count of 1. Just like `std::shared_ptr`, it will automatically manage the reference count and delete the object **immediately** when the reference count hits zero. What does this mean? Well this means if `std::shared_ptr` worked for you before, it'll work for you now exactly the same *(though a tiny bit slower due to having extra things to manage)* and you'll never need to call `GC::collect()` at all. So why does `GC::collect()` exist?

`GC::collect()` triggers a full garbage collection pass, which accounts for cycles using the typical mark-and-sweep algorithm. This is rather slow compared to the other method `cpp-gc` uses to manage non-cyclic references, but is required if you do in fact have cycles. So when should you call it? Whenever you want really, that's why it's a separate function after all. In fact, because of the reference-counting described above you might not ever need to call it. To simulate what other languages like Java and C# do, you could make a background thread whose only job is to call `GC::collect()` every couple of minutes (or whatever logic you want). It's really up to you, but just remember it's required to deallocate cyclical references.

Here's the good news: `cpp-gc` is threadsafe, which it accomplishes by locking a mutex for core gc operations (e.g. bumping up/down a reference count, allocation, collect, etc.). However, this also means you can run a `GC::collect()` from another thread and continue to work on whatever you want. So long as you don't trigger a gc operation, you won't be blocked (in general, just don't create or destroy `GC::ptr` objects).

# The Downside

Other languages that implement garbage collection have it built right into the language and the compiler handles all the nasty bits for you. For instance, one piece of information a garbage collector needs to know is the relative address of each garbage-collected pointer in a struct. Because this is a *library* and not a compiler extension, I don't have the luxury of peeking inside your struct and poking around for the right types. Because of this, if you have a struct that contains a `GC::ptr` instance by value, you need to specify that explicitly. So how do you do this?

`GC::outgoing_t` is a typedef for a pair of begin/end iterators into an array of byte offsets in a given object. `GC::outgoing<T>` is the standardized way by which `cpp-gc` does this. When you call `GC::make<T>()`, it automatically uses `GC::outgoing<T>::get()` to fetch info on the "outgoing" `GC::ptr` instances from `T`. The default implementation of `GC::outgoing<T>` returns an empty iterator range, which is sufficient for any type that does not contain a `GC::ptr` by value. If this is not the case, you need to specialize `GC::outgoing<T>()` for your type `T`.

This will be demonstrated in the examples below:

# Examples

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

Because this contains `GC::ptr` objects by value, we need to specialize `GC::outgoing` for our type.

```cpp
template<>
struct GC::outgoing<ListNode>
{
    static GC::outgoing_t get()
    {
        static const std::size_t offsets[] = {offsetof(ListNode, prev), offsetof(ListNode, next)};
        return {std::begin(offsets), std::end(offsets)};
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

If you run this, you'll find none of the objects are deallocated. This is because we created a bunch of cycles due to making a *doubly*-linked list. As stated above, `cpp-gc` cleans this up via `GC::collect()`. Let's do that now:

```cpp
// the function that called foo()
void bar()
{
    // we need to call foo()
    foo();
    
    std::cerr << "\n\ncalling collect():\n\n";
    
    // but you know what, just to be safe, let's clean up any objects it left lying around unused
    GC::collect();
}
```

As soon as `GC::collect()` is executed, all the cycles will be dealt with and you should get a lot of messages from destructors. It's that easy. And, as already explained above, you don't really need to run `GC::collect()` very often - only if you really need to.

But you know what? `cpp-gc` is far more powerful than that. In fact, it's completely thread safe, so you might find it more performant to write the above function another way:

```cpp
// the function that called foo()
void async_bar()
{
    // we need to call foo()
    foo();
    
    std::cerr << "\n\ncalling collect():\n\n";
    
    // run GC::collect in another thread while we work on other stuff
    std::thread(GC::collect).detach();
    
    // ... do other stuff - anything not involving gc operations will not block ... //
}
```

