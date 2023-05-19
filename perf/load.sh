#! /usr/bin/env bash

threads=4

stop()
{
  pkill -P $$
}

trap stop INT

get_tables()
{
	echo "Starting thread $1."
	# Runs forever.
	for (( i=0; i>=0; i++ )); do
		echo "Request $i"
		http get localhost:8080/asyncdb/tables
	done
}

for (( i=0; i<$threads; i++ )); do
	get_tables $i&
done

wait
