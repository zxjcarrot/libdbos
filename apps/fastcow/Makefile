fastcow: fastcow.cpp 
	g++ -DNDEBUG -g -MD -O3 -DENABLE_DUNE -std=c++20 -I../../libdune -no-pie -mincoming-stack-boundary=3 -g -fnon-call-exceptions -fasynchronous-unwind-tables asm_memcpy.S asm_memset.S fastcow.cpp -o fastcow -L../../libdune -ldune -lbfd -ldl -lpthread

clean:
	rm fastcow
