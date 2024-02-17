#!/bin/bash
set -x
tabby_compile_script="${TABBY_COMPILE_SCRIPT}"
wd="${EXP_DIRECTORY:-/localpv/zxj/dune/apps/libdbos-experiments/tabby}"
ulimit -c 0

pushd $wd

echo "now at $wd, $tabby_compile_script"

bash "$tabby_compile_script"


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

concat_two_csvs() {
    # 定义文件名
    file1="$1"
    file2="$2"

    # 计算每个文件的行数（排除表头）
    lines1=$(($(wc -l < "$file1")-1))
    lines2=$(($(wc -l < "$file2")-1))

    # 确定较短文件的行数（不包括表头）
    if [ "$lines1" -lt "$lines2" ]; then
        k="$lines1"
    else
        k="$lines2"
    fi

    # 提取表头
    head -n 1 "$file1" > "header_file1.csv"
    head -n 1 "$file2" > "header_file2.csv"

    # 合并表头，假设两个文件的表头都需要保留
    paste -d, "header_file1.csv" "header_file2.csv" > $3

    # 从每个文件中提取最后k行（加上k+1来包括表头）
    tail -n $((k+1)) "$file1" > "temp_file1.csv"
    tail -n $((k+1)) "$file2" > "temp_file2.csv"

    # 合并这两部分，跳过一个表头的行
    tail -n +2 "temp_file1.csv" > "temp_file1_no_header.csv"
    tail -n +2 "temp_file2.csv" > "temp_file2_no_header.csv"
    paste -d, "temp_file1_no_header.csv" "temp_file2_no_header.csv" >> $3

    # 清理临时文件
    rm "header_file1.csv" "header_file2.csv" "temp_file1.csv" "temp_file2.csv" "temp_file1_no_header.csv" "temp_file2_no_header.csv" # $1 $2
}

generatePerfCSV() {
    result_iostat_name="$1"
    result_file_name="$2"
    result_name="$3"
    exe_path="$4"
    # iostat -d -y 1 nvme1n1p1 | awk 'BEGIN { OFS=","; print "kB_read/s","kB_wrtn/s"} /nvme1n1p1/ { print $3,$4;fflush(stdout) }' > $result_iostat_name &


    iostat -cdy 1 nvme1n1p1 | awk '
    BEGIN {
        OFS=",";
        print "Device","%user","%nice","%system","%iowait","%steal","%idle","tps","kB_read/s","kB_wrtn/s","kB_read","kB_wrtn"
    }
    /^Device:/ {
        getline; getline; # 跳过CPU统计信息的表头和空行
    }
    /nvme1n1p1/ {
        getline cpu; # 获取CPU统计行
        split(cpu, cpustats, " "); # 分割CPU统计行
        print $1,cpustats[1],cpustats[2],cpustats[3],cpustats[4],cpustats[5],cpustats[6],$2,$3,$4,$5,$6;;fflush(stdout)
    }' > $result_iostat_name &

    iostat -cdy 1 1 | awk '
    BEGIN { OFS=","; print "Device","%user","tps" }
    /CPU/ { getline; split($0, cpu); next }
    /^nvme1n1p1/ { print $1, cpu[1], $2 }
    '


    IOSTAT_PID=$!

    THREADS=64 DATASIZE=${size} RUNFOR=3 RNDREAD=1 READRATIO=100 zipfian=0 theta=0.9 BLOCK=/localpv/zxj/dune/apps/vmcache/vmfile VIRTGB=20 $exe_path | grep '^[(ts)|0-9].*,' | tail -n 11 > $result_file_name

    kill $IOSTAT_PID
    concat_two_csvs $result_iostat_name $result_file_name $result_name
}

database_sizes=(16777216 33554432 67108864 134217728)
database_sizes=(123456)

for size in "${database_sizes[@]}"; do
    echo $size
    # dbsize=$(expr $size \* 1024)
    dbsize_gb=$(expr $size \* 128 / 1073741824)
    echo "dbsize $dbsize_gb gb"

    result_prefix="$wd/data/${EXP_PREFIX_NAME:-tabby}_database_size"
    result_file_name="${result_prefix}_dbsize_${dbsize_gb}_schema.csv"
    result_iostat_name="${result_prefix}_dbsize_${dbsize_gb}_iostat.csv"
    result_name="${result_prefix}_dbsize_${dbsize_gb}.csv"
    
    generatePerfCSV $result_iostat_name $result_file_name $result_name "../../vmcache/tabby"
    
    # result_prefix="$wd/data/vmcache_database_size"
    # result_file_name="${result_prefix}_dbsize_${dbsize_gb}_schema.csv"
    # result_iostat_name="${result_prefix}_dbsize_${dbsize_gb}_iostat.csv"
    # result_name="${result_prefix}_dbsize_${dbsize_gb}.csv"

    # generatePerfCSV $result_iostat_name $result_file_name $result_name "../../vmcache/vmcache"

    # #  wired-tigers
    # result_prefix="$wd/data/wired_database_size"
    # result_file_name="${result_prefix}_dbsize_${dbsize_gb}_schema.csv"
    # result_iostat_name="${result_prefix}_dbsize_${dbsize_gb}_iostat.csv"
    # result_name="${result_prefix}_dbsize_${dbsize_gb}.csv"

    # iostat -d -y 1 nvme2n1 | awk 'BEGIN { OFS=","; print "kB_read/s","kB_wrtn/s"} /nvme2n1/ { print $3,$4;fflush(stdout) }' > $result_iostat_name &

    

    # IOSTAT_PID=$!

    # rm -rf ./leanstore
    # mkdir leanstore
    # ./../../../../leanstore/build/frontend/wiredtiger_ycsb -ycsb_payload_size 120 -run_for_seconds 120 -zipf_factor 0 -ycsb_tuple_count $size -dram-gib 4 -worker_threads 64 | grep '^[(ts)|0-9].*,' | tail -n 11 > $result_file_name

    # kill $IOSTAT_PID
    # concat_two_csvs $result_iostat_name $result_file_name $result_name

    # # # leanstore

    # result_prefix="$wd/data/lean_database_size"
    # result_file_name="${result_prefix}_dbsize_${dbsize_gb}_schema.csv"
    # result_iostat_name="${result_prefix}_dbsize_${dbsize_gb}_iostat.csv"
    # result_name="${result_prefix}_dbsize_${dbsize_gb}.csv"
    # rm -rf ./leanstore
    # touch leanstore
    # # one extra thread for leanstore for stats
    # iostat -d -y 1 nvme1n1p1 | awk 'BEGIN { OFS=","; print "kB_read/s","kB_wrtn/s"} /nvme1n1p1/ { print $3,$4;fflush(stdout) }' > $result_iostat_name &
    # IOSTAT_PID=$!
    # ./../../../../leanstore/build/frontend/ycsb -ycsb_payload_size 120 -run_for_seconds 120 -zipf_factor 0 -ycsb_tuple_count $size -dram-gib 4 -worker_threads 65 -ycsb_sleepy_thread 1 | grep '^[(ts)|0-9].*,' | tail -n 11 > $result_file_name
    # kill $IOSTAT_PID
    # concat_two_csvs $result_iostat_name $result_file_name $result_name
done


popd
