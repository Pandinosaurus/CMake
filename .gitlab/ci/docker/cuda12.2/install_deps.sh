#!/bin/sh

set -e

apt-get update

# Install dependency without interaction.
env DEBIAN_FRONTEND=noninteractive \
    TZ=America/New_York \
  apt-get install -y \
    tzdata

# Install development tools.
apt-get install -y \
    g++ \
    clang-18 \
    libomp-18-dev \
    curl \
    git

apt-get clean
