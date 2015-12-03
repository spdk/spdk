DOXYFILES = Doxyfile.ioat Doxyfile.nvme
OUTPUT_DIRS = $(patsubst Doxyfile.%,output.%,$(DOXYFILES))

all: doc

.PHONY: all doc clean

doc: $(OUTPUT_DIRS)

output.%: Doxyfile.%
	rm -rf $@
	doxygen $^

clean:
	rm -rf $(OUTPUT_DIRS)
