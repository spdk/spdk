# Event Framework {#event}

SPDK provides a framework for writing asynchronous, polled-mode,
shared-nothing server applications. The event framework is intended to be
optional; most other SPDK components are designed to be integrated into an
application without specifically depending on the SPDK event library. The
framework defines several concepts - reactors, events, and pollers - that are
described in the following sections. The event framework spawns one thread per
core (reactor) and connects the threads with lockless queues. Messages
(events) can then be passed between the threads. On modern CPU architectures,
message passing is often much faster than traditional locking. For a
discussion of the theoretical underpinnings of this framework, see @ref
concurrency.

The event framework public interface is defined in event.h.

# Event Framework Design Considerations {#event_design}

Simple server applications can be written in a single-threaded fashion. This
allows for straightforward code that can maintain state without any locking or
other synchronization. However, to scale up (for example, to allow more
simultaneous connections), the application may need to use multiple threads.
In the ideal case where each connection is independent from all other
connections, the application can be scaled by creating additional threads and
assigning connections to them without introducing cross-thread
synchronization. Unfortunately, in many real-world cases, the connections are
not entirely independent and cross-thread shared state is necessary. SPDK
provides an event framework to help solve this problem.

# SPDK Event Framework Components {#event_components}

## Events {#event_component_events}

To accomplish cross-thread communication while minimizing synchronization
overhead, the framework provides message passing in the form of events. The
event framework runs one event loop thread per CPU core. These threads are
called reactors, and their main responsibility is to process incoming events
from a queue. Each event consists of a bundled function pointer and its
arguments, destined for a particular CPU core. Events are created using
spdk_event_allocate() and executed using spdk_event_call(). Unlike a
thread-per-connection server design, which achieves concurrency by depending
on the operating system to schedule many threads issuing blocking I/O onto a
limited number of cores, the event-driven model requires use of explicitly
asynchronous operations to achieve concurrency. Asynchronous I/O may be issued
with a non-blocking function call, and completion is typically signaled using
a callback function.

## Reactors {#event_component_reactors}

Each reactor has a lock-free queue for incoming events to that core, and
threads from any core may insert events into the queue of any other core. The
reactor loop running on each core checks for incoming events and executes them
in first-in, first-out order as they are received. Event functions should
never block and should preferably execute very quickly, since they are called
directly from the event loop on the destination core.

## Pollers {#event_component_pollers}

The framework also defines another type of function called a poller. Pollers
may be registered with the spdk_poller_register() function. Pollers, like
events, are functions with arguments that can be bundled and executed.
However, unlike events, pollers are executed repeatedly until unregistered and
are executed on the thread they are registered on. The reactor event loop
intersperses calls to the pollers with other event processing. Pollers are
intended to poll hardware as a replacement for interrupts. Normally, pollers
are executed on every iteration of the main event loop. Pollers may also be
scheduled to execute periodically on a timer if low latency is not required.

## Application Framework {#event_component_app}

The framework itself is bundled into a higher level abstraction called an "app". Once
spdk_app_start() is called, it will block the current thread until the application
terminates by calling spdk_app_stop() or an error condition occurs during the
initialization code within spdk_app_start(), itself, before invoking the caller's
supplied function.
