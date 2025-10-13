// ───────────────── Shared packet formats ─────────────────
#pragma once
#include <Arduino.h>

#pragma pack(push,1)

// Message types
static const uint8_t MSG_START_V1 = 0xA1;   // TX → RX
static const uint8_t MSG_READY_V1 = 0xB1;   // RX → TX (ACK payload)

// START packet: 17 bytes total
struct StartPacketV1 {
  uint8_t  type;          // = 0xA1
  uint16_t seq;           // batch sequence
  uint32_t masterStart;   // master's micros() t0 (same as your string version)
  uint8_t  volume;        // 0..30
  uint8_t  steps;         // 3 or 4
  uint16_t t_ds[4];       // times in deciseconds from t0: red, orange, green, off
};

// READY ack payload: 3 bytes total
struct ReadyAckV1 {
  uint8_t  type;          // = 0xB1
  uint16_t seq;           // echoes seq
};

#pragma pack(pop)

// Optional static checks (C++11)
static_assert(sizeof(StartPacketV1) == 17, "StartPacketV1 must be 17 bytes");
static_assert(sizeof(ReadyAckV1)   == 3,  "ReadyAckV1 must be 3 bytes");
