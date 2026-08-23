CXX := g++
VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
CPPFLAGS := $(shell pkg-config --cflags raylib libarchive libmagic vips openssl zlib 2>/dev/null) -DRAYMOTHFM_VERSION=\"$(VERSION)\"
CXXFLAGS := -std=c++20 -O3 -flto -Wall -Wextra -Wpedantic
LDFLAGS := -flto -s
LDLIBS := $(shell pkg-config --libs raylib libarchive libmagic vips openssl zlib 2>/dev/null) -lm -lpthread -ldl -lrt -lX11

TARGET := raymothfm
SOURCES := main.cpp
OBJECTS := $(SOURCES:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJECTS) -o $@ $(LDLIBS)

%.o: %.cpp rayicons.h VERSION
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

version:
	@echo raymothfm $(VERSION)

mkcfg: $(TARGET)
	./$(TARGET) --mkcfg

strip: $(TARGET)
	strip -s $(TARGET)

clean:
	rm -f $(TARGET) $(OBJECTS)

.PHONY: all run version mkcfg strip clean
