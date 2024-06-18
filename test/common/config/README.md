Scripts contained in this directory, and the pkgdep/, will attempt to provision
target distro with all dependencies required for running autotest suites located
under test/*. The scope of supported tests may vary across supported distros due
to either limitted selection of packages and/or hw-related requirements (e.g.
tests targeted for CMB, PMR, QAT and similar) or specific dependencies which are
not covered here (e.g. calsoft, vhost's testing VM, etc). That said, the latest
releases of Fedora should be capable of running all the test suites after being
fully provisioned via the pkgdep (other limitations may still apply).
