#!/bin/bash
pushd ../../KeyDB
keydb_pid=$(ps aux | grep '[k]eydb-server' | awk '{print $2}')
kill -9 $keydb_pid

is_port_in_use() {
    lsof -i TCP:$PORT  > /dev/null
    return $?
}

PORT=6379

echo "Checking for processes listening on port $PORT..."

# Loop until no processes are found listening on the port
while is_port_in_use; do
    echo "Port $PORT is in use. Waiting..."
    sleep 5  # wait for 5 seconds before checking again
done

sleep 10
rm dump.rdb
popd
