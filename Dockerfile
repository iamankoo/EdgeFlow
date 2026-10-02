# syntax=docker/dockerfile:1

# ---- build stage -------------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY config ./config

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DEDGEFLOW_WARNINGS_AS_ERRORS=ON \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure \
    && cmake --install build --prefix /opt/edgeflow

# ---- runtime stage -----------------------------------------------------------
FROM ubuntu:24.04 AS runtime

RUN groupadd --system --gid 10001 edgeflow \
    && useradd --system --uid 10001 --gid edgeflow --no-create-home --shell /usr/sbin/nologin edgeflow

COPY --from=build /opt/edgeflow/bin/edgeflow /usr/local/bin/edgeflow
COPY --from=build /opt/edgeflow/etc/edgeflow /etc/edgeflow

USER edgeflow:edgeflow
ENV EDGEFLOW_CONFIG=/etc/edgeflow/config.yaml

# Phase 1 has no network endpoint, so there is nothing meaningful to probe yet.
HEALTHCHECK NONE

# Exec form: the process is PID 1 and receives SIGTERM directly from `docker stop`.
ENTRYPOINT ["/usr/local/bin/edgeflow"]
