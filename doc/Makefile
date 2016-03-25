all: doc

.PHONY: all doc clean

doc: output

output: Doxyfile
	rm -rf $@
	doxygen Doxyfile

clean:
	rm -rf output
