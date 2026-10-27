# syntax=docker/dockerfile:1

# ---- build stage -------------------------------------------------------------
FROM ubuntu:24.04 AS build

# libboost-dev provides the header-only Boost.Asio and Boost.Beast; libpq-dev is the
# PostgreSQL client library used by the service registry.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates libboost-dev libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY config ./config
# bench/ is copied for the tests that check the load-test configuration; the benchmark targets are off.
COPY bench ./bench
COPY db ./db

# Bounded: Beast-heavy translation units need ~1 GB each, so unbounded parallelism can
# exhaust a small Docker VM. Override with --build-arg BUILD_JOBS=N on larger machines.
ARG BUILD_JOBS=2

# The build directory is a BuildKit cache mount: it survives between `docker build` runs, so after a small change only
# the touched translation units are recompiled (and the dependencies are not downloaded again) instead of the whole
# tree. Everything the runtime stage needs is installed to /opt/edgeflow, which is part of the image layer, not the
# cache. The tests run on every build; `--strip` removes the symbols from the installed binary.
RUN --mount=type=cache,target=/src/build,id=edgeflow-build \
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DEDGEFLOW_WARNINGS_AS_ERRORS=ON \
    && cmake --build build --parallel ${BUILD_JOBS} \
    && ctest --test-dir build --output-on-failure \
    && cmake --install build --prefix /opt/edgeflow --strip

# ---- runtime stage -----------------------------------------------------------
FROM ubuntu:24.04 AS runtime

# libpq5 is the PostgreSQL client runtime library (the server runs in its own container).
RUN apt-get update \
    && apt-get install -y --no-install-recommends libpq5 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 10001 edgeflow \
    && useradd --system --uid 10001 --gid edgeflow --no-create-home --shell /usr/sbin/nologin edgeflow

COPY --from=build /opt/edgeflow/bin/edgeflow /usr/local/bin/edgeflow
COPY --from=build /opt/edgeflow/etc/edgeflow /etc/edgeflow

USER edgeflow:edgeflow
ENV EDGEFLOW_CONFIG=/etc/edgeflow/config.yaml

# Must match server.port in the configuration (the shipped default is 8080).
EXPOSE 8080

# Runs `edgeflow --healthcheck`, which sends GET /health to the configured port of the
# running server and succeeds only on HTTP 200. No curl is needed in the image.
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/edgeflow", "--healthcheck"]

# Exec form: the process is PID 1 and receives SIGTERM directly from `docker stop`.
ENTRYPOINT ["/usr/local/bin/edgeflow"]
