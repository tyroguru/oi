.DEFAULT_GOAL := all

CXX      := clang++
CXXFLAGS := -g -std=c++20 -DOIL_AOT_COMPILATION=1
INC      := -I../../include

OILGEN     := ../../build/oilgen
OID_CONFIG := ../../build/testing.oid.toml

# oilgen parses OilVectorOfStrings.cpp directly with libTooling (no
# debug info/drgn involved), so it needs the same header search paths a
# normal compile of that file would use. We ask $(CXX) for its own default
# search paths rather than hardcoding a toolchain-specific list, so this
# keeps working whether $(CXX) is a Nix-provided compiler or a system one.
SYSTEM_INCLUDES := $(shell echo | $(CXX) -std=c++20 -E -x c++ -v - 2>&1 | \
	sed -n '/#include <\.\.\.> search starts here/,/End of search list/p' | \
	grep '^ ' | sed 's/^ /-isystem/')

.PHONY: all clean run oilgen-bin

all: OilVectorOfStrings

# Build oilgen (and everything else in the repo) from the top-level project.
# Real projects should not need this step: it exists here only because
# oilgen isn't currently part of OIL's installable CMake package (see the
# README), so the only way to get it today is to build this monorepo.
oilgen-bin:
	ninja -C ../../build oilgen

$(OID_CONFIG):
	../../tools/config_gen.py -c $(CXX) $(OID_CONFIG)

# Pass 1: compile the source normally. This produces an object file that
# *declares* (but does not define) introspectImpl<Foo> as a weak symbol -
# see include/oi/oi.h.
OilVectorOfStrings.o: OilVectorOfStrings.cpp
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

# Pass 2: run oilgen over the *same* source file to generate the missing
# introspectImpl<Foo> definition as a second object file.
OilIntrospection.o: OilVectorOfStrings.cpp oilgen-bin $(OID_CONFIG)
	$(OILGEN) --config-file=$(OID_CONFIG) --output=$@ $< \
		-- $(CXXFLAGS) $(INC) $(SYSTEM_INCLUDES)

# Link both object files, plus liboil (the IntrospectionResult/exporters
# runtime support code), into the final binary.
OilVectorOfStrings: OilVectorOfStrings.o OilIntrospection.o
	$(CXX) $^ -L../../build -Wl,-rpath,$(abspath ../../build) -loil -o $@

run: OilVectorOfStrings
	./OilVectorOfStrings

clean:
	rm -f OilVectorOfStrings OilVectorOfStrings.o OilIntrospection.o jit.cpp

