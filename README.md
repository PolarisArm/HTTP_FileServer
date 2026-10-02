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