# How to install from a release

The release page provides the latest stable version pre-compiled for different operating systems, in a compressed folder.

Installing that is a rather simple affair:

1. be sure to have the latest MADS version installed from <https://git.new/mads>
2. in a terminal run `mads -p`: this will tell you where MADS is installed
3. copy/move the `mads-chat` executable in the `bin` subfolder of the folder from the previous step.

On Ubuntu 24.04, you also need the following runtime libraries:

```sh
sudo apt update
sudo apt install -y \
  libc6 \
  libstdc++6 \
  libgcc-s1 \
  libssl3t64 \
  libglvnd0 \
  libglx0 \
  libopengl0 \
  libx11-6 \
  libxcb1 \
  libxau6 \
  libxdmcp6 \
  libbsd0 \
  libmd0 \
  libwayland-client0 \
  libwayland-cursor0 \
  libwayland-egl1 \
  libxkbcommon0 \
  libxrandr2 \
  libxinerama1 \
  libxcursor1 \
  libxi6
```
