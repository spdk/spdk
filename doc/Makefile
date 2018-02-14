all: doc
	@:

.PHONY: all doc clean

doc: output

changelog.md: ../CHANGELOG.md
	sed -e 's/^# Changelog/# Changelog {#changelog}/' \
	    -e 's/^##/#/' \
	    -e 's/^# \(\(v..\...\):.*\)/# \1 {#changelog-\2}/' \
	    -e '/# v..\...:/s/\./-/2' \
	    < $< > $@

output: Doxyfile changelog.md $(wildcard *.md) $(wildcard ../include/spdk/*.h)
	rm -rf $@
	doxygen Doxyfile

clean:
	rm -rf output changelog.md
