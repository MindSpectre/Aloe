# Published as ghcr.io/<owner>/aloe_toolchain:stable by .github/workflows/toolchain-deployment.yaml
# when a push to main changes this directory.
FROM fedora:44

ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_DISABLE_METRICS=1 \
    VCPKG_DEFAULT_TRIPLET=x64-linux

# Disable Cisco openh264 repo (region-restricted CDN,
# enabled by default and is not required here)
RUN dnf config-manager setopt fedora-cisco-openh264.enabled=0

# 1. Core build tooling. gcc-c++ brings libstdc++, which clang uses too.
#    python3 is needed by the DPDK port, which builds with meson.
RUN dnf -y group install development-tools
RUN dnf -y install  \
    clang  \
    clang-tools-extra \
    gcc-c++ \
    perl-IPC-Cmd  \
    kernel-headers \
    curl  \
    openssl-devel \
    bison  \
    flex  \
    make  \
    cmake \
    autoconf \
    autoconf-archive \
    automake  \
    libtool \
    ninja-build \
    ccache \
    mold \
    python3 \
    pkgconf-pkg-config \
    git \
    zip \
    unzip \
    tar

RUN dnf install -y perl-CPAN
RUN cpan "IPC::Cmd"

# 2. vcpkg bootstrap (executable only, no packages installed)
RUN git clone https://github.com/microsoft/vcpkg ${VCPKG_ROOT} && \
    cd ${VCPKG_ROOT} && \
    ./bootstrap-vcpkg.sh

# Set up workspace
WORKDIR /workspace

ENV PATH="$VCPKG_ROOT:$PATH"
