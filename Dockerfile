# syntax=docker/dockerfile:1

FROM alpine:latest AS builder
WORKDIR /
RUN apk update && \
    apk add bash build-base git jq openssl cppcheck rocksdb-dev \
    boost-dev gtest-dev curl-dev valgrind gcovr py3-pygments
RUN git clone https://github.com/martinrixham/cheesemake
COPY src/ src/
COPY test/ test/
COPY recipe.json ./
RUN cheesemake/cheesemake verify

FROM alpine:latest AS ui
WORKDIR /ui
ENV CI=1
RUN apk update && apk add npm chromium 
COPY ui .
RUN rm -rf node_modules
RUN npm install
RUN npm test 
RUN npm run build

FROM alpine:latest
WORKDIR /
RUN apk update && \
    apk add libstdc++ rocksdb curl nginx
COPY server/server.conf /etc/nginx/http.d/default.conf
COPY --from=ui /ui/dist /usr/share/nginx/html
COPY --from=builder /build/bin/asyncdb .
CMD nginx & ./asyncdb
EXPOSE 80
