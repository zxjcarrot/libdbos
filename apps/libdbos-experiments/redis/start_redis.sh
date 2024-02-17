#!/bin/bash

pushd ../../redis
rm -rf dump.rdb
#LD_PRELOAD=libhugetlbfs.so HUGETLB_MORECORE=yes 
src/redis-server redis.conf &

# Capture the PID of redis-server
REDIS_PID=$!

# Timeout counter (e.g., 30 seconds)
TIMEOUT=180
COUNT=0

echo "Waiting for Redis to start..."

# Loop until Redis is ready or timeout occurs
while ! src/redis-cli ping &> /dev/null; do
    sleep 1
    COUNT=$((COUNT+1))
    if [ $COUNT -ge $TIMEOUT ]; then
        echo "Timeout waiting for Redis to start."
        exit 1
    fi
done

echo "Redis is now ready for connections."

popd