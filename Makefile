CXX ?= clang++
CXXFLAGS := -std=c++17 -Wall -Wextra
LDFLAGS := -framework CoreFoundation -framework IOKit

OBJS := main.o

all: macvdmtool

macvdmtool: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

ifeq ($(PREFIX),)
    PREFIX := /usr/local
endif

INSTALL := /usr/bin/install

.PHONY: install clean
install: macvdmtool
	@sudo $(INSTALL) -d "$(DESTDIR)$(PREFIX)/bin/"
	@sudo $(INSTALL) -m 755 -o root -g wheel "$(<)" "$(DESTDIR)$(PREFIX)/bin/"

clean:
	rm -f $(OBJS) macvdmtool
