# SPDK CLI

CLI management application for SPDK.

## How to start using
Please follow steps start using SPDKCLI.
You will need a root account or sudo privileges in order to use it.

### Install needed dependencies
All dependencies should be handled by scirpts/pkgdep.sh script.
Package dependencies at the moment include:
 - configshell 

### Run SPDK application instance
~~~{.sh}
./scripts/setup.sh
./app/vhost/vhost -c vhost.conf
~~~

### Run SPDK CLI
In order to use SPDK CLI in interactive mode please use:
~~~{.sh}
spdkcli/spdkcli.py
~~~
Use "help" command to get a list of available commands for each tree node.

It is also possible to use SPDK CLI to run just a single command,
just use the command as an argument to the application.
For example, to view current configuration and immediately exit:
 ~~~{.sh}
spdkcli/spdkcli.py ls
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

Then install the dependencies using pip. That way depedencies will be
installed only inside the virtual environment.
~~~{.sh}
(venv) pip install configshell-fb
~~~

Tip: if you are using "sudo" instead of root account, it is suggested to do
"sudo -s" before activating the environment. This is because venv might not work
correctly when calling spdkcli with sudo, like "sudo python spdkcli.py" -
some environment variables might not be passed and you will experience errors.





