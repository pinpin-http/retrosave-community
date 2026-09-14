# MinIO publie désormais son serveur sous forme de sources. Ce commit inclut
# le correctif GHSA-jjjj-jwhf-8rgr, absent des anciennes images Docker Hub/Quay.
FROM golang:1.26 AS build
WORKDIR /src
ADD https://github.com/minio/minio/archive/9e49d5e7a648f00e26f2246f4dc28e6b07f8c84a.tar.gz /tmp/minio.tar.gz
RUN echo '45521908307306e925c98d629e1c17d78c8b72b6ee242b1bfb1409f7d8ee5841  /tmp/minio.tar.gz' \
      | sha256sum --check --strict - && \
    tar -xzf /tmp/minio.tar.gz --strip-components=1 && \
    CGO_ENABLED=0 go build -trimpath -o /minio .

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /minio /usr/local/bin/minio
COPY --from=build /src/LICENSE /usr/share/doc/minio/LICENSE
EXPOSE 9000
ENTRYPOINT ["minio"]
CMD ["server", "/data"]
