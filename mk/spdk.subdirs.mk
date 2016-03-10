$(DIRS-y) :
	$(Q)$(MAKE) -e -C $@ S=$S$(S:%=/)$@ $(MAKECMDGOALS) $(MAKESUBDIRFLAGS)
