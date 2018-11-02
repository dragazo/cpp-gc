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

# Examples

