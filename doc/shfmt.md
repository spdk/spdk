# shfmt {#shfmt}

## In this document {#shfmt_toc}

* @ref shfmt_overview
* @ref shfmt_usage
* @ref shfmt_installation
* @ref shfmt_examples

## Overview {#shfmt_overview}

The majority of tests (and scripts overall) in the SPDK repo are written
in Bash (with a quite significant emphasis on "Bashism"), thus a style
formatter, shfmt, was introduced to help keep the .sh code consistent
across the entire repo. For more details on the tool itself, please see
[shfmt](https://github.com/mvdan/sh).

We also advise to use 4.4 Bash as a minimum version to make sure scripts
across the whole repo work as intended.

## Usage {#shfmt_usage}

On the CI pool, the shfmt is run against all the updated .sh files that
have been committed but not merged yet. Additionally, shfmt will pick
all .sh present in the staging area when run locally from our pre-commit
hook (via check_format.sh). In case any style errors are detected, a
patch with needed changes is going to be generated and either build (CI)
or the commit will be aborted. Said patch can be then easily applied:

~~~{.sh}
# Run from the root of the SPDK repo
patch --merge -p0 <shfmt-3.1.0.patch
~~~

The name of the patch is derived from the version of shfmt that is
currently in use (3.1.0 is currently supported).

Please, see ./scripts/check_format.sh for all the arguments the shfmt
is run with. Additionally, @ref shfmt_examples has more details on how
each of the arguments behave.

## Installation {#shfmt_installation}

The shfmt can be easily installed via pkgdep.sh:

~~~{.sh}
./scripts/pkgdep.sh -d
~~~

This will install all the developers tools, including shfmt, on the
local system. The precompiled binary will be saved, by default, to
/opt/shfmt and then linked under /usr/bin. Both paths can be changed
by setting SHFMT_DIR and SHFMT_DIR_OUT in the environment. Example:

~~~{.sh}
SHFMT_DIR=/keep_the_binary_here \
SHFMT_DIR_OUT=/and_link_it_here \
  ./scripts/pkgdep.sh -d
~~~

## Examples {#shfmt_examples}

~~~{.sh}
#######################################
if foo=$(bar); then
  echo "$foo"
fi

exec "$foo" \
  --bar \
  --foo

# indent_style = tab

if foo=$(bar); then
        echo "$foo"
fi

exec foobar \
        --bar \
        --foo
######################################
if foo=$(bar); then
        echo "$foo" && \
        echo "$(bar)"
fi
# binary_next_line = true
if foo=$(bar); then
        echo "$foo" \
                && echo "$(bar)"
fi

# Note that each break line is also being indented:

if [[ -v foo ]] \
&& [[ -v bar ]] \
&& [[ -v foobar ]]; then
	echo "This is foo"
fi
# ->
if [[ -v foo ]] \
        && [[ -v bar ]] \
        && [[ -v foobar ]]; then
        echo "This is foo"
fi

# Currently, newlines are being escaped even if syntax-wise
# they are not needed, thus watch for the following:
if [[ -v foo
        && -v bar
        && -v foobar ]]; then
        echo "This is foo"
fi
#->
if [[ -v foo && -v \
        bar && -v \
        foobar ]]; then
        echo "This is foo"
fi
# This, unfortunately, also breaks the -bn behavior.
# (see https://github.com/mvdan/sh/issues/565) for details.
######################################
case "$FOO" in
        BAR)
        echo "$FOO" ;;
esac
# switch_case_indent = true
case "$FOO" in
        BAR)
                echo "$FOO"
                ;;
esac
######################################
exec {foo}>bar
:>foo
exec {bar}<foo
# -sr
exec {foo}> bar
: > foo
exec {bar}< foo
######################################
# miscellaneous, enforced by shfmt
(( no_spacing_at_the_beginning & ~and_no_spacing_at_the_end ))
: $(( no_spacing_at_the_beginning & ~and_no_spacing_at_the_end ))

# ->
((no_spacing_at_the_beginning & ~and_no_spacing_at_the_end))
: $((no_spacing_at_the_beginning & ~and_no_spacing_at_the_end))
~~~
