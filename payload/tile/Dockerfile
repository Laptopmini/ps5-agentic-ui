# syntax=docker/dockerfile:1.7
# Build environment for the home-screen tile (payload/tile): Clang 18, which drakmor's
# ps5-native-app-boilerplate requires, plus what its build scripts call. The boilerplate fetches
# its own pinned PS5 payload SDK (v0.42) and zlib into payload/tile/.cache.
FROM debian:trixie-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends clang-18 lld-18 llvm-18 python3 make git wget unzip tar ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
