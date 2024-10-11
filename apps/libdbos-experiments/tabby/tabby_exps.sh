cwd=`pwd`
TABBY_COMPILE_SCRIPT=compile_tabby.sh EXP_DIRECTORY=$cwd bash varying_database_size.sh
TABBY_COMPILE_SCRIPT=compile_tabby.sh EXP_DIRECTORY=$cwd bash tpcc.sh
# TABBY_COMPILE_SCRIPT=compile_tabby.sh EXP_DIRECTORY=$cwd bash varying_database_size_my.sh
