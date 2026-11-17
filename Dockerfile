# pondd and pondctl as a container image. The database is MariaDB in a container of its own: compose.yaml runs both.
#
#   docker compose up -d                                     # with POND_ADDRESS and POND_NAME set (docs/docker.md)
#   docker compose exec pond pondctl invite create           # the pond://… link to give out
#
# What the Pond keeps besides the database (pond.yaml, its key, its certificate, uploads) is in the /data volume.

FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates \
      libssl-dev libmariadb-dev libhiredis-dev libjsoncpp-dev libyaml-cpp-dev uuid-dev zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY patches patches
COPY src src
COPY tests tests
# Drogon and libdatachannel are fetched and built here, at the releases CMakeLists.txt names, and linked in
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --parallel 4 --target pondd pondctl \
 && strip build/pondd build/pondctl

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3 libmariadb3 libhiredis0.14 libjsoncpp25 libyaml-cpp0.7 libuuid1 zlib1g \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home-dir /data --shell /usr/sbin/nologin pond \
 && mkdir /data && chown pond:pond /data && chmod 700 /data
COPY --from=build /src/build/pondd /src/build/pondctl /usr/local/bin/
COPY dist/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
COPY pond.yaml.example /usr/local/share/pond/pond.yaml.example

USER pond
WORKDIR /data
ENV HOME=/data
VOLUME /data
# HTTPS, and voice on UDP at the same port number
EXPOSE 8443 8443/udp
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
