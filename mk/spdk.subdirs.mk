#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

ALL_DEPDIRS := $(patsubst DEPDIRS-%,%,$(filter DEPDIRS-%,$(.VARIABLES)))

define depdirs_rule
$(DEPDIRS-$(1)):

$(1): | $(DEPDIRS-$(1))

endef

$(DIRS-y) :
	$(Q)$(MAKE) -C $@ S=$S$(S:%=/)$@ $(MAKECMDGOALS)

$(foreach dir,$(ALL_DEPDIRS),$(eval $(call depdirs_rule,$(dir))))

install: all $(DIRS-y)

uninstall: $(DIRS-y)
