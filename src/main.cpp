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

const char* ssid = "";
const char* pass = "";

PsychicHttpServer server;
//PsychicWebSocketHandler webSocketHandler;
PsychicUploadHandler* uploadHandler; 

char tftBuf[50];

TFT_eSPI tft = TFT_eSPI();

std::vector<String> fileList;

const int HEADER_H = 28;
const int ROW_H    = 24;
const int FONT     = 2;
volatile bool FileIncoming = false;
int visibleRow = 0;
int sel = 0;
int topIdx = 0;
volatile bool listUpdate = false;
uint16_t calData[5] = { 445, 3375, 383, 3250, 7 };



void listDir(fs::FS &fs, const char* dirname, uint8_t levels){
  Serial.printf("Listing director: %s\n", dirname);
  fileList.clear();
  File root = fs.open(dirname);

  if(!root){
    Serial.println("Failed to open directory");
    return;
  }

  File file = root.openNextFile();

  while(file){
    if(file.isDirectory()){
      Serial.printf(" DIR: %s\n",file.name());

      if(levels){
        listDir(fs, file.path(), levels-1);
      }
    }else{
      Serial.printf(" FILE: %s SIZE: %ld\n", file.name(), file.size());
      fileList.push_back(String(file.name()));
    }

    file = root.openNextFile();
  }

  if (sel >=  (int)fileList.size()) sel = max(0, (int)fileList.size()-1);
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
  return "application/octet-stream"; // fallback
}


bool deleteFile(fs::FS &fs, const char *path){

  Serial.printf("Deleting File: %s\n", path);
  if(fs.remove(path)){
    Serial.println("[SUCCESS] File Deleted");
    return true;
  }else{
    Serial.println("[FAIL] Delete Operation Failed ");
    return false;
  }

}

void cleanFilename(String& in){
 
  size_t w = 0;
  for(size_t r = 0; r < in.length(); r++){
    char c = in[r];

    if(c == '%' && (r + 2) < in.length() && isxdigit(in[r+1]) && isxdigit(in[r+2])){

    char hex[3] = {in[r+1],in[r+2],0};
    c = (char)strtol(hex, nullptr, 16);
    r+=2;
  }

  if(strchr("\\/:*?\"<>|",c)) c = '_';
  in.setCharAt(w++, c);  
  }

  in.remove(w);
}


void drawHeader(){
  tft.fillRect(0,0,320,HEADER_H,TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);

  char buf[32];
  snprintf(buf, sizeof(buf), "FILES: %d/%d", fileList.empty() ? 0 : sel+1, (int)fileList.size());
  tft.drawString(buf,6, HEADER_H/2, FONT);
}

void drawRow(int slot){
 int y = HEADER_H + slot*ROW_H;
 int x = 0;
 int w = 320 - 6;
 int fileIdx = topIdx + slot;

 uint16_t BG = sel == fileIdx ? TFT_GREEN : TFT_BLACK;

 tft.fillRect(x,y,w,ROW_H,BG);
 tft.setTextColor(TFT_WHITE, BG);
 tft.setTextDatum(ML_DATUM);
 tft.drawString(fileList[fileIdx],6,y+ROW_H/2, FONT);
}

void drawRowUpdate(int prev, int curr){
 int prevY = HEADER_H + prev*ROW_H;
 int prevX = 0;
 int prevW = 320 - 6;

 int currY = HEADER_H + curr*ROW_H;
 int currX = 0;
 int currW = 320 - 6;

 int fileIdxCurr = topIdx + curr;
 int fileIdxPrev = topIdx + prev;

 uint16_t BGCurr = sel == fileIdxCurr ? TFT_GREEN : TFT_BLACK;
 uint16_t BGPrev = sel == fileIdxPrev ? TFT_GREEN : TFT_BLACK;

 tft.fillRect(prevX,prevY,prevW,ROW_H,BGPrev);
 tft.setTextColor(TFT_WHITE, BGPrev);
 tft.setTextDatum(ML_DATUM);
 tft.drawString(fileList[fileIdxPrev],6,prevY+ROW_H/2, FONT);

 tft.fillRect(currX,currY,currW,ROW_H,BGCurr);
 tft.setTextColor(TFT_WHITE, BGCurr);
 tft.setTextDatum(ML_DATUM);
 tft.drawString(fileList[fileIdxCurr],6,currY+ROW_H/2, FONT);
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

    if(total > 0){
              tft.drawRect(10, 65, 300, 10, TFT_WHITE);
    }

    
}

void updateProgressBar(size_t written, size_t total, const String& path){

    tft.setCursor(10, 30);
    tft.print(path);

    tft.setCursor(10, 50);

    if (total > 0)
    {
        int percent = (written * 100) / total;

        tft.printf("%d%%  %u / %u bytes",
                   percent,
                   (unsigned)written,
                   (unsigned)total);

        // Progress bar

        int barWidth = (percent * 298) / 100;

        if (barWidth > 0)
            tft.fillRect(11, 66, barWidth, 8, TFT_GREEN);
    }

}

void setup() {
  Serial.begin(115200);
  tft.begin();
  tft.setRotation(1);
  tft.setTouch(calData);
  tft.fillScreen(TFT_BLACK);
  
  Serial.println("WiFi Connecting ");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,pass);

  while(WiFi.status() != WL_CONNECTED){
    Serial.print(".");
    delay(500);
  }

  Serial.printf("Connected!. Addr: http://%s",WiFi.localIP().toString().c_str());

  //SPI.begin();

  if(!SD.begin(SD_CS)){
    Serial.println("[Error] SD Card not found");
    return;
  }

  listDir(SD,"/",0);


  Serial.println("[SUCCESS] Sd card connected");
  Serial.println(ESP.getFreeHeap());

  visibleRow = (240 - HEADER_H) / ROW_H;
  drawMenu();
  server.maxUploadSize = 100*1024*1024;
  server.maxRequestBodySize = 10 * 1024 * 1024;
  Serial.println(ESP.getFreeHeap());

  // Hello world Page serving

  server.on("/",HTTP_GET, [](PsychicRequest *req, PsychicResponse* resp){
    char buf[40];
    snprintf(buf, sizeof(buf),"Hello Arman from %s\n",req->client()->remoteIP().toString().c_str());
    return resp->send(buf);
  });

  server.on("/filePage", HTTP_GET, [](PsychicRequest *req, PsychicResponse* resp){
    return resp->send(page);
  });

  server.on("/filePage/text", HTTP_POST, [](PsychicRequest* req, PsychicResponse* resp){
    Serial.println(req->bodyCStr());
    return resp->send(200);
  });

   server.on("/filePage/delete", HTTP_POST, [](PsychicRequest* req, PsychicResponse* resp){
    File file;
    String filename;
    filename = req->bodyCStr();
    Serial.println(filename);
    if(filename.indexOf("msg=") != -1){
      int pos = filename.indexOf("msg=");

      if(pos != -1){
        filename = filename.substring(pos+4);
      }else{
        return resp->send(400, "text/plain", "Missing msg");
      }
    }
    cleanFilename(filename);
    filename.trim();

    if(filename.length() == 0) return resp->send(400, "text/plain", "Empty name");

    Serial.println(filename);
    String path = "/"+filename;
    
    if(!SD.exists(path)) return resp->send(404, "text/plain", "Not found");

    if(deleteFile(SD,path.c_str()) == false){
      return resp->send(400);
    }
    return resp->send(200, "text/plain", "Deleted");
  });


  uploadHandler = new PsychicUploadHandler();

  // Added FILE_APPEND AGAIN <- Failed
  // Remove Static in File file

  uploadHandler->onUpload([](PsychicRequest* req, const String& filename, uint64_t index, uint8_t* data, size_t len, bool last){
  
    File file;

    String name = filename;
    cleanFilename(name);
    String path = "/" + name;
    ///path.replace("%20"," "); // Remove that %20 which is coming due to network transmission
    
    if(index == 0){
      Serial.println(filename);
    //  if(file) file.close();
      SD.remove(path); // To prevent overwriting of old file
      FileIncoming = true;
      file = SD.open(path, FILE_WRITE);
      drawDownLoadProgress(req->contentLength());
      
      if(!file){Serial.println("[Error] Failed to open file"); return ESP_FAIL;}
    }
     else{
       file = SD.open(path, FILE_APPEND);
     }

    if(!file){
      Serial.println("[Error] Failed to open file");
      return ESP_FAIL;
    }

    if(file.write(data,len) != len){
      Serial.println("[Error] Write failed");
      file.close();
      return ESP_FAIL;
    }

    size_t written = index + len;
    // char buf[100];
    Serial.printf("Writing %d/%d bytes to: %s\n", (int)index+(int)len, req->contentLength(),path.c_str());
    // snprintf(buf, sizeof(buf),"Writing %d/%d bytes to\n %s", (int)index+(int)len, req->contentLength(),path.c_str());
    // tft.setCursor(30,30);
    // tft.print(buf);
    updateProgressBar(written,req->contentLength(), path);
    
    if(last){
      Serial.printf("%s is finished. Total bytes: %llu\n", path.c_str(), (uint64_t)index+(uint64_t)len);
      file.close();

      // Rebuilding file list
      listDir(SD,"/",0);
      
      if(sel < topIdx) topIdx = sel;
      if(sel >= topIdx + visibleRow) topIdx = sel - visibleRow + 1;
      if(topIdx < 0) topIdx = 0;
      
      drawMenu();
    }

    return ESP_OK;
  });

  uploadHandler->onRequest([](PsychicRequest* req, PsychicResponse* resp){
    //listDir(SD,"/",0);

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


  server.on("/filePage/inload", HTTP_GET, [](PsychicRequest* req, PsychicResponse* resp){
  if(req->hasParam("file")){
    String filename = "/" + req->getParam("file")->value();
    Serial.println(filename);

    if(SD.exists(filename)){
      // PsychicFileResponse handles everything: content-type, chunking, streaming
      String mimeType = getContentType(filename);
      Serial.println("MIME: " + mimeType);
      
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

server.on("/filePage/list", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res){
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>(); // Creating a json array where we will put JSON object

  File root = SD.open("/");

  if(!root){
    return res->send(500, "text/plain","SD open failed");
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
  serializeJson(doc,out);

  return res->send(200, "application/json", out.c_str());

});


  server.begin();


}

uint16_t DWX = 0, DWY = 0, DWW = 320, DWH = 75;

uint16_t OKX = 0, OKY = 80, OKW = 320, OKH = 155;

uint16_t UPX = 0, UPY = 160, UPW = 320, UPH = 235;


void moveSelDown(){

  if(fileList.empty()){return;}

  int oldSel = sel;

  if(sel < (int)fileList.size() - 1){
    sel++;
  }

  if(sel >= topIdx + visibleRow){
    topIdx++;
    drawMenu();
  }else{
    int oldSlot = oldSel - topIdx;
    int newSlot = sel - topIdx;

    drawRowUpdate(oldSlot, newSlot);
  }

  drawHeader();
}

void moveSelUp(){

  if(fileList.empty()){return;}

  int oldSel = sel;

  if(sel > 0){
    sel--;
  }

  if(sel < topIdx){
    topIdx--;

    if(topIdx < 0){topIdx = 0;}
    drawMenu();
  }else{
    int oldSlot = oldSel - topIdx;
    int newSlot = sel - topIdx;

    drawRowUpdate(oldSlot, newSlot);
  }

  drawHeader();
}




void loop() {

  uint16_t x,y;

  if(FileIncoming == false){
    if(tft.getTouch(&x,&y)){

      if(x > DWX && x < DWW && y > DWY && y < DWH){
        Serial.println("DW");
        moveSelDown();
      }

      if(x > OKX && x < OKW && y > OKY && y < OKH){
        Serial.println("OK");
        //topIdx++;

      }

      if(x > UPX && x < UPW && y > UPY && y < UPH){
        Serial.println("UP");
        moveSelUp();
        

      }
    }
}
  delay(10);

}

