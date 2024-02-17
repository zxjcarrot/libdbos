
pushd ../../redis/

# Execute your commands here
make distclean;
make -j
# Return to the original directory
popd