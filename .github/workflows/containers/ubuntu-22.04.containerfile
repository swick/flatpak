# This Containerfile builds an image that we use in some github workflows.
# When this file is changed, or one needs to rebuild the image for another
# reason, the tag in the build-container job in container.yml must be bumped.

FROM ubuntu:22.04

RUN apt update
RUN apt upgrade -y

RUN apt install -y software-properties-common
RUN add-apt-repository ppa:flatpak/stable

RUN apt update
RUN apt upgrade -y

# Install dependencies
RUN apt install -y --no-install-recommends \
    ca-certificates \
    libglib2.0-dev \
    attr \
    gettext \
    bison \
    dbus \
    gtk-doc-tools \
    libfuse3-dev \
    fuse3 \
    ostree \
    libostree-dev \
    libarchive-dev \
    libzstd-dev \
    libcap-dev \
    libattr1-dev \
    libdw-dev \
    libelf-dev \
    python3-pyparsing \
    libjson-glib-dev \
    shared-mime-info \
    desktop-file-utils \
    libpolkit-agent-1-dev \
    libpolkit-gobject-1-dev \
    libseccomp-dev \
    libsoup2.4-dev \
    libcurl4-openssl-dev \
    libsystemd-dev \
    libxml2-utils \
    libgpgme11-dev \
    gobject-introspection \
    libgirepository1.0-dev \
    libappstream-dev \
    libdconf-dev \
    clang \
    socat \
    meson \
    libdbus-1-dev \
    e2fslibs-dev \
    bubblewrap \
    xdg-dbus-proxy \
    meson \
    ninja-build \
    libyaml-dev \
    libstemmer-dev \
    gperf \
    itstool \
    libmalcontent-0-dev \
    libxau-dev \
    libgdk-pixbuf2.0-dev \
    openssl \
    git \
    policykit-1 \
    gnome-desktop-testing \
    libcap2-bin

RUN update-ca-certificates