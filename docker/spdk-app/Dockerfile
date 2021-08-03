# SPDX-License-Identifier: Apache-2.0
# Copyright (c) Intel Corporation

FROM spdk

# Generic args
ARG PROXY
ARG NO_PROXY

ENV http_proxy=$PROXY
ENV https_proxy=$PROXY
ENV no_proxy=$NO_PROXY


COPY init /init

ENTRYPOINT ["/init"]
