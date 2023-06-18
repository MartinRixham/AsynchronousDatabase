#! /usr/bin/env bash

threads=6

stop()
{
  pkill -P $$
}

trap stop INT

get_tables()
{
	echo "Starting thread $1."
	for (( i=0; i<=5000; i++ )); do
		echo "Request $i"
		curl localhost:8080/asyncdb/tables
	done
}

run() {
	for (( i=0; i<$threads; i++ )); do
		get_tables $i&
	done

	wait
}

time run
