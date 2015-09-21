$(DIRS-y) :
	@echo "== $S/$@ ($(MAKECMDGOALS))"
	$(Q)$(MAKE) -C $@ S=$S/$@ $(MAKECMDGOALS) $(MAKESUBDIRFLAGS)
