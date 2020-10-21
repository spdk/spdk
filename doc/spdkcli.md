# SPDK CLI {#spdkcli}

Spdkcli is a command-line management application for SPDK.
Spdkcli has support for a limited number of applications and bdev modules,
and should be considered experimental for the v18.04 release.
This experimental version was added for v18.04 to get early feedback
that can be incorporated as spdkcli becomes more fully-featured
for the next SPDK release.

### Install needed dependencies

All dependencies should be handled by scripts/pkgdep.sh script.
Package dependencies at the moment include:

 - configshell

### Run SPDK application instance

~~~{.sh}
./scripts/setup.sh
./build/bin/vhost -c vhost.json
~~~

### Run SPDK CLI

Spdkcli should be run with the same privileges as SPDK application.
In order to use SPDK CLI in interactive mode please use:
~~~{.sh}
scripts/spdkcli.py
~~~
Use "help" command to get a list of available commands for each tree node.

It is also possible to use SPDK CLI to run just a single command,
just use the command as an argument to the application.
For example, to view current configuration and immediately exit:
 ~~~{.sh}
scripts/spdkcli.py ls
~~~

### Optional - create Python virtual environment

You can use Python virtual environment if you don't want to litter your
system Python installation.

First create the virtual environment:
~~~{.sh}
cd spdk
mkdir venv
virtualenv-3 ./venv
source ./venv/bin/activate
~~~

Then install the dependencies using pip. That way dependencies will be
installed only inside the virtual environment.
~~~{.sh}
(venv) pip install configshell-fb
~~~

Tip: if you are using "sudo" instead of root account, it is suggested to do
"sudo -s" before activating the environment. This is because venv might not work
correctly when calling spdkcli with sudo, like "sudo python spdkcli.py" -
some environment variables might not be passed and you will experience errors.
