# SPDK CLI

CLI management application for SPDK.

## How to start using
Please follow steps start using SPDKCLI.
You will need a root account or sudo privileges in order to use it.

### Create Python virtual environment (optional)
You can use Python virtual environment if you don't want to litter your
system Python installation.
~~~{.sh}
cd spdk
mkdir venv
virtualenv-3 ./venv
source ./venv/bin/activate
~~~
Tip: if you are using "sudo" instead of root account, it is suggested to do
"sudo -s" before activating the environment. This is because venv might not work
correctly when calling spdkcli with sudo, like "sudo python spdkcli.py" -
some environment variables might not be passed and you will experience errors/

### Install needed dependencies
~~~{.sh}
pip install configshell-fb
~~~

### Run SPDK application instance
~~~{.sh}
./scripts/setup.sh
./app/vhost/vhost -c vhost.conf
~~~

### Run SPDKCLI
~~~{.sh}
(venv) spdkcli/spdkcli.py
~~~

Use "help" command to get a list of available commands.
