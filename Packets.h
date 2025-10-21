// ───────────────── Shared packet formats ─────────────────
#pragma once
#include <Arduino.h>

#pragma pack(push,1)

enum CommandType : uint8_t {
  CMD_FLASH   = 1,
};

// Message types
static const uint8_t MSG_START_V1     = 0xA1;   // TX → RX
static const uint8_t MSG_DISCOVER_V1  = 0xA2;
static const uint8_t MSG_BROADCAST_V1 = 0xA3;
static const uint8_t MSG_READY_V1     = 0xB1;   // RX → TX (ACK payload)


// START packet: 17 bytes total
struct StartPacketV1 {
  uint8_t  type;          // = 0xA1
  uint16_t seq;           // unique message id
  uint8_t  targetId;      // which device should run this
  uint32_t currentClock;  // master's current micros()
  uint32_t masterStart;   // master's micros() to start from
  uint8_t  volume;        // 0..30
  uint8_t  steps;         // 3 or 4
  uint16_t t_ds[4];       // times in deciseconds from t0: red, orange, green, off
};

struct DiscoverPacketV1 {
  uint8_t  type;          // = MSG_DISCOVER_V1
  uint16_t seq;           // unique message id
  uint8_t  targetId;      // which device this is meant for
};

struct BroadcastPacketV1 {
  uint8_t  type;          // = 0xA1
  uint16_t seq;           // unique message id
  uint8_t  command;       // numeric command ID (see enum above)
  // allowing commands so that it's future proof if we want to add in more broadcast commands in future
};

// READY ack payload: 3 bytes total
struct ReadyAckV1 {
  uint8_t  type;          // = 0xB1
  uint16_t seq;           // echoes seq
};

#pragma pack(pop)

// Optional static checks (C++11)
static_assert(sizeof(StartPacketV1) == 22, "StartPacketV1 must be 22 bytes");
static_assert(sizeof(DiscoverPacketV1) == 4, "DiscoverPacketV1 must be 4 bytes");
static_assert(sizeof(BroadcastPacketV1) == 4, "BroadcastPacketV1 must be 4 bytes");
static_assert(sizeof(ReadyAckV1)   == 3,  "ReadyAckV1 must be 3 bytes");
