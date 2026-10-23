# syntax=docker/dockerfile:1
# Pins: IMPLEMENTATION_PLAN.md. MXL_REF is the single MXL pin (also in .github/workflows/ci.yaml).
# The image needs no GPU and starts on any x86-64 CPU: the AVX2 engine is chosen at run time.
# MEDIAMTX_VERSION precedes the first FROM: the built-in MediaMTX of the preview (own mode, §2.5).
ARG MEDIAMTX_VERSION=1.20.1

FROM bluenviron/mediamtx:${MEDIAMTX_VERSION} AS mediamtx

FROM ubuntu:24.04 AS build
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
ARG NMOS_CPP_REF=fe303849527394b03bdedc8f161f377fe458bb62

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config curl zip unzip tar ca-certificates python3 \
        bison flex autoconf automake libtool \
        libboost-system-dev libboost-thread-dev libboost-chrono-dev \
        libboost-date-time-dev libboost-regex-dev libboost-random-dev \
        libboost-filesystem-dev libcpprest-dev nlohmann-json3-dev \
        libssl-dev uuid-dev libavahi-compat-libdnssd-dev \
        libavcodec-dev libavformat-dev libavutil-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

RUN git init /tmp/mxl \
    && git -C /tmp/mxl remote add origin https://github.com/dmf-mxl/mxl.git \
    && git -C /tmp/mxl fetch --depth 1 origin "${MXL_REF}" \
    && git -C /tmp/mxl checkout FETCH_HEAD
COPY docker/mxl-vcpkg.json /tmp/mxl-manifest/vcpkg.json
RUN BASELINE="$(git -C /opt/vcpkg rev-parse HEAD)" \
    && sed -i "1a\\  \"builtin-baseline\": \"${BASELINE}\"," /tmp/mxl-manifest/vcpkg.json
RUN cmake -S /tmp/mxl -B /tmp/mxl/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_DIR=/tmp/mxl-manifest \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
        -DMXL_ENABLE_FABRICS_OFI=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /tmp/mxl/build -j"$(nproc)" \
    && cmake --install /tmp/mxl/build

RUN git init /opt/nmos-cpp \
    && git -C /opt/nmos-cpp remote add origin https://github.com/sony/nmos-cpp.git \
    && git -C /opt/nmos-cpp fetch --depth 1 origin "${NMOS_CPP_REF}" \
    && git -C /opt/nmos-cpp checkout FETCH_HEAD

COPY . /src
RUN cmake -S /src -B /src/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="/opt/mxl;/tmp/mxl/build/vcpkg_installed/x64-linux" \
        -DSCOPES_WITH_NMOS=ON -DNMOS_CPP_DIR=/opt/nmos-cpp/Development \
    && cmake --build /src/build -j"$(nproc)" \
    && /src/build/unit-tests

FROM ubuntu:24.04 AS runtime
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl \
        libcpprest2.10 libboost-chrono1.83.0t64 libboost-thread1.83.0 \
        libssl3t64 libavahi-compat-libdnssd1 \
        libavcodec60 libavformat60 libavutil58 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=mediamtx /mediamtx /usr/local/bin/mediamtx
COPY --from=mediamtx /LICENSE /usr/share/doc/mediamtx/LICENSE
COPY --from=build /opt/mxl/lib/ /usr/local/lib/
COPY --from=build /tmp/mxl/build/vcpkg_installed/x64-linux/lib/ /usr/local/lib/
COPY --from=build /src/build/mxl-scopes /usr/local/bin/mxl-scopes
COPY --from=build /src/build/mxl-scopes-bench /usr/local/bin/mxl-scopes-bench
COPY --from=build /src/build/mxl-scopes-writer /usr/local/bin/mxl-scopes-writer
COPY --from=build /src/build/mxl-scopes-sample /usr/local/bin/mxl-scopes-sample
COPY tests/integration/registry_stub.py /usr/local/share/mxl-scopes/registry_stub.py
COPY LICENSE NOTICE /usr/share/doc/mxl-scopes/
LABEL io.dmf.mxl.revision="${MXL_REF}"
RUN ldconfig \
    && if ! getent passwd 1000 >/dev/null; then \
         useradd --uid 1000 --no-create-home --shell /usr/sbin/nologin mxl; \
       fi \
    && mkdir -p /config && chown 1000:1000 /config
USER 1000:1000
# Web, REST, probes, metrics; NMOS Node and Connection APIs (+1 reserved); the own MediaMTX
# (WHEP, HLS, ICE UDP and TCP; RTSP only on 127.0.0.1).
EXPOSE 8170 3322 3323 8579 8578 8279 8279/udp
ENTRYPOINT ["/usr/local/bin/mxl-scopes"]
