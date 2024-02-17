cwd=`pwd`
#KEYDB_COMPILE_SCRIPT=compile_keydb_with_odf.sh EXP_DIRECTORY=$cwd EXP_PREFIX_NAME=/data/keydb_odf bash varying_keydb_server_threads.sh
KEYDB_COMPILE_SCRIPT=compile_keydb_with_linux.sh EXP_DIRECTORY=$cwd EXP_PREFIX_NAME=/data/keydb_linux bash varying_keydb_server_threads.sh
#KEYDB_COMPILE_SCRIPT=compile_keydb_with_dbos.sh EXP_DIRECTORY=$cwd EXP_PREFIX_NAME=/data/keydb_dbos bash varying_keydb_server_threads.sh
#
