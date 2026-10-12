# DMX Scene Box

Lighting scenes for a room, a facade, a stage, a shop. You describe the scene in words to an AI assistant, the box plays it when someone presses a button. No lighting software. No proprietary controller.

The box is a 155 EUR industrial PLC. The scenes are small text files on it. The box carries its own manual, so any AI assistant that can reach it over the network can read the manual and program it for you.

## What you need

<img src="https://erqos.com/wp-content/uploads/2025/07/EQSP32CE-product-picture-400x278.png" alt="Erqos EQSP32CE" width="300" align="right">

- An [Erqos EQSP32CE](https://erqos.com/product/eqsp32ce/) controller (DIN rail, Ethernet, RS485, 16 inputs).
- A 24 V DC power supply.
- Your DMX lights, with a 120 ohm terminator at the end of the line.
- Push buttons, or any dry contact, for the triggers. A key switch for setup mode.
- A laptop with a USB-C cable, once, for flashing.
- An AI assistant that can run commands on a computer in the same network as the box. Claude Code, Grok with a local tool, Codex, or similar. A chat app whose tools run in the cloud cannot reach your network.

Product photo: Erqos Technologies.

## Wiring

| EQSP32 terminal | Connect to |
| --- | --- |
| GND, VIn | 24 V supply |
| TxD / 485A | DMX Data+ |
| RxD / 485B | DMX Data- |
| GND | DMX ground |
| Inputs 1 to 14 | buttons, 24 V from the same supply when pressed |
| Input 15 | override button: everything to full |
| Input 16 | setup key switch |
| Ethernet | your network |

![Bench setup](docs/bench.jpg)

## Setting it up

**1. Flash the box.** Unplug VIn, plug in USB-C, and tell your coding assistant:

> Clone https://github.com/systech/dmx-scene-box and flash `firmware/dmx_scene_box` to the EQSP32 on USB. The README says which board and library versions to use.

Or do it by hand, see "Flashing by hand" below.

**2. Plug in Ethernet.** The box gets an address from your router. The serial monitor at 115200 baud prints it, or look in the router. Give it a fixed lease.

Or use Wi-Fi: tell your assistant the network name and password and ask it to put them in `wifi_secrets.h` and flash again. Ethernet wins when both are connected.

**3. Turn the setup key.** Input 16 high. The box now listens on the network and plays `editing.scene`, a dim test look.

**4. Talk to it.** Tell your assistant:

> There is a DMX controller on my network at http://192.168.1.50 and it is in setup mode. Download http://192.168.1.50/api/files/description.yaml and read it first. Then make a scene called "evening" that fades everything to warm white over 2 seconds, with a short beep, and play it.

It reads the manual, writes the file, uploads it, and the lights change. Then:

> Make "wave": a slow blue wave running across the eight strips, 6 seconds per cycle. Then map button 1 to evening, button 2 to wave, button 3 to off.

**5. Turn the key back.** The network goes quiet. Buttons fire scenes. A power cut brings back the last scene. That is it.

### Finding out what is wired where

Old installations rarely have correct drawings. In setup mode the assistant can light one channel at a time (`/api/probe?ch=5`) while you say what you see, and write the fixture list into the manual on the box. One afternoon, and the box documents your room.

## Under the hood

This part is for the curious, and for AI assistants that want the full picture. The box also serves it as `description.yaml`.

### Scene files

A static scene is a list of channel values. Optional fade and beeps.

```
# evening.scene
fade 2000
beep 880 80
1-24 255
30 120
```

A dynamic scene is a Lua script. The first line starts with `--`. The box calls `update(t)` every 20 ms with the elapsed milliseconds. The script computes the look from `t`; it never sleeps and never counts.

```lua
-- wave.scene: blue wave over 8 strips, channels 1-24 as RGB triples
function update(t)
  for s = 0, 7 do
    local v = 128 + 127 * math.sin(t / 955 - s * 0.6)
    dmx.set(s * 3 + 3, v)
  end
end
```

Vocabulary: `dmx.set(ch, v)`, `dmx.fill(lo, hi, v)`, `dmx.get(ch)`, `dmx.beep(hz, ms)`, `log(...)`, plus Lua `math`, `string`, `table`. Limits: 64 kB heap, 400k instructions per tick. A script error freezes the last frame. It never blacks out the room.

`triggers.txt` maps inputs to scenes, one line each: `1 evening`. `boot.scene`, if present, plays at power-up. `override.scene`, if present, is what input 15 plays.

### API

Only in setup mode, port 80, no login. The key switch is the lock.

| Method | Path | Does |
| --- | --- | --- |
| GET | `/api/status` | mode, scene, script state, IP, uptime |
| GET | `/api/files` | list files |
| GET | `/api/files/{name}` | download |
| PUT | `/api/files/{name}` | upload, body = file, header `Content-Type: text/plain` |
| DELETE | `/api/files/{name}` | delete |
| POST | `/api/play/{name}` | play now |
| GET | `/api/probe?ch=5&to=8&v=255` | light channels 5 to 8, zero the rest |

Uploading `editing.scene` applies it live. Uploading `triggers.txt` reloads the map. Files are validated before they are stored; a bad one is rejected with the line number.

Backup and restore from a laptop, standard Python only:

```
python tools/dmxbox.py 192.168.1.50 pull backup/
python tools/dmxbox.py 192.168.1.50 push backup/
```

### How it is built

- DMX output runs in its own real-time task at 40 Hz with linear crossfades. The web server cannot disturb it.
- Inputs are edge-triggered, latest wins. A stuck-high input fires once and then does nothing.
- Lua 5.4.7 is vendored without file, OS and module libraries, with a bounded allocator and an instruction hook.
- Files live in LittleFS, 1.5 MB. The last scene name is kept in non-volatile memory.
- In normal mode nothing listens on the network.

### Flashing by hand

The vendor library is a precompiled binary, so these versions are fixed.

1. Arduino IDE 2, Boards Manager URL `https://espressif.github.io/arduino-esp32/package_esp32_index.json`, install **esp32 by Espressif 2.0.17** (not 3.x).
2. Library Manager: **EQSP32 1.5.2**, **RadioLib 7.7.1**.
3. Board **ESP32S3 Dev Module**, Flash size **8MB**, Partition scheme **8M with spiffs**.
4. Open `firmware/dmx_scene_box/dmx_scene_box.ino`, upload over USB-C with VIn unplugged.

With arduino-cli:

```
arduino-cli core install esp32:esp32@2.0.17
arduino-cli lib install EQSP32@1.5.2 RadioLib@7.7.1
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB firmware/dmx_scene_box
arduino-cli upload  --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB -p COM13 firmware/dmx_scene_box
```

Wi-Fi is optional: copy `wifi_secrets.example.h` to `wifi_secrets.h`, fill it in, flash again. Ethernet wins when both are connected.

Unsure about the wiring? Flash `examples/hardware_check` first. It cycles colours on channels 1 to 3 and reacts to inputs 9 to 12, with no network and no files.

## Why

A stand-alone DMX controller usually means proprietary hardware and a confusing editor program. This is an off-the-shelf PLC, plain text files, and a conversation. The firmware and this page were built in one day with an AI assistant.

## Licence

MIT. Lua is Copyright (C) 1994-2024 Lua.org, PUC-Rio, MIT licence, see `firmware/dmx_scene_box/src/lua/LICENSE-lua.txt`. The EQSP32 library and product photo belong to Erqos Technologies.
