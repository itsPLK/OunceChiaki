# Build environment for OunceChiaki: devkitA64 plus the pinned public ounce-devkit
# (libounce, OunceVK). The same SDK version as CI (scripts/install-sdk.sh).
#   docker build -t ounce-chiaki-build -f docker/Dockerfile .
# scripts/build.sh builds this image on first use.
FROM devkitpro/devkita64:20260219
ENV DEVKITPRO=/opt/devkitpro
COPY scripts/install-sdk.sh /tmp/install-sdk.sh
RUN bash /tmp/install-sdk.sh && rm /tmp/install-sdk.sh
ENV PATH="/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:${PATH}"
