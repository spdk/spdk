# Message Passing and Concurrency {#concurrency}

## Theory

One of the primary aims of SPDK is to scale linearly with the addition of
hardware. This can mean many things in practice. For instance, moving from one
SSD to two should double the number of I/O's per second. Or doubling the number
of CPU cores should double the amount of computation possible. Or even doubling
the number of NICs should double the network throughput. To achieve this, the
software's threads of execution must be independent from one another as much as
possible. In practice, that means avoiding software locks and even atomic
instructions.

Traditionally, software achieves concurrency by placing some shared data onto
the heap, protecting it with a lock, and then having all threads of execution
acquire the lock only when accessing the data. This model has many great
properties:

* It's easy to convert single-threaded programs to multi-threaded programs
  because you don't have to change the data model from the single-threaded
  version. You add a lock around the data.
* You can write your program as a synchronous, imperative list of statements
  that you read from top to bottom.
* The scheduler can interrupt threads, allowing for efficient time-sharing
  of CPU resources.

Unfortunately, as the number of threads scales up, contention on the lock around
the shared data does too. More granular locking helps, but then also increases
the complexity of the program. Even then, beyond a certain number of contended
locks, threads will spend most of their time attempting to acquire the locks and
the program will not benefit from more CPU cores.

SPDK takes a different approach altogether. Instead of placing shared data in a
global location that all threads access after acquiring a lock, SPDK will often
assign that data to a single thread. When other threads want to access the data,
they pass a message to the owning thread to perform the operation on their
behalf. This strategy, of course, is not at all new. For instance, it is one of
the core design principles of
[Erlang](http://erlang.org/download/armstrong_thesis_2003.pdf) and is the main
concurrency mechanism in [Go](https://tour.golang.org/concurrency/2). A message
in SPDK consists of a function pointer and a pointer to some context. Messages
are passed between threads using a
[lockless ring](http://dpdk.org/doc/guides/prog_guide/ring_lib.html). Message
passing is often much faster than most software developer's intuition leads them
to believe due to caching effects. If a single core is accessing the same data
(on behalf of all of the other cores), then that data is far more likely to be
in a cache closer to that core. It's often most efficient to have each core work
on a small set of data sitting in its local cache and then hand off a small
message to the next core when done.

In more extreme cases where even message passing may be too costly, each thread
may make a local copy of the data. The thread will then only reference its local
copy. To mutate the data, threads will send a message to each other thread
telling them to perform the update on their local copy. This is great when the
data isn't mutated very often, but is read very frequently, and is often
employed in the I/O path. This of course trades memory size for computational
efficiency, so it is used in only the most critical code paths.

## Message Passing Infrastructure

SPDK provides several layers of message passing infrastructure. The most
fundamental libraries in SPDK, for instance, don't do any message passing on
their own and instead enumerate rules about when functions may be called in
their documentation (e.g. @ref nvme). Most libraries, however, depend on SPDK's
[thread](http://www.spdk.io/doc/thread_8h.html)
abstraction, located in `libspdk_thread.a`. The thread abstraction provides a
basic message passing framework and defines a few key primitives.

First, `spdk_thread` is an abstraction for a lightweight, stackless thread of
execution. A lower level framework can execute an `spdk_thread` for a single
timeslice by calling `spdk_thread_poll()`. A lower level framework is allowed to
move an `spdk_thread` between system threads at any time, as long as there is
only a single system thread executing `spdk_thread_poll()` on that
`spdk_thread` at any given time. New lightweight threads may be created at any
time by calling `spdk_thread_create()` and destroyed by calling
`spdk_thread_destroy()`. The lightweight thread is the foundational abstraction for
threading in SPDK.

There are then a few additional abstractions layered on top of the
`spdk_thread`. One is the `spdk_poller`, which is an abstraction for a
function that should be repeatedly called on the given thread. Another is an
`spdk_msg_fn`, which is a function pointer and a context pointer, that can
be sent to a thread for execution via `spdk_thread_send_msg()`.

The library also defines two additional abstractions: `spdk_io_device` and
`spdk_io_channel`. In the course of implementing SPDK we noticed the same
pattern emerging in a number of different libraries. In order to implement a
message passing strategy, the code would describe some object with global state
and also some per-thread context associated with that object that was accessed
in the I/O path to avoid locking on the global state. The pattern was clearest
in the lowest layers where I/O was being submitted to block devices. These
devices often expose multiple queues that can be assigned to threads and then
accessed without a lock to submit I/O. To abstract that, we generalized the
device to `spdk_io_device` and the thread-specific queue to `spdk_io_channel`.
Over time, however, the pattern has appeared in a huge number of places that
don't fit quite so nicely with the names we originally chose. In today's code
`spdk_io_device` is any pointer, whose uniqueness is predicated only on its
memory address, and `spdk_io_channel` is the per-thread context associated with
a particular `spdk_io_device`.

The threading abstraction provides functions to send a message to any other
thread, to send a message to all threads one by one, and to send a message to
all threads for which there is an io_channel for a given io_device.

Most critically, the thread abstraction does not actually spawn any system level
threads of its own. Instead, it relies on the existence of some lower level
framework that spawns system threads and sets up event loops. Inside those event
loops, the threading abstraction simply requires the lower level framework to
repeatedly call `spdk_thread_poll()` on each `spdk_thread()` that exists. This
makes SPDK very portable to a wide variety of asynchronous, event-based
frameworks such as [Seastar](https://www.seastar.io) or [libuv](https://libuv.org/).

## The event Framework

The SPDK project didn't want to officially pick an asynchronous, event-based
framework for all of the example applications it shipped with, in the interest
of supporting the widest variety of frameworks possible. But the applications do
of course require something that implements an asynchronous event loop in order
to run, so enter the `event` framework located in `lib/event`. This framework
includes things like polling and scheduling the lightweight threads, installing
signal handlers to cleanly shutdown, and basic command line option parsing.
Only established applications should consider directly integrating the lower
level libraries.

## Limitations of the C Language

Message passing is efficient, but it results in asynchronous code.
Unfortunately, asynchronous code is a challenge in C. It's often implemented by
passing function pointers that are called when an operation completes. This
chops up the code so that it isn't easy to follow, especially through logic
branches. The best solution is to use a language with support for
[futures and promises](https://en.wikipedia.org/wiki/Futures_and_promises),
such as C++, Rust, Go, or almost any other higher level language. However, SPDK is a low
level library and requires very wide compatibility and portability, so we've
elected to stay with plain old C.

We do have a few recommendations to share, though. For _simple_ callback chains,
it's easiest if you write the functions from bottom to top. By that we mean if
function `foo` performs some asynchronous operation and when that completes
function `bar` is called, then function `bar` performs some operation that
calls function `baz` on completion, a good way to write it is as such:

```c
    void baz(void *ctx) {
            ...
    }

    void bar(void *ctx) {
            async_op(baz, ctx);
    }

    void foo(void *ctx) {
            async_op(bar, ctx);
    }
```

Don't split these functions up - keep them as a nice unit that can be read from bottom to top.

For more complex callback chains, especially ones that have logical branches
or loops, it's best to write out a state machine. It turns out that higher
level languages that support futures and promises are just generating state
machines at compile time, so even though we don't have the ability to generate
them in C we can still write them out by hand. As an example, here's a
callback chain that performs `foo` 5 times and then calls `bar` - effectively
an asynchronous for loop.

```c
    enum states {
            FOO_START = 0,
            FOO_END,
            BAR_START,
            BAR_END
    };

    struct state_machine {
            enum states state;

            int count;
    };

    static void
    foo_complete(void *ctx)
    {
        struct state_machine *sm = ctx;

        sm->state = FOO_END;
        run_state_machine(sm);
    }

    static void
    foo(struct state_machine *sm)
    {
        do_async_op(foo_complete, sm);
    }

    static void
    bar_complete(void *ctx)
    {
        struct state_machine *sm = ctx;

        sm->state = BAR_END;
        run_state_machine(sm);
    }

    static void
    bar(struct state_machine *sm)
    {
        do_async_op(bar_complete, sm);
    }

    static void
    run_state_machine(struct state_machine *sm)
    {
        enum states prev_state;

        do {
            prev_state = sm->state;

            switch (sm->state) {
                case FOO_START:
                    foo(sm);
                    break;
                case FOO_END:
                    /* This is the loop condition */
                    if (sm->count++ < 5) {
                        sm->state = FOO_START;
                    } else {
                        sm->state = BAR_START;
                    }
                    break;
                case BAR_START:
                    bar(sm);
                    break;
                case BAR_END:
                    break;
            }
        } while (prev_state != sm->state);
    }

    void do_async_for(void)
    {
            struct state_machine *sm;

            sm = malloc(sizeof(*sm));
            sm->state = FOO_START;
            sm->count = 0;

            run_state_machine(sm);
    }
```

This is complex, of course, but the `run_state_machine` function can be read
from top to bottom to get a clear overview of what's happening in the code
without having to chase through each of the callbacks.
