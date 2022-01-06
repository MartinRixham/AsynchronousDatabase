# syntax=docker/dockerfile:1

FROM savsgio/alpine-rocksdb:latest AS builder
WORKDIR /
RUN apk update && \
    apk add bash build-base git jq openssl pkgconf cppcheck \
    boost-dev gtest-dev curl-dev valgrind gcovr py3-pygments
RUN git clone https://github.com/martinrixham/cheesemake
COPY src/ src/
COPY test/ test/
COPY recipe.json ./
RUN cheesemake/cheesemake verify

FROM savsgio/alpine-rocksdb:latest
WORKDIR /
RUN apk update && \
    apk add libstdc++
COPY --from=builder build/bin/asyncdb .
CMD ./asyncdb
EXPOSE 8080
