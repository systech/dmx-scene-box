// DMX Scene Box — build-time configuration.
#pragma once
#include <EQSP32.h>

#define FW_NAME            "DMXSceneBox"
#define FW_VERSION         "0.2.0"

// ---- DMX ----
#define DMX_FRAME_MS       25        // 40 Hz refresh
#define DMX_CHANNELS       512
#define DMX_BAUD           250000
#define DMX_BREAK_BAUD     90000     // one 0x00 byte at this rate = 100 us break + 22 us MAB

// ---- Inputs (EQSP32 terminal numbers) ----
#define IN_FIRST           EQ_PIN_1  // trigger inputs 1..14 are mapped via triggers.txt
#define IN_LAST_TRIGGER    EQ_PIN_14
#define IN_OVERRIDE        EQ_PIN_15 // manual override: full house lights
#define IN_SETUP_MODE      EQ_PIN_16 // level: high = setup mode (web server on)

// ---- Files ----
#define FS_PARTITION       "spiffs"  // partition label from "8M with spiffs"
#define FILE_TRIGGERS      "/triggers.txt"
#define FILE_DESCRIPTION   "/description.yaml"
#define FILE_EDITING       "/editing.scene"
#define FILE_BOOT          "/boot.scene"
#define FILE_OVERRIDE      "/override.scene"
#define SCENE_EXT          ".scene"
#define MAX_FILE_BYTES     (64 * 1024)
#define MAX_NAME_LEN       40

// ---- Behaviour ----
#define DEFAULT_FADE_MS    0
#define SETUP_DEBOUNCE_MS  300
#define HTTP_PORT          80
