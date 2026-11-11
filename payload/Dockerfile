# syntax=docker/dockerfile:1.7
# Build environment for PS5 payloads: Debian + host LLVM + the prebuilt ps5-payload-dev bundle
# (SDK toolchain wrappers, plus static SDL2, libcurl, OpenSSL, freetype... for the console).
# The bundle's toolchain wrappers use whichever host LLVM is installed (15 to 23), so this runs
# natively on Apple Silicon (arm64) and on x86_64.
FROM debian:trixie-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends clang lld llvm make cmake pkg-config ca-certificates file bash socat \
    git glslc glslang-tools spirv-tools spirv-headers \
 && rm -rf /var/lib/apt/lists/*
# glslc/glslang/spirv-tools: ggml-vulkan compiles its compute shaders to SPIR-V at build time
# (finding 25, Track B — on-console whisper). git: fetch the pinned ggml/whisper.cpp source.
# ps5-payload-dev/pacbrew-repo v0.41.1 (2026-10-10). Bump the URL and checksum together.
ADD --checksum=sha256:e811bc2e4676c5bb7f80a8fe928d2985dce599996a5ca6b54da3e72699287887 \
    https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.41.1/ps5-payload-dev.tar.gz /tmp/ps5-payload-dev.tar.gz
RUN tar xzf /tmp/ps5-payload-dev.tar.gz -C / && rm /tmp/ps5-payload-dev.tar.gz
ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
WORKDIR /src
