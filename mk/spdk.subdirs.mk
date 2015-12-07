$(DIRS-y) :
	@echo "== $S/$@ ($(MAKECMDGOALS))"
	$(Q)$(MAKE) -e -C $@ S=$S/$@ $(MAKECMDGOALS) $(MAKESUBDIRFLAGS)
