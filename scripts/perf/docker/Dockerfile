FROM debian:buster-20200514-slim

WORKDIR /root

RUN apt-get update \
&& apt-get install -y \
        apt-utils \
        sudo \
        git \
        nvme-cli \
        build-essential \
        libaio-dev \
        kmod \
        procps \
	jq \
&& git clone https://github.com/axboe/fio \
&& cd fio \
&& ./configure \
&& make \
&& make install \
&& cd /root \
&& git clone \
        --recursive \
        --depth 1 \
        --shallow-submodules \
        --branch v20.04.1 \
        https://github.com/spdk/spdk \
&& /root/spdk/scripts/pkgdep.sh \
&& cd /root/spdk \
&& ./configure --with-fio=/root/fio \
&& make \
&& apt-get clean \
&& rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*