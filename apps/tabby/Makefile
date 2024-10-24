all: vmcache tabby tabby_baseline tabby_baseline_priv tabby_no_preallocation_and_no_allocator_striped_lock tabby_no_preallocation
vmcache: vmcache.cpp tpcc/*pp
	g++ -O3 -DNDEBUG -std=c++20 -no-pie -mincoming-stack-boundary=3 -g -fnon-call-exceptions -fasynchronous-unwind-tables vmcache.cpp -o vmcache -laio -lpthread -ldl

tabby_baseline: tabby_baseline.cpp tpcc/*pp # vmcache
	g++ -O3 -DNDEBUG -DENABLE_DUNE -std=c++20 -I../../libdune -no-pie -mincoming-stack-boundary=3 -g -fnon-call-exceptions -fasynchronous-unwind-tables tabby_baseline.cpp -o tabby_baseline -laio  -ldl -lpthread

tabby_baseline_priv: tabby_baseline_priv.cpp tpcc/*pp # running vmcache with elevated privilege level
	g++ -O3 -DNDEBUG -DENABLE_DUNE -std=c++20 -I../../libdune -no-pie -mincoming-stack-boundary=3 -g -fnon-call-exceptions -fasynchronous-unwind-tables tabby_baseline_priv.cpp -o tabby_baseline_priv -laio -L../../libdune -ldune -lbfd -ldl -lpthread

tabby_no_preallocation_and_no_allocator_striped_lock: tabby.cpp tpcc/*pp # tabby with tlb shootdown elimination algorithm
	g++ -O3 -DNDEBUG -DENABLE_DUNE -std=c++20 -I../../libdune -no-pie -mincoming-stack-boundary=3 -g -fnon-call-exceptions -fasynchronous-unwind-tables tabby.cpp -o tabby_no_preallocation_and_no_allocator_striped_lock -laio -L../../libdune -ldune -lbfd -ldl -lpthread

tabby_no_preallocation: tabby.cpp tpcc/*pp # tabby with tlb shootdown elimination algorithm + stripped locks
	g++ -O3 -DTABBY_ALLOCATOR_STRIPED_LOCK -DNDEBUG -DENABLE_DUNE -std=c++20 -I../../libdune -no-pie -mincoming-stack-boundary=3 -g -fnon-call-exceptions -fasynchronous-unwind-tables tabby.cpp -o tabby_no_preallocation -laio -L../../libdune -ldune -lbfd -ldl -lpthread

tabby: tabby.cpp tpcc/*pp # tabby with all optimizations: tlb shootdown elimination, preallocation, stripped locks
	g++ -O3 -DTABBY_ALLOCATOR_STRIPED_LOCK -DTABBY_PREALLOCATION -DNDEBUG -DENABLE_DUNE -std=c++20 -I../../libdune -no-pie -mincoming-stack-boundary=3 -g -fnon-call-exceptions -fasynchronous-unwind-tables tabby.cpp -o tabby -laio -L../../libdune -ldune -lbfd -ldl -lpthread


vmcache_test_dune: array_test.cpp
	g++ -DNDEBUG  -O3 -std=c++20  -I../../libdune -no-pie -mincoming-stack-boundary=3  -g -fnon-call-exceptions -fasynchronous-unwind-tables array_test.cpp -o array_test_dune -DENABLE_DUNE -L../../libdune -ldune 

vmcache_test: array_test.cpp
	g++ -DNDEBUG -O3 -std=c++20 -g -fnon-call-exceptions -fasynchronous-unwind-tables array_test.cpp -o array_test   -I../../libdune -no-pie -mincoming-stack-boundary=3 

clean:
	rm vmcache -f
	rm tabby -f
	rm tabby_baseline -f
	rm tabby_baseline_priv -f
	rm tabby_no_preallocation_and_no_allocator_striped_lock -f
	rm tabby_no_preallocation -f
	rm perf.data -f
	rm perf.data.old -f
	rm output.csv -f