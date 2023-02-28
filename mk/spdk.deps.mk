#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

.PRECIOUS: $(OBJS)

-include $(OBJS:.o=.d)
