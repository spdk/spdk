.PRECIOUS: $(OBJS)

-include $(OBJS:.o=.d)
