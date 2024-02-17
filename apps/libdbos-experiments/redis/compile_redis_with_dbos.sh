
pushd ../../redis/

# Execute your commands here
make distclean;
make -j ENABLE_DBOS=yes
# Return to the original directory
popd