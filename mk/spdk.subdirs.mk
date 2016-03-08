$(DIRS-y) :
	@echo "== $S$(S:%=/)$@ ($(MAKECMDGOALS))"
	$(Q)$(MAKE) -e -C $@ S=$S$(S:%=/)$@ $(MAKECMDGOALS) $(MAKESUBDIRFLAGS)
