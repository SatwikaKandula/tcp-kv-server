#!/usr/bin/env bash
# Functional + concurrency tests for tcp-kv-server.
# Usage: ./tests/run_tests.sh [server_binary]   (default: ./kvserver)
set -u

SERVER=${1:-./kvserver}
PORT=${PORT:-7171}
PASS=0
FAIL=0

"$SERVER" -p "$PORT" -t 8 > /tmp/kvserver_test.log 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null' EXIT

# Wait for the server to start listening.
for _ in $(seq 1 50); do
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break
    sleep 0.1
done

# Send one command on a fresh connection and print the one-line reply.
send() {
    exec 3<>/dev/tcp/127.0.0.1/$PORT
    printf '%s\n' "$1" >&3
    IFS= read -r -t 2 line <&3
    exec 3>&-
    printf '%s' "$line"
}

check() {
    local name=$1 cmd=$2 want=$3 got
    got=$(send "$cmd")
    if [[ "$got" == "$want" ]]; then
        echo "PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL  $name: sent '$cmd', want '$want', got '$got'"
        FAIL=$((FAIL + 1))
    fi
}

echo "== functional tests =="
check "ping"                "PING"                   "PONG"
check "get missing key"     "GET nope"               "NOT_FOUND"
check "set"                 "SET color blue"         "OK"
check "get after set"       "GET color"              "VALUE blue"
check "overwrite"           "SET color green"        "OK"
check "get overwritten"     "GET color"              "VALUE green"
check "value with spaces"   "SET msg hello big world" "OK"
check "get spaced value"    "GET msg"                "VALUE hello big world"
check "delete"              "DEL color"              "DELETED"
check "get after delete"    "GET color"              "NOT_FOUND"
check "delete missing"      "DEL color"              "NOT_FOUND"
check "unknown command"     "FLY away"               "ERR unknown command"
check "set missing value"   "SET lonely"             "ERR usage: SET <key> <value>"

echo "== pipelined commands on one connection =="
exec 3<>/dev/tcp/127.0.0.1/$PORT
printf 'SET a 1\nSET b 2\nGET a\nGET b\nQUIT\n' >&3
replies=$(timeout 2 cat <&3 | tr '\n' ' ')
exec 3>&-
if [[ "$replies" == "OK OK VALUE 1 VALUE 2 BYE " ]]; then
    echo "PASS  pipelined"
    PASS=$((PASS + 1))
else
    echo "FAIL  pipelined: got '$replies'"
    FAIL=$((FAIL + 1))
fi

echo "== concurrency test (32 clients x 2000 requests) =="
if ./bench -p "$PORT" -c 32 -n 2000 -r 80; then
    echo "PASS  concurrent clients, every reply verified"
    PASS=$((PASS + 1))
else
    echo "FAIL  concurrent clients reported errors"
    FAIL=$((FAIL + 1))
fi

echo "== graceful shutdown on SIGINT =="
kill -INT $SERVER_PID
for _ in $(seq 1 50); do
    kill -0 $SERVER_PID 2>/dev/null || break
    sleep 0.1
done
if ! kill -0 $SERVER_PID 2>/dev/null && grep -q "clean exit" /tmp/kvserver_test.log; then
    echo "PASS  clean shutdown"
    PASS=$((PASS + 1))
else
    echo "FAIL  server did not shut down cleanly"
    FAIL=$((FAIL + 1))
fi
trap - EXIT

echo
echo "passed: $PASS  failed: $FAIL"
[[ $FAIL -eq 0 ]]
