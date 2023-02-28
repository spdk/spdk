# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2021 Intel Corporation
#

FROM fedora:35 AS base

ARG spdk_repo=/tmp/spdk_repo
ARG spdk_tar=/spdk.tar.gz

# Generic args
ARG PROXY
ARG NO_PROXY

ENV http_proxy=$PROXY
ENV https_proxy=$PROXY
ENV no_proxy=$NO_PROXY

COPY --chown=root:root spdk.tar.gz /spdk.tar.gz
RUN if [[ ! -e $spdk_tar ]]; then printf 'Missing %s\n' "$spdk_tar" >&2; exit 1; fi
RUN mkdir -p "$spdk_repo" && \
	tar -C "$spdk_repo" -xf "$spdk_tar" && \
	chown -R root:root "$spdk_repo"
RUN dnf install -y rpm-build
RUN "$spdk_repo/scripts/pkgdep.sh" -d
RUN "$spdk_repo/test/common/config/vm_setup.sh" --test-conf=fio

# HACK: In case we received a .tar with built SPDK we need to overwrite the
# configuration to update all the paths make would need to lookup - this is
# needed since we execute inside a different mount namespace so we won't be
# able to find any absolute paths that were used prior creating the .tar.
RUN "$spdk_repo/configure"
RUN DEPS="no" "$spdk_repo/rpmbuild/rpm.sh" \
	--with-shared \
	--with-virtio \
	--with-fio
RUN mv "$HOME/rpmbuild/rpm/$(uname -m)/"*.rpm /tmp
RUN mv "/usr/src/fio/fio" /tmp

# Clean things up
RUN rm -f "$HOME/rpmbuild/rpm/$(uname -m)/"*.rpm
RUN rm -f "$spdk_tar"
RUN rm -rf "$spdk_repo"
RUN dnf clean all

# We are doing a multi-stage build here. This means that previous image,
# base, is going to end up as an intermediate one, untagged, <none> - this
# image can be then manually removed (--force-rm doesn't work here. Go
# figure).
FROM fedora:35 AS spdk

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
