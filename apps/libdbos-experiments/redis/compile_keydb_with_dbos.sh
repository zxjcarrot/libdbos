
pushd ../../KeyDB/

# Execute your commands here
make distclean;make -j ENABLE_DBOS=yes BUILD_TLS=no USE_TCMALLOC_MINIMAL=yes
# Return to the original directory
popd