# This Containerfile builds an image that we use in some github workflows.
# When this file is changed, or one needs to rebuild the image for another
# reason, the tag in the build-container job in container.yml must be bumped.

FROM registry.fedoraproject.org/fedora:latest

# FIXME
RUN update-ca-trust
RUN dnf install -y flatpak gnupg2-gpg-agent

RUN dnf upgrade -y

# Install tools
RUN dnf install -y \
    git \
    clang \
    gnome-desktop-testing \
    libasan \
    glibc-langpack-en \
    dbus-daemon \
    ostree \
    openssl

# Install dependencies
RUN dnf builddep -y flatpak