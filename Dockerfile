FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git ca-certificates \
        libmosquitto-dev \
        nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY CMakeLists.txt .
COPY src/ src/
COPY config/ config/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# ── runtime image ─────────────────────────────────────────────────────────────
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libmosquitto1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/build/svp_manager /usr/local/bin/svp_manager
COPY config/svp_config.json /etc/svp_manager/svp_config.json

ENTRYPOINT ["svp_manager", "/etc/svp_manager/svp_config.json"]
