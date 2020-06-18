#!/usr/bin/env bash

cp -R /home/vagrant/spdk_repo/spdk /tmp/spdk
umount /home/vagrant/spdk_repo/spdk && rm -rf /home/vagrant/spdk_repo/spdk
mv /tmp/spdk /home/vagrant/spdk_repo/spdk
chown -R vagrant:vagrant /home/vagrant/spdk_repo/spdk
