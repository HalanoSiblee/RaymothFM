CXX := g++
VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
CPPFLAGS := $(shell pkg-config --cflags raylib libarchive libmagic vips openssl zlib 2>/dev/null) -DRAYMOTHFM_VERSION=\"$(VERSION)\"
CXXFLAGS := -std=c++20 -O3 -flto -Wall -Wextra -Wpedantic
LDFLAGS := -flto -s
LDLIBS := $(shell pkg-config --libs raylib libarchive libmagic vips openssl zlib 2>/dev/null) -lm -lpthread -ldl -lrt -lX11

TARGET := raymothfm
DEBUG_TARGET := raymothfm-debug
SOURCES := main.cpp
OBJECTS := $(SOURCES:.cpp=.o)
DEBUG_OBJECTS := main.debug.o

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJECTS) -o $@ $(LDLIBS)

%.o: %.cpp rayicons.h VERSION
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# Debug build: no stripping/LTO, full symbols, frame pointers, ASan + UBSan.
debug: CXXFLAGS := -std=c++20 -O0 -g3 -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -fsanitize=address,undefined

debug: LDFLAGS := -fsanitize=address,undefined

debug: $(DEBUG_TARGET)

$(DEBUG_TARGET): $(DEBUG_OBJECTS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(DEBUG_OBJECTS) -o $@ $(LDLIBS)

main.debug.o: main.cpp rayicons.h VERSION
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

run-debug: $(DEBUG_TARGET)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 ./$(DEBUG_TARGET)

version:
	@echo raymothfm $(VERSION)

mkcfg: $(TARGET)
	./$(TARGET) --mkcfg

strip: $(TARGET)
	strip -s $(TARGET)

clean:
	rm -f $(TARGET) $(DEBUG_TARGET) $(OBJECTS) $(DEBUG_OBJECTS)

.PHONY: all debug run run-debug version mkcfg strip clean
