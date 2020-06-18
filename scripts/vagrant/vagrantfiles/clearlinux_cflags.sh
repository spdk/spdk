#!/usr/bin/env bash

cat <<- FLAGS >> /etc/profile.d/clearcflags.sh
	export CFLAGS=
	export CFFLAGS=
	export CXXFLAGS=
	export FFLAGS=
	export THEANO_FLAGS=
FLAGS
