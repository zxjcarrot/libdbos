#!/bin/bash

keydb_compile_script="${KEYDB_COMPILE_SCRIPT:-compile_linux.sh}"
wd="${EXP_DIRECTORY:-/localpv/zxj/dune/apps/libdbos-experiments/redis}"
ulimit -c 0

pushd $wd

bash kill_keydb.sh

echo "now at $wd, $keydb_compile_script"

result_prefix="$wd/${EXP_PREFIX_NAME:-redis_linux}_server_threads_"

bash "$keydb_compile_script"


#database_sizes=(8388608 16777216 33554432 67108864 134217728 268435456 536870912)
database_sizes=(838860 1677721 3355443 6710886 13421772 26843545)
#database_sizes=(26843545)
#database_sizes=(6710886 26843545)
#database_sizes=(13421772 26843545)
#database_sizes=(1677721 26843545)

server_threads=(1 2 4 8 16)
time_command() {
    local start_time=$(date +%s%3N)  # Start time in microseconds
    # Execute the command passed to the function
    "$@" > /dev/null 2>&1

    local end_time=$(date +%s%3N)    # End time in microseconds

    # Calculate elapsed time
    local elapsed_time=$((end_time - start_time))

    # Return the elapsed time
    echo $elapsed_time
}

# Function to get the last save time from KeyDB
get_last_save_time() {
    src/keydb-cli LASTSAVE | awk '{print $1}'
}

# use a database of size 25Gb
for st in "${server_threads[@]}"; do
    echo $st
    size=26843545
    dbsize=$(expr $size \* 1024)
    dbsize_gb=$(expr $dbsize / 1073741824)
    echo "dbsize $dbsize_gb gb"

    result_file_name="${result_prefix}_${st}.json"

    huge_pages_free=$(grep -i 'HugePages_Free' /proc/meminfo | awk '{print $2}')
    echo "free huge pages $huge_pages_free"

    # Start the KeyDB server
    KEYDB_SERVER_THREADS=$st bash start_keydb.sh

    pushd ../../KeyDB
    # warmup
    echo "warming up"
    memtier_benchmark --ratio=1:0 -n `expr $size \* 2 / 1` -t 1 --pipeline=1000 -c 1 --key-minimum=1 --key-maximum=$size --data-size=1000  --print-percentiles 50,95,99,99.9,100 --ratio=1:0
    
    # sleep 1

    echo "warmup finished"
    

    echo "benchmark started"
    memtier_benchmark --ratio=1:0 -n 500000 -t 4 --key-pattern=P:P --pipeline=1000 -c 20 --key-minimum=1 --key-maximum=$size --data-size=1000  --print-percentiles 50,95,99,99.9,100 --ratio=1:0 --json-out-file=$result_file_name & # > /dev/null 2>&1      
    benchmark_pid=$!
    sleep 2
    # Capture the PID of the last background job
    #redis_benchmark_pid=$(ps aux | grep '[r]edis-benchmark' | awk '{print $2}')
    
    time src/keydb-cli BGSAVE 

    # Wait for the main command to complete, just in case
    wait $benchmark_pid
    popd

    sleep 30
    bash kill_keydb.sh
done


popd
