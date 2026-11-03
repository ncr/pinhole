# The base and package snapshot move only through an intentional source edit.
FROM archlinux:base-devel@sha256:767de6eaa40dcfeda5263a48737ea03a782caabdf3a403b6e6a203c7fd333681
RUN printf 'Server = https://archive.archlinux.org/repos/2026/11/02/$repo/os/$arch\n' > /etc/pacman.d/mirrorlist \
 && pacman -Syuu --noconfirm \
 && pacman -S --noconfirm --needed \
    git curl ca-certificates rust python python-pillow python-pytest python-yaml \
    qt6-base qt6-declarative qt6-svg qt6-wayland qt6-multimedia qt6-multimedia-ffmpeg qt6-shadertools \
    libjpeg-turbo libheif libultrahdr lcms2 wayland-protocols sqlite \
    imagemagick ffmpeg fontconfig ttf-jetbrains-mono-nerd \
    xorg-server-xvfb xorg-xauth dbus weston mesa vulkan-swrast \
 && sed -i 's/^#\(en_US.UTF-8\|pl_PL.UTF-8\)/\1/' /etc/locale.gen \
 && locale-gen \
 && pacman -Q > /opt/pinhole-ci-packages.txt \
 && useradd --create-home --uid 1000 --shell /bin/bash build
USER build
WORKDIR /home/build
ENV LANG=en_US.UTF-8 PINHOLE_BUILD_JOBS=2
