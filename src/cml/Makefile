ROOT = ../../src

include $(ROOT)/mk/env.mk
include $(ROOT)/mk/grpc.mk
include $(ROOT)/mk/objs.mk

.PHONY: all clean

OBJ_DEPS = \
$(DSERVICE)/flame_client.o

all: flame

flame: flame.cc $(OBJ_COMMON) $(OBJ_PROTO) $(OBJ_DEPS)
	$(CXX) $(CXXFLAGS) $(IGRPC) $^ -o $@ $(ISRC) $(GRPC_LDFLAGS)

%.o: %.cc
	$(CXX) $(CXXFLAGS) $^ -c $(ISRC)

clean:
	rm -f *.o