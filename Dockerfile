FROM debian:bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make -C aircast/tests
RUN make -C aircast CC=gcc HOST=linux PLATFORM=x86_64 -j2

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/bin/aircast-linux-x86_64 /usr/local/bin/aircast
ENTRYPOINT ["/usr/local/bin/aircast"]
CMD ["-Z"]
