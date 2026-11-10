FROM fedora:latest

# Install dependencies:
RUN dnf install -y \
    appstream \
    catch-devel \
    createrepo_c \
    desktop-file-utils \
    gtk4-devel \
    libdnf5-devel \
    dnf5daemon-server \
    polkit \
    gcc-c++ \
    glibc-langpack-sv \
    gdb \
    gdb-gdbserver \
    make \
    meson \
    ninja-build \
    pkgconf \
    cmake \
    git \
    rpm-build \
    mesa-libEGL \
    mesa-libGL \
    mesa-libGLES \
    dbus-daemon \
    dbus-glib \
    procps-ng \
    psmisc \
    valgrind \
    heaptrack \
    perf \
    && dnf clean all
