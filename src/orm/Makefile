ROOT = ../../src

include $(ROOT)/mk/env.mk

.PHONY: all my_impl clean

all: stmt.o my_impl engine.o

my_impl:
	make -C ./my_impl

%.o: %.cc
	$(CXX) $(CXXFLAGS) $^ -c $(ISRC)

clean:
	make -C ./my_impl clean
	rm -f *.o