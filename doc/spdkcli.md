# spdkcli {#spdkcli}

spdkcli is a command-line management application for SPDK.
spdkcli has support for most transport protocols and
bdev modules.

## Dependencies

Dependencies are installed by scripts/pkgdep.sh.
Package dependencies at the moment include:

- configshell

Some distributions name the package configshell_fb.

## Example usage

### Start SPDK application

~~~{.sh}
./scripts/setup.sh
./build/bin/vhost -c vhost.json
~~~

### Run spdkcli

spdkcli should be run with the same privileges as SPDK application.

To use spdkcli in interactive mode:
~~~{.sh}
scripts/spdkcli.py
~~~
Use "help" command to get a list of available commands for each tree node.

It is also possible to use spdkcli to run just a single command,
just use the command as an argument to the application.
For example, to view current configuration and immediately exit:
~~~{.sh}
scripts/spdkcli.py ls
~~~

## Optional - create Python virtual environment

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
