CXXFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

paravr: paravr.cpp
	$(CXX) -std=c++17 $(CXXFLAGS) $(LDFLAGS) -o $@ $<

install: paravr
	install -Dm755 paravr $(DESTDIR)$(PREFIX)/bin/paravr

clean:
	rm -f paravr

.PHONY: install clean
