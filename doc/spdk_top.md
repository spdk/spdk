# spdk_top {#spdk_top}

The spdk_top application is designed to resemble the standard top in that it provides a real-time insights into CPU cores usage by SPDK
lightweight threads and pollers. Have you ever wondered which CPU core is used most by your SPDK instance? Are you building your own bdev
or library and want to know if your code is running efficiently? Are your new pollers busy most of the time? The spdk_top application uses
RPC calls to collect performance metrics and displays them in a report that you can analyze and determine if your code is running efficiently
so that you can tune your implementation and get more from SPDK.

Why doesn't the classic top utility work for SPDK? SPDK uses a polled-mode design; a reactor thread running on each CPU core assigned to
an SPDK application schedules SPDK lightweight threads and pollers to run on the CPU core. Therefore, the standard Linux top utility is
not effective for analyzing the CPU usage for polled-mode applications like SPDK because it just reports that they are using 100% of the
CPU resources assigned to them. The spdk_top utility was developed to analyze and report the CPU cycles used to do real work vs just
polling for work. The utility relies on instrumentation added to pollers to track when they are doing work vs. polling for work. The
spdk_top utility gets the fine grained metrics from the pollers, analyzes and report the metrics on a per poller, thread and core basis.
This information enables users to identify CPU cores that are busy doing real work so that they can determine if the application
needs more or less CPU resources.

## Run spdk_top

Before running spdk_top you need to run the SPDK application whose performance you want to analyze using spdk_top.

Run the spdk_top application

~~~{.sh}
./build/bin/spdk_top
~~~

## Bottom menu

Menu at the bottom of SPDK top window shows many options for changing displayed data. Each menu item has a key associated with it in square brackets.

* Quit - quits the SPDK top application.
* Switch tab - allows to select THREADS/POLLERS/CORES tabs.
* Previous page/Next page - scrolls up/down to the next set of rows displayed. Indicator in the bottom-left corner shows current page and number
  of all available pages.
* Item details - displays details pop-up window for highlighted data row. Selection is changed by pressing UP and DOWN arrow keys.
* Help - displays help pop-up window.

## Threads Tab

The threads tab displays a line item for each spdk thread. The information displayed shows:

* Thread name - name of SPDK thread.
* Core - core on which the thread is currently running.
* Active/Timed/Paused pollers - number of pollers grouped by type on this thread.
* Idle/Busy - how many microseconds the thread was idle/busy.

\n
By pressing ENTER key a pop-up window appears, showing above and a list of pollers running on selected
thread (with poller name, type, run count and period).
Pop-up then can be closed by pressing ESC key.

To learn more about spdk threads see @ref concurrency.

## Pollers Tab

The pollers tab displays a line item for each poller. The information displayed shows:

* Poller name - name of currently selected poller.
* Type - type of poller (Active/Paused/Timed).
* On thread - thread on which the poller is running.
* Run count - how many times poller was run.
* Period - poller period in microseconds. If period equals 0 then it is not displayed.
* Status - whether poller is currently Busy (red color) or Idle (blue color).

\n
Poller pop-up window can be displayed by pressing ENTER on a selected data row and displays above information.
Pop-up can be closed by pressing ESC key.

## Cores Tab

The cores tab provides insights into how the application is using the CPU cores assigned to it. The information displayed for each core shows:

* Core - core number.
* Thread count - number of threads currently running on core.
* Poller count - total number of pollers running on core.
* Idle/Busy - how many microseconds core was idle (including time when core ran pollers but did not find any work) or doing actual work.
* Intr - whether this core is in interrupt mode or not.

\n
Pressing ENTER key makes a pop-up window appear, showing above information, along with a list of threads running on selected core. Cores details
window allows to select a thread and display thread details pop-up on top of it. To close both pop-ups use ESC key.

## Help Window

Help window pop-up can be invoked by pressing H key inside any tab. It contains explanations for each key used inside the spdk_top application.
