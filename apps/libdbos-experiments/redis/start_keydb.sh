#!/bin/bash

pushd ../../KeyDB
keydb_server_threads="${KEYDB_SERVER_THREADS:-1}"
rm -rf dump.rdb
echo "Starting KeyDB with $keydb_server_threads server threads"
#LD_PRELOAD=libhugetlbfs.so HUGETLB_MORECORE=yes 
src/keydb-server keydb.conf --server-threads $keydb_server_threads &

# Capture the PID of redis-server
REDIS_PID=$!

# Timeout counter (e.g., 30 seconds)
TIMEOUT=180
COUNT=0

echo "Waiting for KeyDB to start..."

# Loop until Redis is ready or timeout occurs
while ! src/keydb-cli ping &> /dev/null; do
    sleep 1
    COUNT=$((COUNT+1))
    if [ $COUNT -ge $TIMEOUT ]; then
        echo "Timeout waiting for KeyDB to start."
        exit 1
    fi
done

echo "KeyDB is now ready for connections."

popd