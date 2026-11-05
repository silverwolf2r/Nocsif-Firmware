# Builds the NocSif Desktop Bridge AppImage on Ubuntu 22.04 (glibc 2.35), so the result runs on
# Ubuntu 22.04+, Debian 12+, Fedora 36+ and the like. x86_64 only. Driven by ../build_appimage.sh.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

# Python 3.12 (deadsnakes) with Tk, plus the few tools the build needs.
RUN apt-get update && apt-get install -y --no-install-recommends \
        software-properties-common ca-certificates curl gnupg binutils file && \
    add-apt-repository -y ppa:deadsnakes/ppa && \
    apt-get update && apt-get install -y --no-install-recommends \
        python3.12 python3.12-venv python3.12-tk libpython3.12 && \
    rm -rf /var/lib/apt/lists/*

# Python deps, pinned to the versions proven on the maintainer's machine.
RUN python3.12 -m venv /opt/venv && \
    /opt/venv/bin/pip install --no-cache-dir \
        pyinstaller==6.22.3 esptool==5.6.0 pyserial==3.5 requests==2.34.2 Pillow==13.0.0
ENV PATH=/opt/venv/bin:$PATH

# AppImage tooling, pinned. Fetched here (build time) so `docker run` needs no network.
ARG APPIMAGETOOL=1.9.1
ARG RUNTIME=20251108
RUN curl -fsSL -o /usr/local/bin/appimagetool \
        https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL}/appimagetool-x86_64.AppImage && \
    chmod +x /usr/local/bin/appimagetool && \
    curl -fsSL -o /opt/runtime-x86_64 \
        https://github.com/AppImage/type2-runtime/releases/download/${RUNTIME}/runtime-x86_64
ENV APPIMAGE_EXTRACT_AND_RUN=1
