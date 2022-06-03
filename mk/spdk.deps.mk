#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

.PRECIOUS: $(OBJS)

-include $(OBJS:.o=.d)
