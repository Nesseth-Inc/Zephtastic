#pragma once
struct StubConfig { struct { bool override_duty_cycle=false; } lora; };
inline StubConfig config;
#define LOG_DEBUG(...) ((void)0)
#define LOG_WARN(...) ((void)0)
