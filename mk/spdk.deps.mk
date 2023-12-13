#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

.PRECIOUS: $(OBJS)

# workaround for GNU Make 4.4 bug
.NOTINTERMEDIATE: $(OBJS:.o=.d)

-include $(OBJS:.o=.d)
