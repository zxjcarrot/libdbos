cwd=`pwd`
#REDIS_COMPILE_SCRIPT=compile_redis_with_odf.sh EXP_DIRECTORY=$cwd EXP_PREFIX_NAME=/data/redis_odf bash varying_database_size.sh
REDIS_COMPILE_SCRIPT=compile_redis_with_linux.sh EXP_DIRECTORY=$cwd EXP_PREFIX_NAME=/data/redis_linux bash varying_database_size.sh
REDIS_COMPILE_SCRIPT=compile_redis_with_dbos.sh EXP_DIRECTORY=$cwd EXP_PREFIX_NAME=/data/redis_dbos bash varying_database_size.sh
#

#REDIS_COMPILE_SCRIPT=compile_redis_with_odf.sh EXP_DIRECTORY=$cwd EXP_PREFIX_NAME=/data/redis_odf bash varying_redis_rw_ratio.sh
#REDIS_COMPILE_SCRIPT=compile_redis_with_linux.sh EXP_DIRECTORY=$cwd EXP_PREFIX_NAME=/data/redis_linux bash varying_redis_rw_ratio.sh
#REDIS_COMPILE_SCRIPT=compile_redis_with_dbos.sh EXP_DIRECTORY=$cwd EXP_PREFIX_NAME=/data/redis_dbos bash varying_redis_rw_ratio.sh
#
