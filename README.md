# ESP32 SD File Server (PsychicHttp + TFT_eSPI)

A reference for using **PsychicHttp** with **SD card file I/O** on an ESP32, plus the full project
that uses it: a web page to upload / list / download / delete files, and a touchscreen menu that
shows the files on the SD card.

---

## Contents

1. [Libraries and hardware](#1-libraries-and-hardware)
2. [Route handler (lambda) structure](#2-route-handler-lambda-structure)
3. [Sending responses](#3-sending-responses)
4. [Upload handler (`onUpload`)](#4-upload-handler-onupload)
5. [`onRequest` for the upload handler](#5-onrequest-for-the-upload-handler)
6. [File retrieve with `inload`](#6-file-retrieve-with-inload)
7. [List and delete endpoints](#7-list-and-delete-endpoints)
8. [The whole project](#8-the-whole-project)
9. [Threading rules (important)](#9-threading-rules-important)
10. [Gotchas and fixes](#10-gotchas-and-fixes)
11. [Full source](#11-full-source)

---

## 1. Libraries and hardware

```cpp
#include <Arduino.h>
#include <PsychicHttp.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include "FS.h"
#include <ArduinoJson.h>   // v7 (uses JsonDocument)
#include <vector>
#include "WebFile.hpp"      // holds the HTML page as `page`
```

| Part | Notes |
|------|-------|
| ESP32 (classic) | Arduino framework |
| SD card | `SD_CS = 25`, shares the SPI bus with the TFT/touch |
| 320x240 TFT + XPT2046 touch | TFT_eSPI, rotation 1, touch via `tft.setTouch(calData)` |
| `WebFile.hpp` | contains the web page string `page` served at `/filePage` |

---

## 2. Route handler (lambda) structure

Every route is registered with `server.on(path, method, handler)`.
The handler is a lambda that receives a **request** and a **response** and returns `esp_err_t`
(the value returned by `resp->send(...)`).

```cpp
PsychicHttpServer server;

server.on("/hello", HTTP_GET, [](PsychicRequest* req, PsychicResponse* resp){
    // 1. read the request
    // 2. do the work
    // 3. return a response
    return resp->send("Hello");
});
```

Rules of thumb:

- Signature is always `(PsychicRequest* req, PsychicResponse* resp)`.
- **Every code path must `return` a `resp->send(...)`**. A path with no return is a bug.
- The lambda has no captures (`[]`). Use globals or `static` variables for shared state.
- Handlers run on the **server task**, not on `loop()`. See [Threading rules](#9-threading-rules-important).

### Reading the request

```cpp
req->client()->remoteIP().toString();   // caller's IP
req->bodyCStr();                         // body of a POST as a C string
req->contentLength();                    // size of the request body
req->hasParam("file");                   // query parameter present?
req->getParam("file")->value();          // query parameter value (?file=abc.txt)
```

Example, a POST that reads the body:

```cpp
server.on("/filePage/text", HTTP_POST, [](PsychicRequest* req, PsychicResponse* resp){
    Serial.println(req->bodyCStr());
    return resp->send(200);
});
```

### Server limits (set before routes)

```cpp
server.maxUploadSize      = 100 * 1024 * 1024;  // largest file accepted by the upload handler
server.maxRequestBodySize = 10  * 1024 * 1024;  // largest ordinary POST body
```

### Starting the server

Register all routes first, then call `server.begin();` at the end of `setup()`.

---

## 3. Sending responses

All of these end the handler with `return`.

```cpp
// Plain text, status 200
return resp->send("Hello");

// Status code only
return resp->send(200);

// Status + content type + body
return resp->send(404, "text/plain", "Not found");
return resp->send(200, "application/json", jsonString.c_str());

// Serve an HTML page stored as a string
return resp->send(page);

// Set the status first, then send
resp->setCode(404);
return resp->send("File not found");
```

Common status codes used in this project:

| Code | Meaning | Used for |
|------|---------|----------|
| 200 | OK | success |
| 400 | Bad request | missing / empty filename, delete failed |
| 404 | Not found | file does not exist |
| 500 | Server error | SD failed to open |

Building the body with a buffer:

```cpp
char buf[40];
snprintf(buf, sizeof(buf), "Hello from %s\n", req->client()->remoteIP().toString().c_str());
return resp->send(buf);
```

---

## 4. Upload handler (`onUpload`)

`PsychicUploadHandler` receives the file in **chunks**. The callback is called once per chunk and
you write each chunk to the SD card.

```cpp
PsychicUploadHandler* uploadHandler = new PsychicUploadHandler();

uploadHandler->onUpload([](PsychicRequest* req, const String& filename,
                           uint64_t index, uint8_t* data, size_t len, bool last){
    // ...
    return ESP_OK;        // or ESP_FAIL to abort the upload
});

server.on("/upload/*", HTTP_POST, uploadHandler);
```

### Callback arguments

| Argument | Meaning |
|----------|---------|
| `req` | the request (use `req->contentLength()` for the total size) |
| `filename` | name of the uploaded file (may arrive percent-encoded, e.g. `My%20Song.mp3`) |
| `index` | byte offset of this chunk. `0` means **first chunk** |
| `data`, `len` | the chunk and its length |
| `last` | `true` on the final chunk |

### Pattern: first chunk creates, later chunks append

```cpp
uploadHandler->onUpload([](PsychicRequest* req, const String& filename,
                           uint64_t index, uint8_t* data, size_t len, bool last){
    File file;

    String name = filename;
    cleanFilename(name);              // decode %20 etc. and replace illegal characters
    String path = "/" + name;

    if(index == 0){
        SD.remove(path);              // avoid appending to an old file with the same name
        file = SD.open(path, FILE_WRITE);
    } else {
        file = SD.open(path, FILE_APPEND);
    }

    if(!file){
        Serial.println("[Error] Failed to open file");
        return ESP_FAIL;
    }

    if(file.write(data, len) != len){
        Serial.println("[Error] Write failed");
        file.close();
        return ESP_FAIL;
    }

    file.close();                     // close every chunk (the File is local)

    if(last){
        Serial.printf("%s finished. Total bytes: %llu\n",
                      path.c_str(), (uint64_t)index + (uint64_t)len);
    }
    return ESP_OK;
});
```

Why it is written this way:

- The `File` is a **local variable**, so it is opened and closed for each chunk. That is simple and
  safe, at the cost of some speed.
- `SD.remove(path)` on the first chunk deletes any old copy. The log line
  `remove(): ... does not exists or is directory` just means there was no old file. It is harmless.
- Always close the file on every path, including errors.
- Returning `ESP_FAIL` aborts the upload.

### Filename cleaning

Browsers send names such as `01%20Sanam%20Re.mp3`. `cleanFilename()` decodes `%XX` sequences and
replaces characters that FAT does not allow (`\ / : * ? " < > |`) with `_`:

```cpp
void cleanFilename(String& in){
    size_t w = 0;
    for(size_t r = 0; r < in.length(); r++){
        char c = in[r];

        if(c == '%' && (r + 2) < in.length() && isxdigit(in[r+1]) && isxdigit(in[r+2])){
            char hex[3] = { in[r+1], in[r+2], 0 };
            c = (char)strtol(hex, nullptr, 16);
            r += 2;
        }

        if(strchr("\\/:*?\"<>|", c)) c = '_';
        in.setCharAt(w++, c);
    }
    in.remove(w);
}
```

### Showing upload progress on the TFT

Drawing from the upload callback is allowed **only if `loop()` is not touching `tft` at the same
time**. See [Threading rules](#9-threading-rules-important). Throttle the redraw so it does not
slow the transfer:

```cpp
static uint32_t lastDraw = 0;
if(millis() - lastDraw > 150 || last){
    lastDraw = millis();
    updateProgressBar(index + len, req->contentLength(), path);
}
```

---

## 5. `onRequest` for the upload handler

`onRequest` runs **once, after the whole upload finished** (after the last `onUpload` chunk).
This is where you send the final HTTP response to the browser.

```cpp
uploadHandler->onRequest([](PsychicRequest* req, PsychicResponse* resp){
    String url = "/";
    url += req->getFilename();

    String output = "<a href=\"";
    output += url;
    output += "\">";
    output += url;
    output += "</a>";

    FileIncoming = false;             // upload is over: give the touchscreen back
    return resp->send(output.c_str());
});
```

Notes:

- `onUpload` = per chunk, writes data. `onRequest` = once at the end, sends the reply.
- `req->getFilename()` returns the uploaded file name.
- If the upload fails or the browser disconnects, `onRequest` may never run. Anything you set in
  `onUpload` (like a "busy" flag) must also be cleared on the failure paths, or protected by a
  timeout. See [Gotchas](#10-gotchas-and-fixes).

---

## 6. File retrieve with `inload`

`/filePage/inload?file=<name>` streams a file from the SD card to the browser using
`PsychicFileResponse`. It handles content type, chunking and streaming for you.

```cpp
server.on("/filePage/inload", HTTP_GET, [](PsychicRequest* req, PsychicResponse* resp){
    if(req->hasParam("file")){
        String filename = "/" + req->getParam("file")->value();

        if(SD.exists(filename)){
            String mimeType = getContentType(filename);

            PsychicFileResponse fileResponse(resp, (fs::FS &)SD, filename, mimeType.c_str());
            return fileResponse.send();
        } else {
            resp->setCode(404);
            return resp->send("File not found");
        }
    }
    resp->setCode(404);
    return resp->send("File not found");
});
```

### MIME type helper

```cpp
String getContentType(const String& filename) {
    if (filename.endsWith(".html") || filename.endsWith(".htm")) return "text/html";
    if (filename.endsWith(".css"))  return "text/css";
    if (filename.endsWith(".js"))   return "application/javascript";
    if (filename.endsWith(".png"))  return "image/png";
    if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
    if (filename.endsWith(".gif"))  return "image/gif";
    if (filename.endsWith(".svg"))  return "image/svg+xml";
    if (filename.endsWith(".ico"))  return "image/x-icon";
    if (filename.endsWith(".webp")) return "image/webp";
    if (filename.endsWith(".json")) return "application/json";
    if (filename.endsWith(".txt"))  return "text/plain";
    if (filename.endsWith(".pdf"))  return "application/pdf";
    if (filename.endsWith(".mp3"))  return "audio/mpeg";
    if (filename.endsWith(".mp4"))  return "video/mp4";
    return "application/octet-stream";   // fallback: browser downloads it
}
```

Usage from a browser:

```
http://<esp-ip>/filePage/inload?file=photo.jpg
```

Tip: a file name with spaces or special characters should be URL-encoded by the page
(`encodeURIComponent(name)`).

---

## 7. List and delete endpoints

### List files as JSON, `GET /filePage/list`

```cpp
server.on("/filePage/list", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res){
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    File root = SD.open("/");
    if(!root){
        return res->send(500, "text/plain", "SD open failed");
    }

    File f = root.openNextFile();
    while(f){
        if(!f.isDirectory()){
            JsonObject o = arr.add<JsonObject>();
            o["name"] = f.name();
            o["size"] = (uint32_t)f.size();
        }
        f = root.openNextFile();
    }
    root.close();

    String out;
    serializeJson(doc, out);
    return res->send(200, "application/json", out.c_str());
});
```

Response looks like:

```json
[{"name":"one.jpg","size":19767},{"name":"INS.mp3","size":701054}]
```

### Delete a file, `POST /filePage/delete`

The page posts `msg=<filename>`. The handler extracts the name, cleans it and removes the file.

```cpp
server.on("/filePage/delete", HTTP_POST, [](PsychicRequest* req, PsychicResponse* resp){
    String filename = req->bodyCStr();

    int pos = filename.indexOf("msg=");
    if(pos != -1) filename = filename.substring(pos + 4);

    cleanFilename(filename);
    filename.trim();

    if(filename.length() == 0) return resp->send(400, "text/plain", "Empty name");

    String path = "/" + filename;
    if(!SD.exists(path)) return resp->send(404, "text/plain", "Not found");

    if(!SD.remove(path)) return resp->send(400);
    return resp->send(200, "text/plain", "Deleted");
});
```

> Note: deleting does not refresh the TFT menu by itself. Call `listDir()` + `drawMenu()` from
> `loop()` (set a flag in the handler) if you want the screen to update.

---

## 8. The whole project

### Routes

| Route | Method | Purpose |
|-------|--------|---------|
| `/` | GET | hello world, shows caller IP |
| `/filePage` | GET | serves the HTML page (`page` from `WebFile.hpp`) |
| `/filePage/text` | POST | prints the POST body to Serial (test endpoint) |
| `/filePage/list` | GET | JSON list of files on SD |
| `/filePage/inload?file=NAME` | GET | download / stream a file |
| `/filePage/delete` | POST | delete a file (`msg=NAME`) |
| `/upload/*` | POST | upload a file (upload handler) |

### Startup order in `setup()`

1. `Serial.begin`, `tft.begin()`, `tft.setRotation(1)`, `tft.setTouch(calData)`.
2. Connect Wi-Fi (STA mode) and print the IP.
3. `SD.begin(SD_CS)`. Stop if it fails.
4. `listDir(SD, "/", 0)` to fill `fileList`.
5. Compute `visibleRow = (240 - HEADER_H) / ROW_H`, then `drawMenu()`.
6. Set `server.maxUploadSize` / `maxRequestBodySize`.
7. Register all routes and the upload handler.
8. `server.begin()`.

### TFT menu

- Header shows `FILES: <selected>/<total>`.
- Rows show file names; the selected row has a green background.
- Touch zones (screen is 320x240, rotation 1):
  - top area: **move selection down**
  - middle area: **OK** (currently only prints to Serial)
  - bottom area: **move selection up**
- `drawRowUpdate()` redraws only the two changed rows. `drawMenu()` redraws everything (used when
  scrolling).

### After an upload finishes

The list is rebuilt with `listDir()`, and the selection stays where it was (it is only moved to the
new file if you search for it by name):

```cpp
listDir(SD, "/", 0);

// optional: select the file that was just uploaded
for(int i = 0; i < (int)fileList.size(); i++){
    if(fileList[i] == name){ sel = i; break; }
}

if(sel < topIdx) topIdx = sel;
if(sel >= topIdx + visibleRow) topIdx = sel - visibleRow + 1;
if(topIdx < 0) topIdx = 0;

drawMenu();
```

---

## 9. Threading rules (important)

PsychicHttp runs your lambdas and upload callbacks on its **own server task**. Arduino's `loop()`
runs on a **different task**. TFT_eSPI, and the SD library, are **not thread-safe**.

What went wrong in this project:

- The upload callback drew the progress bar (`tft.fillRect`, `tft.print`) on the server task.
- At the same time `loop()` called `tft.getTouch()`.
- Both use the same SPI lock. One task released a lock the other one held, which triggered:

```
assert failed: xQueueGenericSend queue.c:832 (... xMutexHolder == xTaskGetCurrentTaskHandle())
```

Removing either the progress bar or the touch code made the crash disappear, which confirmed it.

### The fix used here: pause touch during an upload

```cpp
volatile bool     FileIncoming = false;   // global
volatile uint32_t lastChunkMs  = 0;       // global
```

Upload callback:

```cpp
lastChunkMs = millis();
if(index == 0) FileIncoming = true;
// on every failure path:  FileIncoming = false;  return ESP_FAIL;
```

`onRequest`:

```cpp
FileIncoming = false;
```

`loop()`:

```cpp
if(FileIncoming && millis() - lastChunkMs > 5000) FileIncoming = false;  // upload died

if(!FileIncoming){
    // safe to use tft.getTouch() and draw
}
```

### Stronger options

- **Mutex around every `tft` call** (in `loop()` and in the upload callback):

  ```cpp
  SemaphoreHandle_t tftMutex;                 // global
  tftMutex = xSemaphoreCreateMutex();         // in setup(), before server.begin()

  xSemaphoreTake(tftMutex, portMAX_DELAY);
  /* tft calls */
  xSemaphoreGive(tftMutex);
  ```

- **Only `loop()` draws.** The upload callback only sets variables (`upWritten`, `upTotal`,
  flags). `loop()` reads them and draws. This is the cleanest design, and it also keeps `listDir()`
  and `fileList` on one task.

- **Separate SPI bus for the SD card** (speed, not a fix for this crash):

  ```cpp
  SPIClass sdSPI(HSPI);
  sdSPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  SD.begin(SD_CS, sdSPI, 25000000);
  ```

  A separate bus for the SD card does **not** fix the TFT/touch collision, because
  `tft.getTouch()` runs on the TFT's own bus.

- If SD is used from several server tasks at once (upload + list + inload), protect it with a
  shared mutex too.

---

## 10. Gotchas and fixes

| Problem | Cause | Fix |
|---------|-------|-----|
| Crash with `xQueueGenericSend` assert after a few upload packets | TFT drawn from the server task while `loop()` reads touch | Flag, mutex, or draw only from `loop()` ([section 9](#9-threading-rules-important)) |
| Touchscreen dead after a failed upload | busy flag only cleared in `onRequest` | clear on failure paths and add a no-chunk timeout |
| Uploads are slow | progress bar redrawn on every chunk | throttle to every ~150 ms |
| Leftover characters on the progress text (`10%` then `9%`) | text drawn with a background color but shorter | pad the format (`%3d%%`) or clear the line first |
| `remove(): ... does not exists or is directory` in the log | `SD.remove()` on a new file | harmless, ignore |
| File names contain `%20` | browser URL-encodes names | decode with `cleanFilename()` |
| Selection jumps to the last item after upload | code sets `sel = fileList.size() - 1` | remove it, or search the list for the new file by name |
| `else { topIdx = 0; }` attached to the wrong `if` | missing braces on `if(!fileList.empty())` | always use braces |
| `fileList` changed while the UI draws | `listDir()` called from the server task | set a flag and call `listDir()` from `loop()` |
| Wi-Fi password in the source | hard-coded credentials | keep them in a git-ignored `secrets.h` |

### Keep credentials out of the repository

```cpp
// secrets.h  (add to .gitignore)
const char* ssid = "YOUR_SSID";
const char* pass = "YOUR_PASSWORD";
```

```cpp
#include "secrets.h"
```

---

## 11. Full source

Credentials are replaced with placeholders. Changes from the original are marked
`// CHANGED`.

```cpp
#include <Arduino.h>
#include <PsychicHttp.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include "FS.h"
#include <ArduinoJson.h>
#include <vector>
#include "WebFile.hpp"

#define SD_CS 25

const char* ssid = "YOUR_SSID";
const char* pass = "YOUR_PASSWORD";

PsychicHttpServer server;
PsychicUploadHandler* uploadHandler;

TFT_eSPI tft = TFT_eSPI();

std::vector<String> fileList;

const int HEADER_H = 28;
const int ROW_H    = 24;
const int FONT     = 2;

volatile bool     FileIncoming = false;
volatile uint32_t lastChunkMs  = 0;       // CHANGED: upload watchdog

int visibleRow = 0;
int sel = 0;
int topIdx = 0;
uint16_t calData[5] = { 445, 3375, 383, 3250, 7 };

// ---------------------------------------------------------------- SD helpers

void listDir(fs::FS &fs, const char* dirname, uint8_t levels){
  Serial.printf("Listing directory: %s\n", dirname);
  fileList.clear();
  File root = fs.open(dirname);
  if(!root){ Serial.println("Failed to open directory"); return; }

  File file = root.openNextFile();
  while(file){
    if(file.isDirectory()){
      Serial.printf(" DIR: %s\n", file.name());
      if(levels) listDir(fs, file.path(), levels - 1);
    } else {
      Serial.printf(" FILE: %s SIZE: %ld\n", file.name(), file.size());
      fileList.push_back(String(file.name()));
    }
    file = root.openNextFile();
  }
  if(sel >= (int)fileList.size()) sel = max(0, (int)fileList.size() - 1);
}

String getContentType(const String& filename) {
  if (filename.endsWith(".html") || filename.endsWith(".htm")) return "text/html";
  if (filename.endsWith(".css"))  return "text/css";
  if (filename.endsWith(".js"))   return "application/javascript";
  if (filename.endsWith(".png"))  return "image/png";
  if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
  if (filename.endsWith(".gif"))  return "image/gif";
  if (filename.endsWith(".svg"))  return "image/svg+xml";
  if (filename.endsWith(".ico"))  return "image/x-icon";
  if (filename.endsWith(".webp")) return "image/webp";
  if (filename.endsWith(".json")) return "application/json";
  if (filename.endsWith(".txt"))  return "text/plain";
  if (filename.endsWith(".pdf"))  return "application/pdf";
  if (filename.endsWith(".mp3"))  return "audio/mpeg";
  if (filename.endsWith(".mp4"))  return "video/mp4";
  return "application/octet-stream";
}

bool deleteFile(fs::FS &fs, const char *path){
  Serial.printf("Deleting File: %s\n", path);
  if(fs.remove(path)){ Serial.println("[SUCCESS] File Deleted"); return true; }
  Serial.println("[FAIL] Delete Operation Failed");
  return false;
}

void cleanFilename(String& in){
  size_t w = 0;
  for(size_t r = 0; r < in.length(); r++){
    char c = in[r];
    if(c == '%' && (r + 2) < in.length() && isxdigit(in[r+1]) && isxdigit(in[r+2])){
      char hex[3] = { in[r+1], in[r+2], 0 };
      c = (char)strtol(hex, nullptr, 16);
      r += 2;
    }
    if(strchr("\\/:*?\"<>|", c)) c = '_';
    in.setCharAt(w++, c);
  }
  in.remove(w);
}

// ---------------------------------------------------------------- TFT drawing

void drawHeader(){
  tft.fillRect(0, 0, 320, HEADER_H, TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  char buf[32];
  snprintf(buf, sizeof(buf), "FILES: %d/%d", fileList.empty() ? 0 : sel + 1, (int)fileList.size());
  tft.drawString(buf, 6, HEADER_H / 2, FONT);
}

void drawRow(int slot){
  int y = HEADER_H + slot * ROW_H;
  int fileIdx = topIdx + slot;
  uint16_t BG = (sel == fileIdx) ? TFT_GREEN : TFT_BLACK;

  tft.fillRect(0, y, 320 - 6, ROW_H, BG);
  tft.setTextColor(TFT_WHITE, BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(fileList[fileIdx], 6, y + ROW_H / 2, FONT);
}

void drawRowUpdate(int prev, int curr){
  drawRow(prev);
  drawRow(curr);
}

void drawMenu(){
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  int rows = min(visibleRow, (int)fileList.size() - topIdx);
  if(rows < 0) rows = 0;
  for(int s = 0; s < rows; s++) drawRow(s);
}

void drawDownLoadProgress(size_t total){
  tft.fillRect(0, 0, 320, 80, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 10);
  tft.print("Downloading:");
  if(total > 0) tft.drawRect(10, 65, 300, 10, TFT_WHITE);
}

void updateProgressBar(size_t written, size_t total, const String& path){
  tft.setCursor(10, 30);
  tft.print(path);
  tft.setCursor(10, 50);

  if(total > 0){
    int percent = (written * 100) / total;
    tft.printf("%3d%%  %u / %u bytes", percent, (unsigned)written, (unsigned)total);

    int barWidth = (percent * 298) / 100;
    if(barWidth > 0) tft.fillRect(11, 66, barWidth, 8, TFT_GREEN);
  }
}

// ---------------------------------------------------------------- setup

void setup() {
  Serial.begin(115200);
  tft.begin();
  tft.setRotation(1);
  tft.setTouch(calData);
  tft.fillScreen(TFT_BLACK);

  Serial.println("WiFi Connecting ");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  while(WiFi.status() != WL_CONNECTED){ Serial.print("."); delay(500); }
  Serial.printf("Connected! Addr: http://%s\n", WiFi.localIP().toString().c_str());

  if(!SD.begin(SD_CS)){
    Serial.println("[Error] SD Card not found");
    return;
  }

  listDir(SD, "/", 0);
  Serial.println("[SUCCESS] SD card connected");

  visibleRow = (240 - HEADER_H) / ROW_H;
  drawMenu();

  server.maxUploadSize      = 100 * 1024 * 1024;
  server.maxRequestBodySize = 10 * 1024 * 1024;

  server.on("/", HTTP_GET, [](PsychicRequest* req, PsychicResponse* resp){
    char buf[40];
    snprintf(buf, sizeof(buf), "Hello from %s\n", req->client()->remoteIP().toString().c_str());
    return resp->send(buf);
  });

  server.on("/filePage", HTTP_GET, [](PsychicRequest* req, PsychicResponse* resp){
    return resp->send(page);
  });

  server.on("/filePage/text", HTTP_POST, [](PsychicRequest* req, PsychicResponse* resp){
    Serial.println(req->bodyCStr());
    return resp->send(200);
  });

  server.on("/filePage/delete", HTTP_POST, [](PsychicRequest* req, PsychicResponse* resp){
    String filename = req->bodyCStr();
    int pos = filename.indexOf("msg=");
    if(pos != -1) filename = filename.substring(pos + 4);

    cleanFilename(filename);
    filename.trim();
    if(filename.length() == 0) return resp->send(400, "text/plain", "Empty name");

    String path = "/" + filename;
    if(!SD.exists(path)) return resp->send(404, "text/plain", "Not found");
    if(!deleteFile(SD, path.c_str())) return resp->send(400);
    return resp->send(200, "text/plain", "Deleted");
  });

  // ------------------------------------------------------------ upload
  uploadHandler = new PsychicUploadHandler();

  uploadHandler->onUpload([](PsychicRequest* req, const String& filename,
                             uint64_t index, uint8_t* data, size_t len, bool last){
    File file;
    String name = filename;
    cleanFilename(name);
    String path = "/" + name;

    lastChunkMs = millis();                                   // CHANGED

    if(index == 0){
      SD.remove(path);
      FileIncoming = true;
      file = SD.open(path, FILE_WRITE);
      drawDownLoadProgress(req->contentLength());
    } else {
      file = SD.open(path, FILE_APPEND);
    }

    if(!file){
      Serial.println("[Error] Failed to open file");
      FileIncoming = false;                                   // CHANGED
      return ESP_FAIL;
    }

    if(file.write(data, len) != len){
      Serial.println("[Error] Write failed");
      file.close();
      FileIncoming = false;                                   // CHANGED
      return ESP_FAIL;
    }
    file.close();

    // CHANGED: throttled progress redraw
    static uint32_t lastDraw = 0;
    if(millis() - lastDraw > 150 || last){
      lastDraw = millis();
      updateProgressBar(index + len, req->contentLength(), path);
    }

    if(last){
      Serial.printf("%s finished. Total bytes: %llu\n", path.c_str(), (uint64_t)index + (uint64_t)len);

      listDir(SD, "/", 0);

      // CHANGED: select the uploaded file, keep it visible
      for(int i = 0; i < (int)fileList.size(); i++){
        if(fileList[i] == name){ sel = i; break; }
      }
      if(sel < topIdx) topIdx = sel;
      if(sel >= topIdx + visibleRow) topIdx = sel - visibleRow + 1;
      if(topIdx < 0) topIdx = 0;

      drawMenu();
    }
    return ESP_OK;
  });

  uploadHandler->onRequest([](PsychicRequest* req, PsychicResponse* resp){
    String url = "/";
    url += req->getFilename();

    String output = "<a href=\"";
    output += url;
    output += "\">";
    output += url;
    output += "</a>";

    FileIncoming = false;
    return resp->send(output.c_str());
  });

  server.on("/upload/*", HTTP_POST, uploadHandler);

  // ------------------------------------------------------------ inload
  server.on("/filePage/inload", HTTP_GET, [](PsychicRequest* req, PsychicResponse* resp){
    if(req->hasParam("file")){
      String filename = "/" + req->getParam("file")->value();
      if(SD.exists(filename)){
        String mimeType = getContentType(filename);
        PsychicFileResponse fileResponse(resp, (fs::FS &)SD, filename, mimeType.c_str());
        return fileResponse.send();
      }
      resp->setCode(404);
      return resp->send("File not found");
    }
    resp->setCode(404);
    return resp->send("File not found");
  });

  // ------------------------------------------------------------ list
  server.on("/filePage/list", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res){
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    File root = SD.open("/");
    if(!root) return res->send(500, "text/plain", "SD open failed");

    File f = root.openNextFile();
    while(f){
      if(!f.isDirectory()){
        JsonObject o = arr.add<JsonObject>();
        o["name"] = f.name();
        o["size"] = (uint32_t)f.size();
      }
      f = root.openNextFile();
    }
    root.close();

    String out;
    serializeJson(doc, out);
    return res->send(200, "application/json", out.c_str());
  });

  server.begin();
}

// ---------------------------------------------------------------- touch UI

uint16_t DWX = 0, DWY = 0,   DWW = 320, DWH = 75;
uint16_t OKX = 0, OKY = 80,  OKW = 320, OKH = 155;
uint16_t UPX = 0, UPY = 160, UPW = 320, UPH = 235;

void moveSelDown(){
  if(fileList.empty()) return;
  int oldSel = sel;
  if(sel < (int)fileList.size() - 1) sel++;

  if(sel >= topIdx + visibleRow){
    topIdx++;
    drawMenu();
  } else {
    drawRowUpdate(oldSel - topIdx, sel - topIdx);
  }
  drawHeader();
}

void moveSelUp(){
  if(fileList.empty()) return;
  int oldSel = sel;
  if(sel > 0) sel--;

  if(sel < topIdx){
    topIdx--;
    if(topIdx < 0) topIdx = 0;
    drawMenu();
  } else {
    drawRowUpdate(oldSel - topIdx, sel - topIdx);
  }
  drawHeader();
}

void loop() {
  // CHANGED: if the upload died, give the touchscreen back
  if(FileIncoming && millis() - lastChunkMs > 5000) FileIncoming = false;

  uint16_t x, y;
  if(!FileIncoming){
    if(tft.getTouch(&x, &y)){
      if(x > DWX && x < DWW && y > DWY && y < DWH){ Serial.println("DW"); moveSelDown(); }
      if(x > OKX && x < OKW && y > OKY && y < OKH){ Serial.println("OK"); }
      if(x > UPX && x < UPW && y > UPY && y < UPH){ Serial.println("UP"); moveSelUp(); }
    }
  }
  delay(10);
}
```

### Known remaining limitations

- The touch pause is a flag, not a lock, so a tiny race window exists when an upload starts exactly
  while `loop()` is inside `getTouch()`. Use the mutex or "only `loop()` draws" approach to remove it.
- Delete does not refresh the TFT menu.
- Only the SD root folder is listed (`levels = 0`).
- Touch zones overlap slightly (the middle zone ends at y=155, the bottom starts at y=160, and the
  top ends at y=75), so the gaps are dead zones. Adjust to taste.