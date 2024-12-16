#!/bin/bash

set -euo pipefail

NUM_CHUNKS=2
SENDER_WINDOW=1
SENDER_DROP_PATTERN=""
RECEIVER_WINDOW=1
RECEIVER_DROP_PATTERN=""

rm -f send.dat receive.dat sender-packets.log receiver-packets.log
dd if=/dev/urandom of=send.dat bs=1000 count=$((NUM_CHUNKS-1))

LD_PRELOAD="./log-packets.so" \
    PACKET_LOG="receiver-packets.log" \
    DROP_PATTERN="$RECEIVER_DROP_PATTERN" \
    ./file-receiver receive.dat 1234 $RECEIVER_WINDOW &
RECEIVER_PID=$!
sleep .1

LD_PRELOAD="./log-packets.so" \
    PACKET_LOG="sender-packets.log" \
    DROP_PATTERN="$SENDER_DROP_PATTERN" \
    ./file-sender send.dat localhost 1234 $SENDER_WINDOW || true

wait $RECEIVER_PID || true

diff -qs send.dat receive.dat || true
rm send.dat receive.dat

./generate-msc.sh msc.eps sender-packets.log receiver-packets.log
rm sender-packets.log receiver-packets.log
