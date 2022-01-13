# Asynchronous Database [![Build Status](https://app.travis-ci.com/MartinRixham/AsynchronousDatabase.svg?branch=master)](https://app.travis-ci.com/MartinRixham/AsynchronousDatabase)
A database for asynchronous data processing

### Build

The database is built with [Cheesemake](https://github.com/martinrixham/cheesemake).

Once Cheesemake is installed the database can be built with the command `cmk verify`.

### Run

The easiest way to run the database is with `docker` and `docker-compose`.

Build the docker image `docker build -t asyncdb .` then run it with `docker-compose up`.
The server should then be running on `localhost:8080`.
