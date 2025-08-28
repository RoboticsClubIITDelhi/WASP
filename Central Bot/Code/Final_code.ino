#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

// ---- Motor Pins ----
#define IN1_PIN 27
#define IN2_PIN 26
#define IN3_PIN 25
#define IN4_PIN 33
#define TRIG_PIN 14   // choose any free GPIO
#define ECHO_PIN 12
#define LEFT_PWM_CH   0
#define RIGHT_PWM_CH  1

int motorSpeed = 128;

// ---- SSIDs of your WASPs ----
const char* knownSSIDs[4] = {"wasp1", "wasp2", "wasp3", "wasp4"};

// ---- Coordinates of APs (meters) ----
float apCoords[4][2] = {
  {0, 0},    
  {0, 5},    
  {5,0},    
  {5,5}     
};

// ---- Environment constants ----
float txPower = -40.0;        // Calibrate at 1m manually
const float pathLossExp = 2;  // Path loss exponent (2 ~ free space)

// ---- Variables ----
float distances[4];         
int rawRSSI[4] = {-100, -100, -100, -100};
float posX = 0, posY = 0; // Estimated position

// ---- WebServer ----
WebServer server(80);

// ======================================================
// Motor Functions
// ======================================================
void CAR_moveForward() { digitalWrite(IN1_PIN,HIGH); digitalWrite(IN2_PIN,LOW);  digitalWrite(IN3_PIN,HIGH); digitalWrite(IN4_PIN,LOW); }
void CAR_moveBackward(){ digitalWrite(IN1_PIN,LOW);  digitalWrite(IN2_PIN,HIGH); digitalWrite(IN3_PIN,LOW);  digitalWrite(IN4_PIN,HIGH);}
void CAR_turnLeft()    { digitalWrite(IN1_PIN,HIGH); digitalWrite(IN2_PIN,LOW);  digitalWrite(IN3_PIN,LOW);  digitalWrite(IN4_PIN,LOW); }
void CAR_turnRight()   { digitalWrite(IN1_PIN,LOW);  digitalWrite(IN2_PIN,LOW);  digitalWrite(IN3_PIN,HIGH); digitalWrite(IN4_PIN,LOW); }
void CAR_stop()        { digitalWrite(IN1_PIN,LOW);  digitalWrite(IN2_PIN,LOW);  digitalWrite(IN3_PIN,LOW);  digitalWrite(IN4_PIN,LOW); }

// ======================================================
// WiFi RSSI → Distance
// ======================================================
long getDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 20000); // timeout 20ms (~3.4m)
  long distance = duration * 0.034 / 2; // cm
  return distance;
}
//======================================================
float calculateDistance(int rssi) {
  if (rssi < -80) return -1; // Ignore weak APs
  return pow(10.0, ((txPower - rssi) / (10.0 * pathLossExp)));
}

// ======================================================
// Scan all APs once (faster than per-SSID scan)
// ======================================================
void scanAllAPs() {
  int n = WiFi.scanNetworks();
  for (int j = 0; j < 4; j++) {
    rawRSSI[j] = -100;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == String(knownSSIDs[j])) {
        rawRSSI[j] = WiFi.RSSI(i);
        break;
      }
    }
    distances[j] = calculateDistance(rawRSSI[j]);
  }
}

// ======================================================
// Trilateration (top 3 strongest APs)
// ======================================================
void trilaterate(float &x, float &y) {
  struct APinfo {
    int rssi;
    float dist;
    int idx;
  };

  APinfo aps[4];
  for (int i = 0; i < 4; i++) {
    aps[i].rssi = rawRSSI[i];
    aps[i].dist = distances[i];
    aps[i].idx = i;
  }

  // Sort by strongest RSSI
  for (int i = 0; i < 4; i++) {
    for (int j = i+1; j < 4; j++) {
      if (aps[j].rssi > aps[i].rssi) {
        APinfo tmp = aps[i];
        aps[i] = aps[j];
        aps[j] = tmp;
      }
    }
  }

  // Pick top 3 with valid distances
  int chosen[3], count = 0;
  for (int i = 0; i < 4 && count < 3; i++) {
    if (aps[i].dist > 0) {
      chosen[count++] = i;
    }
  }

  if (count < 3) { x = y = 0; return; }

  float x1 = apCoords[aps[chosen[0]].idx][0], y1 = apCoords[aps[chosen[0]].idx][1], r1 = aps[chosen[0]].dist;
  float x2 = apCoords[aps[chosen[1]].idx][0], y2 = apCoords[aps[chosen[1]].idx][1], r2 = aps[chosen[1]].dist;
  float x3 = apCoords[aps[chosen[2]].idx][0], y3 = apCoords[aps[chosen[2]].idx][1], r3 = aps[chosen[2]].dist;

  float A = 2 * (x2 - x1);
  float B = 2 * (y2 - y1);
  float C = r1 * r1 - r2 * r2 - x1 * x1 + x2 * x2 - y1 * y1 + y2 * y2;
  float D = 2 * (x3 - x2);
  float E = 2 * (y3 - y2);
  float F = r2 * r2 - r3 * r3 - x2 * x2 + x3 * x3 - y2 * y2 + y3 * y3;

  float denom = (E*A - B*D);
  if (fabs(denom) < 1e-6) { x = y = 0; return; }

  x = (C*E - F*B) / denom;
  y = (C*D - A*F) / (B*D - A*E);
}

// ======================================================
// Web Page (HTML + JS)
// ======================================================
String webpage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>WASP Bot Control</title>
  <style>
    body { font-family: Arial; text-align: center; }
    .btn { width:80px; height:80px; font-size:32px; margin:10px; }
    #coords { margin-top:20px; font-size:20px; }
    #getBtn { margin-top:15px; padding:12px 24px; font-size:18px; }
    .disabled { background: #ccc; color: #777; pointer-events: none; }
  </style>
</head>
<body>
  <h2>WASP Bot Control</h2>
  <!-- Joystick Buttons -->
  <div id="joystick">
    <button class="btn" onclick="move('forward')">↑</button><br>
    <button class="btn" onclick="move('left')">←</button>
    <button class="btn" onclick="move('stop')">■</button>
    <button class="btn" onclick="move('right')">→</button><br>
    <button class="btn" onclick="move('backward')">↓</button>
  </div>

  <!-- Coordinates -->
  <div>
    <button id="getBtn" onclick="getCoords()">Get Coordinates</button>
    <div id="coords">Coordinates: Not yet fetched</div>
  </div>

<script>
function move(dir){ fetch('/'+dir); }

// Disable joystick
function setJoystick(enabled) {
  let buttons = document.querySelectorAll('#joystick button');
  buttons.forEach(b => {
    if (enabled) {
      b.classList.remove('disabled');
    } else {
      b.classList.add('disabled');
    }
  });
}

function getCoords(){
  document.getElementById('coords').innerText = "Updating...";
  setJoystick(false); // disable joystick
  fetch('/coords').then(r=>r.text()).then(txt=>{
    document.getElementById('coords').innerText = txt;
    setJoystick(true); // re-enable joystick
  }).catch(()=>{
    document.getElementById('coords').innerText = "Error fetching coords";
    setJoystick(true); // re-enable even if error
  });
}
</script>
</body>
</html>
)rawliteral";
}




// ======================================================
// Web Handlers
// ======================================================
void handleRoot() { server.send(200,"text/html",webpage()); }
void handleCoords() {
  String data = "Coordinates: X=" + String(posX,2) + ", Y=" + String(posY,2);
  server.send(200,"text/plain",data);
}
void handleForward(){ CAR_moveForward(); server.send(200,"text/plain","Forward"); }
void handleBackward(){ CAR_moveBackward(); server.send(200,"text/plain","Backward"); }
void handleLeft(){ CAR_turnLeft(); server.send(200,"text/plain","Left"); }
void handleRight(){ CAR_turnRight(); server.send(200,"text/plain","Right"); }
void handleStop(){ CAR_stop(); server.send(200,"text/plain","Stop"); }

// ======================================================
// Setup
// ======================================================
void setup() {
  pinMode(IN1_PIN,OUTPUT); pinMode(IN2_PIN,OUTPUT);
  pinMode(IN3_PIN,OUTPUT); pinMode(IN4_PIN,OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
    
  



  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin("Skillissue?","00000001"); // <-- CHANGE
  while(WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  // Routes
  server.on("/", handleRoot);
  server.on("/coords", handleCoords);
  server.on("/forward", handleForward);
  server.on("/backward", handleBackward);
  server.on("/left", handleLeft);
  server.on("/right", handleRight);
  server.on("/stop", handleStop);
  server.begin();
}

// ======================================================
// Loop
// ======================================================
unsigned long lastScan = 0;
void loop() {
  server.handleClient();

  // 🚨 Ultrasonic safety check
  long dist = getDistanceCM();
  if (dist > 0 && dist <= 25) {   // threshold hit
    Serial.printf("Obstacle detected at %ld cm! Backing off...\n", dist);
    CAR_moveBackward();
    delay(1000);   // move back 1s
    CAR_stop();
  }


  // Update position every 20s
  if (millis() - lastScan > 20000) {
    scanAllAPs();
    int goodAPs=0; for(int i=0;i<4;i++) if(distances[i]>0) goodAPs++;
    if (goodAPs >= 3) {
      trilaterate(posX,posY);
      Serial.printf("Estimated Position: X=%.2f, Y=%.2f\n", posX, posY);
    } else {
      Serial.println("Not enough APs for trilateration.");
    }
    lastScan = millis();
  }
}