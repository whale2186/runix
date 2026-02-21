
# ESP32 OS (runix)

A lightweight, web-driven operating system prototype built for the ESP32.  
This project demonstrates cooperative multitasking, a simple scripting engine, web-based file management, and live system monitoring — all running on an ESP32 using LittleFS.


<img width="200" height="200" alt="Image" src="https://github.com/user-attachments/assets/cd230cd9-9fb1-42a8-82e0-99da21775b3b" />


---
## Showcase

https://github.com/user-attachments/assets/aa02f79a-840b-4d3d-98e1-c4d44e738aa5

---

## Repository contents

- `runix.ino` — Main Arduino program (task scheduler, HTTP server, script engine).
- `/apps` — Example scripts and web apps used by the dashboard (`*.run`, `*.wx`, `*.html`).
- `/sysapps` — Endpoint mappings created at runtime.
- `/runix.bin` — Precompiled binary that can be uploaded directly use SSID: SSID PASSWORD: PASSWORD.
- `/res` — Images and Videos used in this README file.
- `README.md` — This file.

> Note: paths above refer to files on the ESP32's LittleFS image.

---

## Quick overview

The project exposes a web dashboard and several REST endpoints. Scripts use a lightweight syntax (commands with parentheses) and run cooperatively: each script line executes and then yields control, which allows multiple scripts to run concurrently.

Key components:
- HTTP server (WebServer) serving static files and API endpoints.
- A script engine with commands such as `delayms(...)`, `logmsg(...)`, `calc(...)`, `runbg(...)`, `kill(...)`, `pause(...)`, `resume(...)`.
- Cooperative scheduler that executes one script line per tick, with non-blocking delays.
- System monitor exposing `/status` and `/tasks` for UI polling.

---

## Setup and installation

1. Open the Arduino IDE (or PlatformIO).
2. Install board support for ESP32.
3. Install required libraries if you do not already have them:
   - `WiFi.h` (ESP32 core)
   - `HTTPClient.h` (ESP32 core)
   - `WebServer.h` (ESP32 core)
   - `ArduinoOTA.h`
   - `LittleFS.h` and `FS.h`
4. Copy `runix.ino` into your Arduino sketch folder (or open it in PlatformIO).
5. Edit Wi‑Fi credentials at the top of `runix.ino`:
   ```cpp
   const char* SSID = "your-ssid";
   const char* PASSWORD = "your-password";
   ```
   If you want a static IP, edit the `local_IP`, `gateway` and `subnet` variables. Otherwise remove or skip the `WiFi.config(...)` call.
6. Upload the sketch to your ESP32.
7. Use LittleFS uploader (Arduino IDE plugin or platform-specific tool) to upload the `/apps`, `/server`, `/configs` and web files to LittleFS.
   - Ensure `index.html`, `filemanager.html`, `logs.html`, and any app files (e.g. `/apps/sysmon.html`, `/apps/calc.html`) are present.

---

## How to use

Open a browser and visit the device IP (the program config uses `192.168.1.25` by default):

```
http://192.168.1.25/
```

Primary UI pages:
- Dashboard: `/index.html` — app launcher, quick actions.
- File Manager: `/filemanager.html` — manage files and run scripts.
- Logs: `/logs.html` — read runtime logs.
- System Monitor: `/apps/sysmon.html` — live heap, disk, tasks and CPU.

Primary API endpoints:
- `GET /list?dir=/path` — list files.
- `POST /upload?path=/path` — upload file.
- `GET /open?path=/path/file` — serve a file from LittleFS.
- `GET /runscript?script=/apps/foo.run` — create and start a task for that script.
- `GET /runcommand?command=cmd(args...)` — execute a script engine command (useful for runbg, kill, pause, resume).
- `GET /tasks` — JSON list of tasks and metadata.
- `GET /status` — JSON system status (heap, disk, uptime).

---

## Script format

Scripts are plain text files (e.g. `/apps/demo.run`). Each non-comment line is a single command:

- Comments begin with `//` and are ignored.
- Command syntax:
  ```
  name(arg1,arg2,...)
  ```
- Arguments may be quoted; numbers and unquoted identifiers are accepted.

Examples:
```
logmsg(Starting demo)
delayms(1000)
calc("5+3*2")
logmsg(Done)
```

Important: use `delayms(...)` where appropriate to yield control; without delays a script that contains only computation will run to completion in a single scheduler cycle and will appear to finish immediately.

Built-in commands (selected):
- `logmsg(text)` — append message to logs.
- `delayms(ms)` — non-blocking delay for the current task.
- `runbg("/apps/foo.run")` — start a script in the background.
- `run("/apps/foo.run")` — create a task (non-blocking).
- `kill(id)` — stop a task by id.
- `pause(id)` / `resume(id)` — pause or resume a task.
- `calc(expr)` — evaluate a simple arithmetic expression on the ESP32 and log the result.

---

## Example scripts

Place these in `/apps`:

`task1.run`
```
logmsg(Task1 Start)
delayms(1000)
logmsg(Task1 Running)
delayms(1000)
logmsg(Task1 End)
```

`heavy.run`
```
logmsg(Heavy Start)
calc("1000*2000")
delayms(200)
calc("9999*8888")
delayms(200)
logmsg(Heavy Done)
```

Run them using:
- Dashboard/File Manager: click Run (BG) — or
- API:
  ```
  GET /runcommand?command=runbg("/apps/heavy.run")
  ```

---

## System monitor and task control

The system monitor at `/apps/sysmon.html` polls `/status` and `/tasks`. Each task returned by `/tasks` contains:
- `id` — numeric task id.
- `name` — script path or descriptive name.
- `running` — boolean.
- `paused` — boolean.
- `cpu` — percent (computed over a recent window).
- `line` — current script line number.

Use the UI buttons or API `runcommand` endpoint to pause/resume/kill tasks.

---

## Troubleshooting

- If apps do not appear in the dashboard, verify that `/apps` is present in LittleFS and that `.wx` metadata lines follow the `// key: value` format.
- If CPU shows zero or task finishes instantly, add `delayms(...)` in scripts so tasks yield between heavy lines.
- If uploads fail, check the `upload` handler permissions and ensure requests include `path`.
- If Wi‑Fi does not connect, check SSID/password and optionally remove the static `WiFi.config(...)` line to use DHCP.

---

## Screenshots and final recording

Leave space below for screenshots and links to your final demonstration recording.

### Screenshots

1. Dashboard
<img width="1920" height="1080" alt="Image" src="https://github.com/user-attachments/assets/a9fd4f8c-4eb2-4b00-94eb-98aed0853d60" />

2. System monitor
  <img width="1920" height="1080" alt="Image" src="https://github.com/user-attachments/assets/fb67ee24-7996-45e6-bf65-e8c93c8f97ca" />

3. Terminal
<img width="1920" height="1080" alt="Image" src="https://github.com/user-attachments/assets/094321d3-a224-49ef-9daa-a45b57ada153" />

4. File manager
<img width="1920" height="1080" alt="Image" src="https://github.com/user-attachments/assets/1e3e831c-56e3-41f7-a445-0388a9450afa" />

---

## Notes

- This project is intended for learning and demonstration. 
