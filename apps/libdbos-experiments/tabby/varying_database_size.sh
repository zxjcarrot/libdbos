#!/bin/bash
set -x
tabby_compile_script="${TABBY_COMPILE_SCRIPT}"
wd="${EXP_DIRECTORY:-/localpv/zxj/dune/apps/libdbos-experiments/tabby}"
ulimit -c 0

pushd $wd

echo "now at $wd, $tabby_compile_script"

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
    rm "header_file1.csv" "header_file2.csv" "temp_file1.csv" "temp_file2.csv" "temp_file1_no_header.csv" "temp_file2_no_header.csv" $1 $2
}

generateIOStat() {
    path2=$1
    iostat -cdxy 1 $path2 | awk -v disk="$path2" '
    BEGIN {
        OFS=",";
        print "Device","%user","%system","%iowait","%idle","%util","kB_read/s","kB_wrtn/s";
    }

    /^avg-cpu:/ {
        getline;
        cpu_user = $1;
        cpu_system = $3;
        cpu_iowait = $4;
        cpu_idle = $NF;
    }

    /Device:/ {
        device_header = 1;
        next;
    }

    $1 == disk && device_header {
        print $1, cpu_user, cpu_system, cpu_iowait, cpu_idle, $14, $6, $7;fflush(stdout)
    }
    ' > $result_iostat_name &
}

nvmedev="nvme3n1p3"

#database_sizes=(8388608 33554432 67108864)
database_sizes=(8388608 536870912)
readRatio=(100 50)
zipfFactor=(0 0.9)
runForSeconds=240
blockPath="/dev/$nvmedev"
threads=64

generatePerfCSV() {
    result_iostat_name="$1"
    result_file_name="$2"
    result_name="$3"
    exe_path="$4"
    exmap=$5
    execution_log="$result_file_name.log"
    # iostat -d -y 1 nvme1n1p1 | awk 'BEGIN { OFS=","; print "kB_read/s","kB_wrtn/s"} /nvme1n1p1/ { print $3,$4;fflush(stdout) }' > $result_iostat_name &

    generateIOStat $nvmedev

    IOSTAT_PID=$!

    THREADS=$threads DATASIZE=${size} RUNFOR=$runForSeconds RNDREAD=1 READRATIO=$ratio zipfian=1 theta=$zipff BLOCK=$blockPath VIRTGB=100 PHYSGB=8 $exe_path &> $execution_log
    cat $execution_log | grep -E '^((ts|[0-9]+),(tx|[0-9]+))' > $result_file_name

    kill $IOSTAT_PID
    concat_two_csvs $result_iostat_name $result_file_name $result_name
}


# touch leanstore_file
# dd if=/dev/zero of=leanstore_file bs=1M count=81920

# ssdPath="/Higgs2/
# database_sizes=(123456)
for zipff in "${zipfFactor[@]}"; do
    for ratio in "${readRatio[@]}"; do
        for size in "${database_sizes[@]}"; do
            echo $ratio
            echo $size
            echo $zipff
            # dbsize=$(expr $size \* 1024)
            dbsize_gb=$(expr $size \* 128 / 1073741824)
            echo "dbsize $dbsize_gb gb"

            result_prefix="$wd/data/${EXP_PREFIX_NAME:-tabby}_database_size"
            result_file_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_schema.csv"
            result_iostat_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_iostat.csv"
            result_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}.csv"
            
            generatePerfCSV $result_iostat_name $result_file_name $result_name "../../vmcache/tabby" 0
            
            result_prefix="$wd/data/vmcache_database_size"
            result_file_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_schema.csv"
            result_iostat_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_iostat.csv"
            result_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}.csv"

            generatePerfCSV $result_iostat_name $result_file_name $result_name "../../vmcache/vmcache" 0

            # result_prefix="$wd/data/exmap_database_size"
            # result_file_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_schema.csv"
            # result_iostat_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_iostat.csv"
            # result_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}.csv"

            # generatePerfCSV $result_iostat_name $result_file_name $result_name "../../vmcache/vmcache" 1

            #  wired-tigers
            result_prefix="$wd/data/wired_database_size"
            result_file_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_schema.csv"
            result_iostat_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_iostat.csv"
            result_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}.csv"
            execution_log="$result_file_name.log"
            # generateIOStat nvme1n1p1

            IOSTAT_PID=$!
            
            # finalPath = "${ssd_path}/leanstore"
            rm -rf ./leanstore
            mkdir leanstore

            # ./../../../../leanstore/build/frontend/wiredtiger_ycsb --mv=false --isolation_level=si --wal=false --pp_threads=4 -ycsb_read_ratio $ratio -ycsb_payload_size 120 -run_for_seconds $runForSeconds -zipf_factor $zipff -ycsb_tuple_count $size -dram_gib 8 -worker_threads $threads &> $execution_log
            # cat $execution_log | grep '^[(ts)|0-9].*,' > $result_file_name

            kill $IOSTAT_PID
            #  concat_two_csvs $result_iostat_name $result_file_name $result_name

            # leanstore

            result_prefix="$wd/data/lean_database_size"
            result_file_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_schema.csv"
            result_iostat_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}_iostat.csv"
            result_name="${result_prefix}_dbsize_${dbsize_gb}_${ratio}_${zipff}.csv"
            execution_log="$result_file_name.log"
            #rm -rf ./leanstore
            #touch leanstore
            # one extra thread for leanstore for stats
            # generateIOStat $nvmedev
            # IOSTAT_PID=$!
            # pp_threads=8
            # ./../../../../leanstore/build/frontend/ycsb --bulk_insert=true --ssd_path=$blockPath --vi=false --mv=false  --isolation_level=ru --wal=false --pp_threads=$pp_threads -ycsb_read_ratio $ratio -ycsb_payload_size 120 -run_for_seconds $runForSeconds -zipf_factor $zipff -ycsb_tuple_count $size -dram_gib 8 -worker_threads `expr $threads - $pp_threads + 1` -ycsb_sleepy_thread 1 &> $execution_log
            # #./../../../../leanstore/build/frontend/ycsb  --vi=false --mv=false --isolation_level=ru --wal=false --pp-threads=4 -ycsb_read_ratio 100 -ycsb_payload_size 120 -run_for_seconds 180 -zipf_factor 0.9 -ycsb_tuple_count 134217728 -dram_gib 8  --xmerge -worker_threads 61 --pin_threads -ycsb_sleepy_thread 1 
            # cat $execution_log | grep '^[(ts)|0-9].*,' > $result_file_name
            # kill $IOSTAT_PID
            # concat_two_csvs $result_iostat_name $result_file_name $result_name
        done
    done
done


popd
