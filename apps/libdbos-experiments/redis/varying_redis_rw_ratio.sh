#!/bin/bash

redis_compile_script="${REDIS_COMPILE_SCRIPT:-compile_linux.sh}"
wd="${EXP_DIRECTORY:-/localpv/zxj/dune/apps/libdbos-experiments/redis}"
ulimit -c 0

pushd $wd

bash kill_redis.sh

echo "now at $wd, $redis_compile_script"

result_prefix="$wd/${EXP_PREFIX_NAME:-redis_linux}_rw_ratio"

bash "$redis_compile_script"


#database_sizes=(8388608 16777216 33554432 67108864 134217728 268435456 536870912)
database_sizes=(838860 1677721 3355443 6710886 13421772 26843545)
#database_sizes=(26843545)
#database_sizes=(6710886 26843545)
#database_sizes=(13421772 26843545)
#database_sizes=(1677721 26843545)

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

# Function to get the last save time from Redis
get_last_save_time() {
    src/redis-cli LASTSAVE | awk '{print $1}'
}

read_ratios=(0 1 2 4 8)
for read_ratio in "${read_ratios[@]}"; do
    size=26843545 # 25GB
    dbsize=$(expr $size \* 1024)
    dbsize_gb=$(expr $dbsize / 1073741824)
    echo "dbsize $dbsize_gb gb"

    result_file_name="${result_prefix}_rw_ratio_1_${read_ratio}.json"

    huge_pages_free=$(grep -i 'HugePages_Free' /proc/meminfo | awk '{print $2}')
    echo "free huge pages $huge_pages_free"

    # Start the redis server
    bash start_redis.sh

    pushd ../../redis
    # warmup
    echo "warming up"
    memtier_benchmark --ratio=1:0 -n `expr $size \* 2 / 1` -t 1 --pipeline=1000 -c 1 --key-minimum=1 --key-maximum=$size --data-size=1000  --print-percentiles 50,95,99,99.9,100
    
    #src/redis-benchmark -t set -n `expr $size \* 2` -c 3 -r $size -d 1000 -P 2000 --csv
    
    # sleep 1

    echo "warmup finished"
    

    echo "benchmark started"
    memtier_benchmark --ratio=1:$read_ratio -n `expr 3000000 / 1` -t 1 --pipeline=1000 -c 1 --key-minimum=1 --key-maximum=$size --data-size=1000  --print-percentiles 50,95,99,99.9,100 --json-out-file=$result_file_name & # > /dev/null 2>&1      
    benchmark_pid=$!
    sleep 2
    # Capture the PID of the last background job
    #redis_benchmark_pid=$(ps aux | grep '[r]edis-benchmark' | awk '{print $2}')
    
    time src/redis-cli BGSAVE 
    #
    
    #elapsed=$(time_command "src/redis-cli BGSAVE")


    # # Loop to periodically execute another command while the main command is running
    # while :; do
    #     # Sleep for a specified duration before the next iteration
    #     sleep 1
    #     # Check if the process is still running
    #     if ! ps -p $redis_benchmark_pid > /dev/null; then
    #         # Process has finished, break the loop
    #         break
    #     fi

    #     current_last_save_timestamp=$(get_last_save_time)

    #     if [[ "$current_last_save_timestamp" != "$last_bgsave_timestamp" ]]; then
    #         time_diff=$((current_last_save_timestamp - last_bgsave_timestamp))
    #         echo "Time elapsed for BGSAVE: $time_diff seconds."
    #         break
    #     else
    #         echo "Waiting for BGSAVE to complete..."
    #     fi

    # done

    # # tell the benchmark tool to quit and report results
    # kill -USR2 $redis_benchmark_pid

    # Wait for the main command to complete, just in case
    wait $benchmark_pid
    popd

    sleep 30
    bash kill_redis.sh
done


popd
