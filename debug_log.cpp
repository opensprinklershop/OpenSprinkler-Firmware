#include "debug_log.h"
#include <string.h>
#include <stdlib.h>

DebugBufferClass debug_buffer;

DebugBufferClass::DebugBufferClass() : buffer(NULL), head(0), size(0), wrapped(false) {
}

DebugBufferClass::~DebugBufferClass() {
    if (buffer) {
        free(buffer);
    }
}

void DebugBufferClass::begin(size_t sz) {
    if (buffer) {
        free(buffer);
        buffer = NULL;
    }
    size = sz;
    head = 0;
    wrapped = false;
    if (size > 0) {
        buffer = (char*)malloc(size);
        if (buffer) {
            memset(buffer, 0, size);
        }
    }
}

void DebugBufferClass::clear() {
    head = 0;
    wrapped = false;
    if (buffer && size > 0) {
        memset(buffer, 0, size);
    }
}

size_t DebugBufferClass::write(uint8_t c) {
    if (!buffer || size == 0) return 0;
    
    buffer[head] = (char)c;
    head++;
    if (head >= size) {
        head = 0;
        wrapped = true;
    }
    return 1;
}

size_t DebugBufferClass::write(const uint8_t *buf, size_t len) {
    if (!buffer || size == 0 || !buf || len == 0) return 0;
    
    for (size_t i = 0; i < len; i++) {
        write(buf[i]);
    }
    return len;
}

#if !defined(ARDUINO)
void DebugBufferClass::print(const char* s) {
    if (s) write((const uint8_t*)s, strlen(s));
}

void DebugBufferClass::print(char c) {
    write((uint8_t)c);
}

void DebugBufferClass::print(int n) {
    char tmp[32];
    sprintf(tmp, "%d", n);
    print(tmp);
}

void DebugBufferClass::print(unsigned int n) {
    char tmp[32];
    sprintf(tmp, "%u", n);
    print(tmp);
}

void DebugBufferClass::print(long n) {
    char tmp[32];
    sprintf(tmp, "%ld", n);
    print(tmp);
}

void DebugBufferClass::print(unsigned long x) {
    char tmp[32];
    sprintf(tmp, "%lu", x);
    print(tmp);
}

void DebugBufferClass::print(double d) {
    char tmp[32];
    sprintf(tmp, "%.2f", d);
    print(tmp);
}

void DebugBufferClass::println() {
    print("\n");
}

void DebugBufferClass::println(const char* s) {
    print(s);
    println();
}

void DebugBufferClass::printf(const char* format, ...) {
    char tmp[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    if (len > 0) {
        write((const uint8_t*)tmp, len);
    }
}
#endif
