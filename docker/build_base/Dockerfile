# SPDX-License-Identifier: Apache-2.0
# Copyright (c) Intel Corporation

FROM fedora:33 AS base

# Generic args
ARG PROXY
ARG NO_PROXY

ENV http_proxy=$PROXY
ENV https_proxy=$PROXY
ENV no_proxy=$NO_PROXY


COPY spdk.tar.gz /spdk.tar.gz
COPY pre-install /install
RUN /install

# We are doing a multi-stage build here. This means that previous image,
# base, is going to end up as an intermediate one, untagged, <none> - this
# image can be then manually removed (--force-rm doesn't work here. Go
# figure).
FROM fedora:33 AS spdk

LABEL maintainer=spdk.io

# Proxy configuration must be set for each build separately...
ARG PROXY
ARG NO_PROXY

ENV http_proxy=$PROXY
ENV https_proxy=$PROXY
ENV no_proxy=$NO_PROXY

# Copy SPDK's RPMs built during pre-install step.
COPY --from=base /tmp/*.rpm /tmp/
COPY --from=base /tmp/fio /tmp/
# Wrap up the image
COPY post-install /install
RUN /install
