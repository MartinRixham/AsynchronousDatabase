# syntax=docker/dockerfile:1

FROM savsgio/alpine-rocksdb:latest AS builder
WORKDIR /
RUN apk update && \
    apk add bash build-base git jq openssl pkgconf cppcheck chromium \
    boost-dev gtest-dev curl-dev valgrind gcovr py3-pygments
RUN git clone https://github.com/martinrixham/cheesemake
COPY src/ src/
COPY test/ test/
COPY recipe.json ./
RUN cheesemake/cheesemake verify

FROM savsgio/alpine-rocksdb:latest
WORKDIR /
RUN apk update && \
    apk add libstdc++ curl nginx
COPY server/server.conf /etc/nginx/http.d/default.conf
COPY ui/dist /usr/share/nginx/html
COPY --from=builder build/bin/asyncdb .
CMD nginx & ./asyncdb
EXPOSE 80
