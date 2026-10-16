# KDS server image: a Release build of kds_server plus the SQL client.
# `docker compose run --rm cli` builds it, starts the server and opens a
# SQL prompt; see README.md's "Try it with Docker".

FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake libssl-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF \
 && cmake --build build --target kds_server -j"$(nproc)"

FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3 python3 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home-dir /var/lib/kds --create-home kds
COPY --from=build /src/build/kds_server /usr/local/bin/kds_server
COPY tools/ckdbs_cli.py tools/kwp.py /opt/kds/tools/
COPY docker/kds.conf /etc/kds/kds.conf
USER kds
WORKDIR /var/lib/kds
VOLUME /var/lib/kds
EXPOSE 15432
HEALTHCHECK --interval=2s --timeout=3s --start-period=5s --retries=15 \
    CMD ["python3", "/opt/kds/tools/ckdbs_cli.py", "PING"]
# kds_server stops cleanly on SIGTERM (server/stop_signal.hpp), so
# `docker stop` syncs and checkpoints rather than leaving a crash to recover.
CMD ["kds_server", "--config", "/etc/kds/kds.conf"]
