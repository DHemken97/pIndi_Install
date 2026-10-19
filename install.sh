#!/bin/bash

sudo apt update
sudo apt full-upgrade -y
sudo apt install -y \
    fbi \
    python3 \
    python3-pil \
    fonts-dejavu-core \
    curl \
    git \
    procps \
    iproute2 \
    ca-certificates \
    pkg-config
sudo apt install -y \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad

sudo install -d -o root -g root -m 755 /usr/share/pindi

sudo install -m 644 \
    /home/pindi/Splash.jpg \
    /usr/share/pindi/splash.jpg


mv ./pindi-splash-render /usr/local/bin/pindi-splash-render
sudo chmod 755 /usr/local/bin/pindi-splash-render
sudo /usr/local/bin/pindi-splash-render

mv ./pindi-splash.service /etc/systemd/system/pindi-splash.service

sudo systemctl disable --now getty@tty1.service

sudo systemctl daemon-reload
sudo systemctl enable pindi-splash.service
sudo systemctl start pindi-splash.service


