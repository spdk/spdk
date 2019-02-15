SPDK_REPO=/home/vagrant/spdk_repo/spdk
SDL_DIR=$SPDK_REPO/../sdl_scans

# Can be found on ProtexIP server Tools section
# ex. https://gkipscn01.intel.com/protex/?uifsid=4#0=dW,go,fh,gc,gL,fI
PROTEX_CLIENT=$SPDK_REPO/blackduck-client-linux-amd64.zip

# Prepare enva
sudo mkdir $SDL_DIR
sudo chown -R vagrant $SDL_DIR

# Install ProtexIP client
sudo apt-get install unzip
unzip $PROTEX_CLIENT -d $SDL_DIR
cd $SDL_DIR/blackduck-client-linux-amd64
sudo sh blackduck-client-linux-amd64.bin -i silent

# This is helper script (origin?)
sudo apt-get install liblwp-mediatypes-perl
cp $SPDK_REPO/bdscan.pl $SDL_DIR/
cp $SPDK_REPO/protex.cfg $SDL_DIR/
