all: doc

.PHONY: all doc clean

doc: output

changelog.md: ../CHANGELOG.md
	sed -e 's/# Changelog/# Changelog {#changelog}/' < $< > $@

output: Doxyfile changelog.md
	rm -rf $@
	doxygen Doxyfile

clean:
	rm -rf output changelog.md
