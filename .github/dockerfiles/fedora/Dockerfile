FROM fedora:40
LABEL maintainer "os@safl.dk"
RUN dnf install -y \
  bridge-utils \
  cloud-utils \
  git \
  htop \
  lshw \
  openssh-server \
  pipx \
  procps \
  python3-boto3 \
  qemu-img \
  qemu-kvm.x86_64 \
  qemu-system-x86.x86_64 \
  qemu-tools \
  time \
  vim
RUN dnf clean all && \
  rm -rf /var/cache/dnf /var/log/dnf* /var/log/yum.log && \
  pipx ensurepath && \
  qemu-system-x86_64 --version

RUN mkdir -p /root/.ssh
RUN chmod 0700 /root/.ssh
RUN ssh-keygen -b 2048 -t rsa -f /root/.ssh/id_rsa -q -N ""
RUN cp /root/.ssh/id_rsa.pub /root/.ssh/authorized_keys
RUN echo "Host *" >> /root/.ssh/config
RUN echo "  StrictHostKeyChecking no" >> /root/.ssh/config
RUN echo "  NoHostAuthenticationForLocalhost yes" >> /root/.ssh/config
RUN echo "  UserKnownHostsFile=/dev/null" >> /root/.ssh/config
RUN chmod 0400 /root/.ssh/config

RUN ssh-keygen -A
RUN /usr/sbin/sshd

CMD ["bash"]