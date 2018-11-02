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

`GC::outgoing_t` is a typedef for a pair of begin/end iterators into an array of byte offsets in a given object. `GC::outgoing<T>()` is the standardized way by which `cpp-gc` does this. When you call `GC::make<T>()`, it automatically uses `GC::outgoing<T>()` to fetch info on the "outgoing" `GC::ptr` instances from `T`. The default implementation of `GC::outgoing<T>()` returns an empty iterator range, which is sufficient for any type that does not contain a `GC::ptr` by value. If this is not the case, you need to specialize `GC::outgoing<T>()` for your type `T`.

Here's an example:

```cpp
// a type that contains GC::ptr instances by value
struct foo
{
    GC::ptr<foo> prev;
    GC::ptr<foo> next;
};
// because of this, we need to specialize the GC::outgoing function
template<>
GC::outgoing_t GC::outgoing<foo>()
{
    static const std::size_t offsets[] = { offsetof(foo, prev), offsetof(foo, next) };
    return {std::begin(offsets), std::end(offsets)};
}

/* ... later on in the code ... */

void func()
{
    // once GC::outgoing is specialized, we can use GC::ptr and GC::make as usual
    GC::ptr<foo> ptr = GC::make<foo>();
}
```

Here's another example where the internal pointers are private

```cpp
// a type that contains GC::ptr instances by value
struct foo
{
private:
    GC::ptr<foo> prev;
    GC::ptr<foo> next;
    
    // we need to use a friend helper function so that it can look at our private data
    friend GC::outgoing_t foo_outgoing_helper();
public:
    // accessors/etc
};
GC::outgoing_t foo_outgoing_helper()
{
    static const std::size_t offsets[] = { offsetof(foo, prev), offsetof(foo, next) };
    return {std::begin(offsets), std::end(offsets)};
}
// specialize GC::outgoing for foo and use our helper function
template<> GC::outgoing_t GC::outgoing<foo>() { return foo_outgoing_helper(); }

/* ... later on in the code ... */

void func()
{
    // once GC::outgoing is specialized, we can use GC::ptr and GC::make as usual
    GC::ptr<foo> ptr = GC::make<foo>();
}
```

# Examples

