#ifndef _DEBUG_LOG_H
#define _DEBUG_LOG_H

#include <stddef.h>
#include <stdint.h>

#if defined(ARDUINO)
#include <Arduino.h>
#include <Print.h>
class DebugBufferClass : public Print {
#else
#include <string>
#include <vector>
#include <stdarg.h>
class DebugBufferClass {
#endif

private:
    char* buffer;
    size_t head;
    size_t size;
    bool wrapped;

public:
    DebugBufferClass();
    ~DebugBufferClass();
    void begin(size_t sz);

#if defined(ARDUINO)
    virtual size_t write(uint8_t c) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;
#else
    size_t write(uint8_t c);
    size_t write(const uint8_t *buffer, size_t size);
    void print(const char* s);
    void print(char c);
    void print(int n);
    void print(unsigned int n);
    void print(long n);
    void print(unsigned long x);
    void print(double d);
    void println();
    void println(const char* s);
    void printf(const char* format, ...);
#endif

    void clear();
    
    const char* get_buffer() const { return buffer; }
    size_t get_head() const { return head; }
    size_t get_size() const { return size; }
    bool is_wrapped() const { return wrapped; }
};

extern DebugBufferClass debug_buffer;

#endif // _DEBUG_LOG_H
