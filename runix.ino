#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <FS.h>

#include <map>
#include <vector>
#include <algorithm>
#include <functional>

// ——— Configuration ———
const char* SSID       = "SSID";
const char* PASSWORD   = "PASSWORD";

// Static IP
IPAddress local_IP(192,168,1,25), gateway(192,168,1,1), subnet(255,255,255,0);

// DNS
IPAddress primaryDNS(8,8,8,8), secondaryDNS(8,8,4,4);

#define AUTOSTART_CONFIG "/configs/autostart.txt"
#define ENDPOINTS_TABLE  "/server/endpoints.csv"

WiFiClient   httpClient;
WebServer    server(80);

// ——— Scripting engine types ———
typedef std::vector<String> ArgList;
typedef std::function<void(const ArgList&)> CmdFn;
std::map<String, CmdFn> commands;
unsigned long lastCpuCalc = 0;
// removed volatile scriptRunning as we replaced blocking runScript with task creation
// volatile bool scriptRunning = false;

// ——— Task System ———
struct Task {
  int id;
  String name;
  std::vector<String> lines;
  int currentLine;

  bool running;
  bool paused;     // 🔥 NEW
  bool background;

  unsigned long delayUntil;
  unsigned long cpuTime;
  unsigned long lastCpuTime;
float cpuPercent;
};

std::vector<Task> tasks;
int nextTaskId = 1;
Task* currentTask = nullptr;
unsigned long totalCpuTime = 0; // microseconds tracked globally

// ——— Forward declarations ———
void connectWiFi();
void logMsg(const char* msg);

String getContentType(const String& path);
bool parseLine(const String& line, String& name, ArgList& args);
void initScriptEngine();
void runScript(const String& path); // kept signature for compatibility (now creates a task)

// HTTP handlers
void handleRoot();
void handleList();
void handleDelete();
void handleUpload();
void handleDownload();
void handleMkdir();
void handleRename();
void handleNotFound();
void handleRunScript();
void handleAutoStartAdd();
void handleAutoStartRemove();
void handleGetAutoStart();
void loadAutoStart();
void handleFileManagerPage();
void handleLogsPage();
void handleLogs();
void handleRunCommand();
void handleListApps();
void handleRestart();
void handleClearLogs();
void handleTasks();
void handleStatus();

// AutoStart helpers
std::vector<String> readAutoStartList();
bool writeAutoStartList(const std::vector<String>& L);

// ——— Helpers ———
bool removeRecursive(const String & path) {
  File entry = LittleFS.open(path, "r");
  if (!entry) return false;
  if (entry.isDirectory()) {
    File child;
    while ((child = entry.openNextFile())) {
      String childPath;
      // openNextFile returns name relative to the directory opened; ensure childPath resolves properly
      if (String(child.name()).startsWith("/")) childPath = String(child.name());
      else childPath = path + "/" + String(child.name());
      removeRecursive(childPath);
      child.close();
    }
    entry.close();
    return LittleFS.rmdir(path);
  } else {
    entry.close();
    return LittleFS.remove(path);
  }
}

void appendEndpoint(const String& uri, const String& target) {
  LittleFS.mkdir("/server");
  File f = LittleFS.open(ENDPOINTS_TABLE, "a");
  if (!f) return;
  f.printf("%s,%s\n", uri.c_str(), target.c_str());
  f.close();
}

void removeEndpoint(const String& uri) {
  if (!LittleFS.exists(ENDPOINTS_TABLE)) return;
  File f = LittleFS.open(ENDPOINTS_TABLE, "r");
  std::vector<String> lines;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    if (!l.startsWith(uri + ",")) lines.push_back(l);
  }
  f.close();
  File fw = LittleFS.open(ENDPOINTS_TABLE, "w");
  for (auto &l: lines) fw.println(l);
  fw.close();
}

// ——— Task subsystem utilities ———
std::vector<String> loadScript(const String& path) {
  std::vector<String> lines;
  if (!LittleFS.exists(path)) return lines;
  File f = LittleFS.open(path, "r");
  if (!f) return lines;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) lines.push_back(line);
  }
  f.close();
  return lines;
}
void updateCpuUsage() {

  if (millis() - lastCpuCalc < 1000) return;

  lastCpuCalc = millis();

  unsigned long totalDelta = 0;

  // calculate total delta
  for (auto &t : tasks) {
    unsigned long delta = t.cpuTime - t.lastCpuTime;
    totalDelta += delta;
  }

  // assign %
  for (auto &t : tasks) {
    unsigned long delta = t.cpuTime - t.lastCpuTime;

    if (totalDelta > 0) {
      t.cpuPercent = (delta * 100.0) / totalDelta;
    } else {
      t.cpuPercent = 0;
    }

    t.lastCpuTime = t.cpuTime;
  }
}
void createTask(const String& path, bool background) {
  auto lines = loadScript(path);
 
  if (lines.empty()) {
    logMsg(("Script not found or empty: " + path).c_str());
    return;
  }

  Task t;
  t.id = nextTaskId++;
  t.name = path;
  t.lines = std::move(lines);
  t.currentLine = 0;
  t.running = true;
  t.background = background;
  t.delayUntil = 0;
  t.cpuTime = 0;
   t.paused = false;

  tasks.push_back(std::move(t));
  logMsg(("Task created: " + path).c_str());
}

// parseLine is already available; use it in executeLine
void executeLine(Task &t, String line) {

  currentTask = &t;

  // 🔥 START TIMER
  unsigned long startMicros = micros();

  // ===== YOUR EXISTING PARSER =====
  int p1 = line.indexOf("(");
  int p2 = line.lastIndexOf(")");

  if (p1 == -1 || p2 == -1) return;

  String cmd = line.substring(0, p1);
  String argsStr = line.substring(p1 + 1, p2);

  std::vector<String> args;
  int last = 0;
  for (int i = 0; i <= argsStr.length(); i++) {
    if (i == argsStr.length() || argsStr[i] == ',') {
      String a = argsStr.substring(last, i);
      a.trim();
      a.replace("\"", "");
      args.push_back(a);
      last = i + 1;
    }
  }

  if (commands.count(cmd)) {
    commands[cmd](args);
  }

  // 🔥 END TIMER (ONLY ONCE)
  unsigned long execTime = micros() - startMicros;
  t.cpuTime += execTime;

  currentTask = nullptr;
}

void schedulerLoop() {
  // cooperative scheduler: iterate tasks and execute single line per loop per runnable task
  for (auto &t : tasks) {
    if (!t.running || t.paused) continue;

    // respect delayUntil (non-blocking delay)
    if (millis() < t.delayUntil) continue;

    if (t.currentLine >= (int)t.lines.size()) {
      // finished
      t.running = false;
      logMsg(("Task finished: " + t.name).c_str());
      continue;
    }

    // execute single line
    executeLine(t, t.lines[t.currentLine]);
    t.currentLine++;

    // if task is foreground (background == false), we can choose to run it to completion.
    // But to keep UI responsive and support multiple tasks, we execute one line per tick for all tasks.
  }
}

// call this instead of original blocking runScript
void runScript(const String& path) {
  // by default start as background to avoid blocking server
  createTask(path, true);
}

// ——— Setup ———
void setup() {
  Serial.begin(115200);
  delay(500);

  if (!LittleFS.begin()) {
    Serial.println("❌ LittleFS Mount Failed");
    return;
  }
  Serial.println("✅ LittleFS Mounted");

  connectWiFi();

  ArduinoOTA.onStart([]() { logMsg("OTA Start"); });
  ArduinoOTA.onEnd([]()   { logMsg("OTA End");   });
  ArduinoOTA.onError([](ota_error_t error) { logMsg("OTA Error"); });

  server.on("/",                 HTTP_GET, handleRoot);
  server.on("/index.html",       HTTP_GET, handleRoot);
  server.on("/filemanager.html", HTTP_GET, handleFileManagerPage);
  server.on("/logs.html",        HTTP_GET, handleLogsPage);
  server.on("/logs",             HTTP_GET, handleLogs);

  server.on("/list",     HTTP_GET,  handleList);
  server.on("/delete",   HTTP_GET,  handleDelete);
  server.on("/upload",   HTTP_POST, [](){ server.send(200); }, handleUpload);
  server.on("/download", HTTP_GET,  handleDownload);
  server.on("/mkdir",    HTTP_GET,  handleMkdir);
  server.on("/rename",   HTTP_GET,  handleRename);
  server.on("/open",     HTTP_GET,  handleOpen);

  server.on("/runscript",        HTTP_GET, handleRunScript);
  server.on("/autostart/add",    HTTP_GET, handleAutoStartAdd);
  server.on("/autostart/remove", HTTP_GET, handleAutoStartRemove);
  server.on("/runcommand",       HTTP_GET, handleRunCommand);
  server.on("/getautostart",     HTTP_GET, handleGetAutoStart);
  server.on("/listapps",         HTTP_GET, handleListApps);
  server.on("/restart",          HTTP_GET, handleRestart);
  server.on("/clearlogs",        HTTP_GET, handleClearLogs);

  // New monitoring endpoints
  server.on("/tasks", HTTP_GET, handleTasks);
  server.on("/status", HTTP_GET, handleStatus);

  server.onNotFound(handleNotFound);
  server.begin();

  initScriptEngine();
  loadAutoStart();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  schedulerLoop();
  updateCpuUsage();
}

// ——— Core ———
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(SSID, PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) {
    ESP.restart();
  }
}

void logMsg(const char *msg) {
  Serial.println(msg);

  const size_t MAX_LOG_SIZE = 50000;

  if (LittleFS.exists("/logs.txt")) {
    File f = LittleFS.open("/logs.txt", "r");
    if (f && f.size() > MAX_LOG_SIZE) {
      f.close();
      LittleFS.remove("/logs.txt");
    } else if (f) {
      f.close();
    }
  }

  File f = LittleFS.open("/logs.txt", "a");
  if (!f) return;
  f.printf("[%lu] %s\n", millis(), msg);
  f.close();
}

String getContentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg"))  return "image/jpeg";
  if (path.endsWith(".gif"))  return "image/gif";
  if (path.endsWith(".webp")) return "image/webp";
  if (path.endsWith(".bmp"))  return "image/bmp";
  if (path.endsWith(".mp3"))  return "audio/mpeg";
  if (path.endsWith(".mp4"))  return "video/mp4";
  return "application/octet-stream";
}

// ——— Handlers ———
void handleRoot() {
  if (!LittleFS.exists("/index.html"))
    return server.send(404,"text/plain","index.html not found");
  File f = LittleFS.open("/index.html","r");
  server.streamFile(f,"text/html");
  f.close();
}

void handleList() {
  String dir = server.hasArg("dir") ? server.arg("dir") : "/";
  if (!dir.endsWith("/")) dir += "/";

  if (!LittleFS.exists(dir)) {
    server.send(404, "application/json", "[]");
    return;
  }

  File root = LittleFS.open(dir);
  String json = "[";
  File entry;
  while ((entry = root.openNextFile())) {
    if (json.length()>1) json += ",";
    json += String("{\"name\":\"") + entry.name() +
            String("\",\"size\":") + entry.size() +
            String(",\"isDir\":") + (entry.isDirectory()?"true":"false") +
            String("}");
    entry.close();
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleFileManagerPage() {
  File f = LittleFS.open("/filemanager.html","r");
  server.streamFile(f,"text/html");
  f.close();
}

void handleLogsPage() {
  File f = LittleFS.open("/logs.html","r");
  server.streamFile(f,"text/html");
  f.close();
}

void handleLogs(){
  if (!LittleFS.exists("/logs.txt"))
    return server.send(404,"text/plain","logs.txt not found");
  File f = LittleFS.open("/logs.txt","r");
  server.streamFile(f,"text/plain");
  f.close();
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  static File file;

  if (upload.status == UPLOAD_FILE_START) {
    String dir = server.arg("path");
    if (dir.length() == 0) dir = "/";
    String path = dir + (dir.endsWith("/") ? "" : "/") + upload.filename;

    if (LittleFS.exists(path)) {
      logMsg(("Overwriting: " + path).c_str());
      LittleFS.remove(path);
    }

    file = LittleFS.open(path, "w");
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (file) file.write(upload.buf, upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (file) file.close();
  }
}

void handleOpen() {
  String path = server.arg("path");
  if (!LittleFS.exists(path)) return server.send(404,"text/plain","Not Found");
  File f = LittleFS.open(path,"r");
  server.streamFile(f, getContentType(path));
  f.close();
}
void handleDownload() {
  String path = server.arg("path");

  if (!LittleFS.exists(path)) {
    server.send(404, "text/plain", "Not Found");
    return;
  }

  File f = LittleFS.open(path, "r");

  String filename = path.substring(path.lastIndexOf("/") + 1);

  // Force download
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.sendHeader("Connection", "close");

  server.streamFile(f, "application/octet-stream");

  f.close();
}
void handleDelete() {
  String path = server.arg("path");
  if (!LittleFS.exists(path)) return server.send(404,"text/plain","Not Found");
  bool ok = removeRecursive(path);
  server.send(ok?200:500,"text/plain", ok?"Deleted":"Delete failed");
}

void handleMkdir() {
  String path = server.arg("path");
  bool ok = LittleFS.mkdir(path);
  server.send(ok?200:500,"text/plain", ok?"Created":"Failed");
}

void handleRename() {
  String from = server.arg("from"), to = server.arg("to");
  bool ok = LittleFS.rename(from,to);
  server.send(ok?200:500,"text/plain", ok?"Renamed":"Rename failed");
}

void handleNotFound() {
  String uri = server.uri();
  if (!LittleFS.exists(ENDPOINTS_TABLE)) {
    server.send(404,"text/plain","404: Not Found");
    return;
  }

  File f = LittleFS.open(ENDPOINTS_TABLE,"r");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    int c = line.indexOf(',');
    if (c>0 && line.substring(0,c)==uri) {
      String tgt = line.substring(c+1);
      tgt.trim();
      if (LittleFS.exists(tgt)) {
        File hf = LittleFS.open(tgt,"r");
        server.streamFile(hf,getContentType(tgt));
        hf.close(); f.close();
        return;
      }
    }
  }
  f.close();
  server.send(404,"text/plain","404: Not Found");
}

void handleRunCommand() {
  if (!server.hasArg("command")) {
    server.send(400, "text/plain", "Missing command");
    return;
  }

  String line = server.arg("command");
  String cmd;
  ArgList args;

  if (!parseLine(line, cmd, args)) {
    server.send(400, "text/plain", "Invalid format");
    return;
  }

  auto it = commands.find(cmd);
  if (it == commands.end()) {
    logMsg(("Unknown command: " + cmd).c_str());
    server.send(400, "text/plain", "Unknown command");
    return;
  }

  it->second(args);
  server.send(200, "text/plain", "OK");
}

void handleRestart(){
  logMsg("Reboot Initiated Manually !");
  server.send(200, "text/plain", "Rebooting..");
  delay(500);
  ESP.restart();
}

void handleClearLogs() {
  if (LittleFS.exists("/logs.txt")) {
    LittleFS.remove("/logs.txt");
  }
  logMsg("Logs cleared !");
  server.send(200, "text/plain", "Logs cleared");
}
void handleRunScript() {
  if (!server.hasArg("script")) {
    server.send(400, "text/plain", "Missing script");
    return;
  }

  String path = server.arg("script");

  // 🔥 create task instead of blocking run
  createTask(path, true);

  server.send(200, "text/plain", "Task started");
}

// ——— Apps ———
void handleListApps() {
  if (!LittleFS.exists("/apps")) {
    server.send(200, "application/json", "[]");
    return;
  }

  File dir = LittleFS.open("/apps");
  String json = "[";

  File entry;
  while ((entry = dir.openNextFile())) {

    String fname = entry.name();  // usually "sysmon.wx"

    // 🔥 ensure only filename (no path issues)
    if (fname.indexOf("/") != -1) {
      fname = fname.substring(fname.lastIndexOf("/") + 1);
    }

    if (fname.endsWith(".run") || fname.endsWith(".wx")) {

      String name = fname;
      String icon = "default.png";

      // ✅ ALWAYS construct correct path
      String path = "/apps/" + fname;

      File f = LittleFS.open(path, "r");

      if (f) {
        int linesRead = 0;

        while (f.available() && linesRead < 10) {
          String line = f.readStringUntil('\n');
          line.trim();

          if (!line.startsWith("//")) continue;

          // remove "//"
          line.remove(0, 2);
          line.trim();

          int colon = line.indexOf(':');
          if (colon <= 0) continue;

          String key = line.substring(0, colon);
          String val = line.substring(colon + 1);

          key.trim();
          val.trim();

          if (key == "name") name = val;
          else if (key == "icon") icon = val;

          linesRead++;
        }

        f.close();
      } else {
        Serial.println("❌ Failed to open: " + path); // debug
      }

      json += "{";
      json += "\"file\":\"" + fname + "\",";
      json += "\"name\":\"" + name + "\",";
      json += "\"icon\":\"" + icon + "\"";
      json += "},";
    }

    entry.close();
  }

  if (json.endsWith(",")) json.remove(json.length() - 1);
  json += "]";

  server.send(200, "application/json", json);
}

// ——— AutoStart ———
void handleAutoStartAdd() {
  String s = server.arg("script");
  auto L = readAutoStartList();
  if (std::find(L.begin(),L.end(),s) == L.end()) {
    L.push_back(s);
    writeAutoStartList(L);
  }
  server.send(200,"text/plain","ok");
}

void handleAutoStartRemove() {
  String s = server.arg("script");
  auto L = readAutoStartList();
  L.erase(std::remove(L.begin(),L.end(),s),L.end());
  writeAutoStartList(L);
  server.send(200,"text/plain","ok");
}

void handleGetAutoStart() {
  auto L = readAutoStartList();
  String j = "[";
  for (auto &s : L) j += "\"" + s + "\",";
  if (j.endsWith(",")) j.remove(j.length()-1);
  j += "]";
  server.send(200,"application/json",j);
}

std::vector<String> readAutoStartList() {
  std::vector<String> out;
  if (!LittleFS.exists(AUTOSTART_CONFIG)) return out;
  File f = LittleFS.open(AUTOSTART_CONFIG,"r");
  while (f && f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (l.length() && !l.startsWith("//")) out.push_back(l);
  }
  if (f) f.close();
  return out;
}

bool writeAutoStartList(const std::vector<String>& L) {
  File f = LittleFS.open(AUTOSTART_CONFIG,"w");
  if (!f) return false;
  for (auto &s: L) f.println(s);
  f.close();
  return true;
}

void loadAutoStart() {
  if (!LittleFS.exists(AUTOSTART_CONFIG)) return;
  File cfg = LittleFS.open(AUTOSTART_CONFIG, "r");
  while (cfg && cfg.available()) {
    String line = cfg.readStringUntil('\n'); line.trim();
    if (line.length()==0 || line.startsWith("//")) continue;
    String scriptPath = "/apps/" + line;
    createTask(scriptPath, true);
  }
  if (cfg) cfg.close();
}

void handleTasks() {
  String json = "[";

  for (auto &t : tasks) {

    json += "{";
    json += "\"id\":" + String(t.id) + ",";
    json += "\"name\":\"" + t.name + "\",";
    json += "\"running\":" + String(t.running ? "true":"false") + ",";
    json += "\"paused\":" + String(t.paused ? "true":"false") + ",";
    json += "\"cpu\":" + String(t.cpuPercent, 1) + ",";
    json += "\"line\":" + String(t.currentLine);
    json += "},";
  }

  if (json.endsWith(",")) json.remove(json.length()-1);
  json += "]";

  server.send(200, "application/json", json);
}

void handleStatus() {

  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  size_t free  = total - used;

  String json = "{";
  json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"tasks\":" + String(tasks.size()) + ",";
  json += "\"uptime\":" + String(millis()) + ",";
  
  json += "\"disk_total\":" + String(total) + ",";
  json += "\"disk_used\":" + String(used) + ",";
  json += "\"disk_free\":" + String(free);

  json += "}";

  server.send(200, "application/json", json);
}

// ——— Script Engine ———
bool parseLine(const String& line, String& name, ArgList& args) {
  int p=line.indexOf('('), q=line.lastIndexOf(')');
  if (p<1||q<=p) return false;
  name=line.substring(0,p);
  String body=line.substring(p+1,q);
  args.clear();
  int i=0;
  while (i<body.length()) {
    int c=body.indexOf(',',i);
    String a = c<0 ? body.substring(i) : body.substring(i,c);
    a.trim();
    if (a.length()) {
      // remove surrounding quotes
      if (a.startsWith("\"") && a.endsWith("\"") && a.length()>=2) a = a.substring(1,a.length()-1);
      args.push_back(a);
    }
    if (c<0) break;
    i=c+1;
  }
  return true;
}
double evalSimple(String expr) {
  expr.replace(" ", "");

  // handle multiplication and division first
  for (int i = 0; i < expr.length(); i++) {
    if (expr[i] == '*' || expr[i] == '/') {
      int l = i - 1;
      while (l >= 0 && (isdigit(expr[l]) || expr[l] == '.')) l--;
      int r = i + 1;
      while (r < expr.length() && (isdigit(expr[r]) || expr[r] == '.')) r++;

      double a = expr.substring(l + 1, i).toDouble();
      double b = expr.substring(i + 1, r).toDouble();
      double res = (expr[i] == '*') ? a * b : a / b;

      expr = expr.substring(0, l + 1) + String(res) + expr.substring(r);
      i = -1;
    }
  }

  // handle + and -
  double result = 0;
  char op = '+';
  int last = 0;

  for (int i = 0; i <= expr.length(); i++) {
    if (i == expr.length() || expr[i] == '+' || expr[i] == '-') {
      double val = expr.substring(last, i).toDouble();

      if (op == '+') result += val;
      else result -= val;

      if (i < expr.length()) op = expr[i];
      last = i + 1;
    }
  }

  return result;
}
void initScriptEngine() {

  // logging
  commands["logmsg"] = [](const ArgList& A){
    if (A.size()<1) return;
    logMsg(A[0].c_str());
  };

  // http get
  commands["httpget"] = [](const ArgList& A){
    if (A.size()<1) return;
    HTTPClient h;
    String u=A[0]; u.replace("\"","");
    if (h.begin(httpClient,u)) { int code=h.GET(); logMsg(("GET="+String(code)).c_str()); h.end(); }
  };

  // http post
  commands["httppost"] = [](const ArgList& A){
    if (A.size()<2) return;
    HTTPClient h;
    String u=A[0],b=A[1];
    u.replace("\"",""); b.replace("\"","");
    if (h.begin(httpClient,u)) {
      h.addHeader("Content-Type","application/json");
      int code=h.POST(b);
      logMsg(("POST="+String(code)).c_str());
      h.end();
    }
  };

  // pin operations
  commands["setpin"] = [](const ArgList& A){
    if (A.size()<2) return;
    int p=A[0].toInt();
    digitalWrite(p, A[1]=="HIGH"?HIGH:LOW);
  };

  commands["readpin"] = [](const ArgList& A){
    if (A.size()<1) return;
    int p=A[0].toInt();
    logMsg(("PIN"+String(p)+"="+String(digitalRead(p))).c_str());
  };

  commands["pinmode"] = [](const ArgList& A){
    if (A.size()<2) return;
    int p=A[0].toInt();
    pinMode(p, A[1]=="input"?INPUT:OUTPUT);
  };

  // non-blocking delay: schedules current task to sleep for ms
  commands["delayms"] = [](const ArgList& A){
    if (A.size()<1) return;
    if (!currentTask) {
      // if invoked outside task context, fallback to blocking delay
      delay(A[0].toInt());
      return;
    }
    currentTask->delayUntil = millis() + A[0].toInt();
  };

  // write file
  commands["writedata"] = [](const ArgList& A){
    if (A.size()<2) return;
    String p=A[0],d=A[1];
    p.replace("\"",""); d.replace("\"","");
    if (LittleFS.exists(p)) LittleFS.remove(p);
    File f=LittleFS.open(p,"w"); if (f) { f.print(d); f.close(); }
  };

  commands["clearlogs"] = [](const ArgList&){
    if (LittleFS.exists("/logs.txt")) LittleFS.remove("/logs.txt");
    logMsg("Logs cleared !");
    // avoid calling server.send here — handler exists for HTTP
  };

  commands["rm"] = [](const ArgList& A){
    if (A.size()<1) return;
    String p=A[0]; p.replace("\"","");
    removeRecursive(p);
  };

  commands["serverbind"] = [](const ArgList& A){
    if (A.size()<2) return;
    String u=A[0],t=A[1];
    u.replace("\"",""); t.replace("\"","");
    if (!u.startsWith("/")) u="/"+u;
    appendEndpoint(u,t);
  };

  commands["serverunbind"] = [](const ArgList& A){
    if (A.size()<1) return;
    String u=A[0]; u.replace("\"","");
    if (!u.startsWith("/")) u="/"+u;
    removeEndpoint(u);
  };

  commands["togglepin"] = [](const ArgList& A){
    if (A.size()<1) return;
    int p = A[0].toInt();
    int s = digitalRead(p);
    digitalWrite(p, s == LOW ? HIGH : LOW);
  };

  // run script (foreground-ish) and runbg (background)
  commands["run"] = [](const ArgList& A){
    if (A.size()<1) return;
    String p = A[0]; p.replace("\"","");
    // start as foreground? to avoid blocking, start as background to be safe
    createTask(p, false);
  };

  commands["runbg"] = [](const ArgList& A){
    if (A.size()<1) return;
    String p = A[0]; p.replace("\"","");
    createTask(p, true);
  };
commands["kill"] = [](const ArgList& A){
  if (A.size()<1) return;
  int id = A[0].toInt();

  for (auto &t : tasks) {
    if (t.id == id) {
      t.running = false;
      logMsg(("Task killed: " + String(id)).c_str());
    }
  }
};
commands["pause"] = [](const ArgList& A){
  if (A.size()<1) return;
  int id = A[0].toInt();

  for (auto &t : tasks) {
    if (t.id == id && t.running) {
      t.paused = true;
      logMsg(("Task paused: " + String(id)).c_str());
    }
  }
};

commands["resume"] = [](const ArgList& A){
  if (A.size()<1) return;
  int id = A[0].toInt();

  for (auto &t : tasks) {
    if (t.id == id && t.running) {
      t.paused = false;
      logMsg(("Task resumed: " + String(id)).c_str());
    }
  }
};
commands["calc"] = [](const ArgList& A){
  if (A.size() < 1) return;

  String expr = A[0];
  expr.replace("\"","");

  double res = evalSimple(expr);

  logMsg(("CALC: " + expr + " = " + String(res)).c_str());
};
  commands["listcmds"] = [](const ArgList&){
    String all;
    for (auto &kv: commands) { if (all.length()) all+=", "; all+=kv.first; }
    logMsg(("cmds: "+all).c_str());
  };
}
