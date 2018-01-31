# Message Passing and Concurrency {#concurrency}

# Theory

One of the primary aims of SPDK is to scale linearly with the addition of
hardware. This can mean a number of things in practice. For instance, moving
from one SSD to two should double the number of I/O's per second. Or doubling
the number of CPU cores should double the amount of computation possible. Or
even doubling the number of NICs should double the network throughput. To
achieve this, the software must be designed such that threads of execution are
independent from one another as much as possible. In practice, that means
avoiding software locks and even atomic instructions.

Traditionally, software achieves concurrency by placing some shared data onto
the heap, protecting it with a lock, and then having all threads of execution
acquire the lock only when that shared data needs to be accessed. This model
has a number of great properties:

* It's relatively easy to convert single-threaded programs to multi-threaded
programs because you don't have to change the data model from the
single-threaded version. You just add a lock around the data.
* You can write your program as a synchronous, imperative list of statements
that you read from top to bottom.
* Your threads can be interrupted and put to sleep by the operating system
scheduler behind the scenes, allowing for efficient time-sharing of CPU resources.

Unfortunately, as the number of threads scales up, contention on the lock
around the shared data does too. More granular locking helps, but then also
greatly increases the complexity of the program. Even then, beyond a certain
number highly contended locks, threads will spend most of their time
attempting to acquire the locks and the program will not benefit from any
additional CPU cores.

SPDK takes a different approach altogether. Instead of placing shared data in a
global location that all threads access after acquiring a lock, SPDK will often
assign that data to a single thread. When other threads want to access the
data, they pass a message to the owning thread to perform the operation on
their behalf. This strategy, of course, is not at all new. For instance, it is
one of the core design principles of
[Erlang](http://erlang.org/download/armstrong_thesis_2003.pdf) and is the main
concurrency mechanism in [Go](https://tour.golang.org/concurrency/2). A message
in SPDK typically consists of a function pointer and a pointer to some context,
and is passed between threads using a
[lockless ring](http://dpdk.org/doc/guides/prog_guide/ring_lib.html). Message
passing is often much faster than most software developer's intuition leads them to
believe, primarily due to caching effects. If a single core is consistently
accessing the same data (on behalf of all of the other cores), then that data
is far more likely to be in a cache closer to that core. It's often most
efficient to have each core work on a relatively small set of data sitting in
its local cache and then hand off a small message to the next core when done.

In more extreme cases where even message passing may be too costly, a copy of
the data will be made for each thread. The thread will then only reference its
local copy. To mutate the data, threads will send a message to each other
thread telling them to perform the update on their local copy. This is great
when the data isn't mutated very often, but may be read very frequently, and is
often employed in the I/O path. This of course trades memory size for
computational efficiency, so it's use is limited to only the most critical code
paths.

# Message Passing Infrastructure

SPDK provides several layers of message passing infrastructure. The most
fundamental libraries in SPDK, for instance, don't do any message passing on
their own and instead enumerate rules about when functions may be called in
their documentation (e.g. @ref nvme). Most libraries, however, depend on SPDK's
[io_channel](http://www.spdk.io/doc/io__channel_8h.html) infrastructure,
located in `libspdk_util.a`. The io_channel infrastructure is an abstraction
around a basic message passing framework and defines a few key abstractions.

First, spdk_thread is an abstraction for a thread of execution and
spdk_poller is an abstraction for a function that should be
periodically called on the given thread. On each system thread that the user
wishes to use with SPDK, they must first call spdk_allocate_thread(). This
function takes three function pointers - one that will be called to pass a
message to this thread, one that will be called to request that a poller be
started on this thread, and finally one to request that a poller be stopped.
*The implementation of these functions is not provided by this library*. Many
applications already have facilities for passing messages, so to ease
integration with existing code bases we've left the implementation up to the
user. However, for users starting from scratch, see the following section on
the event framework for an SPDK-provided implementation.

The library also defines two other abstractions: spdk_io_device and
spdk_io_channel. In the course of implementing SPDK we noticed the
same pattern emerging in a number of different libraries. In order to
implement a message passing strategy, the code would describe some object with
global state and also some per-thread context associated with that object that
was accessed in the I/O path to avoid locking on the global state. The pattern
was clearest in the lowest layers where I/O was being submitted to block
devices. These devices often expose multiple queues that can be assigned to
threads and then accessed without a lock to submit I/O. To abstract that, we
generalized the device to spdk_io_device and the thread-specific queue to
spdk_io_channel. Over time, however, the pattern has appeared in a huge
number of places that don't fit quite so nicely with the names we originally
chose. In today's code spdk_io_device is any pointer, whose uniqueness is
predicated only on its memory address, and spdk_io_channel is the per-thread
context associated with a particular spdk_io_device.

The io_channel infrastructure provides functions to send a message to any other
thread, to send a message to all threads one by one, and to send a message to
all threads for which there is an io_channel for a given io_device.

# The event Framework

As the number of example applications in SPDK grew, it became clear that a
large portion of the code in each was implementing the basic message passing
infrastructure required to call spdk_allocate_thread(). This includes spawning
one thread per core, pinning each thread to a unique core, and allocating
lockless rings between the threads for message passing. Instead of
re-implementing that infrastructure for each example application, SPDK
provides the SPDK @ref event. This library handles setting up all of the
message passing infrastructure, installing signal handlers to cleanly
shutdown, implements periodic pollers, and does basic command line parsing.
When started through spdk_app_start(), the library automatically spawns all of
the threads requested, pins them, and calls spdk_allocate_thread() with
appropriate function pointers for each one. This makes it much easier to
implement a brand new SPDK application and is the recommended method for those
starting out. Only established applications with sufficient message passing
infrastructure should consider directly integrating the lower level libraries.

# Limitations of the C Language

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

    void baz(void *ctx) {
            ...
    }

    void bar(void *ctx) {
            async_op(baz, ctx);
    }

    void foo(void *ctx) {
            async_op(bar, ctx);
    }

Don't split these functions up - keep them as a nice unit that can be read from bottom to top.

For more complex callback chains, especially ones that have logical branches
or loops, it's best to write out a state machine. It turns out that higher
level langauges that support futures and promises are just generating state
machines at compile time, so even though we don't have the ability to generate
them in C we can still write them out by hand. As an example, here's a
callback chain that performs `foo` 5 times and then calls `bar` - effectively
an asynchronous for loop.

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

This is complex, of course, but the `run_state_machine` function can be read
from top to bottom to get a clear overview of what's happening in the code
without having to chase through each of the callbacks.
