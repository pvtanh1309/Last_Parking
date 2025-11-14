#include <WiFi.h>
#include <WebSocketsClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>

const char* ssid = "Tuan Anh";
const char* password = "tuananh1309@";
const char* ws_host = "10.75.122.249";
const uint16_t ws_port = 4000;
const char* ws_path = "/";

WiFiUDP ntpUDP;
WebSocketsClient wsClient;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7*3600, 600000);
LiquidCrystal_I2C lcd(0x27, 16, 2);

bool wsConnected = false;
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL = 30000;

struct RfidRequest {
    String rfidCode;
    String action;
    unsigned long requestTime;
    bool waitingForResponse;
};
RfidRequest pendingRfidRequest;
const unsigned long RFID_RESPONSE_TIMEOUT = 20000;

bool forceDisplayUpdate = false;
bool useServerStatus = false;
unsigned long lastServerStatusUpdate = 0;
const unsigned long SERVER_STATUS_TIMEOUT = 30000;

struct ParkingData {    
    uint8_t availableSlots : 4;
    uint8_t totalSlots : 4;
    bool slots[4];
    bool entryGateOpen;
    bool exitGateOpen;
    String lastUpdated;
    bool isAdminMode;
} parkingData;

struct ServerParkingData {  
    uint8_t availableSlots : 4;
    uint8_t totalSlots : 4;
    bool slots[4];
    bool entryGateOpen;
    bool exitGateOpen;
    String lastUpdated;
    bool isAdminMode;
    bool hasData; 
} serverParkingData;

struct HistoryEntry {
    String event;
    String idCode;
    String action;
    String method;
    String servoType;
    String timestamp;
    String triggerSource;
};
HistoryEntry history[20];
int historycount = 0;

void initializeWiFi() {
    WiFi.begin(ssid, password);
    lcd.clear();
    lcd.print(F("Connecting WiFi"));
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        lcd.setCursor(0, 1);
        lcd.print(".");
    }
    
    lcd.clear();
    lcd.print(F("Connected"));
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    delay(2000);
}

void initializeWebSocket() {
    wsClient.begin(ws_host, ws_port, ws_path);
    wsClient.onEvent(handleWebSocketEvent);
    wsClient.setReconnectInterval(5000);
    wsClient.enableHeartbeat(15000, 3000, 2);
    wsConnected = false;
}

//Khởi tạo dữ liệu ban đầu của bãi
void initializeParkingData() {
    parkingData.availableSlots = 4;
    parkingData.totalSlots = 4;
    for (int i = 0; i < 4; i++) {
        parkingData.slots[i] = false;
    }
    parkingData.entryGateOpen = false;
    parkingData.exitGateOpen = false;
    parkingData.isAdminMode = false;
}

void handleWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            handleDisconnection();
            break;
        case WStype_CONNECTED:
            handleConnection(payload);
            break;
        case WStype_TEXT:
            handleTextMessage(payload, length);
            break;
        case WStype_ERROR:
            wsConnected = false;
            break;
        case WStype_PONG:
            break;
    }
}

void handleDisconnection() {
    wsConnected = false;
    useServerStatus = false;
}

void handleConnection(uint8_t * payload) {
    wsConnected = true;
    sendConnectionIdentification();
    lastPingTime = millis();
}

void sendConnectionIdentification() {
    DynamicJsonDocument doc(256);
    doc["type"] = "esp32_connect";
    doc["deviceId"] = "ESP32_PARKING_SYSTEM";
    doc["timestamp"] = timeClient.getFormattedTime();
    
    String connectMsg;
    serializeJson(doc, connectMsg);
    wsClient.sendTXT(connectMsg);
}

void handleTextMessage(uint8_t * payload, size_t length) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, (char*)payload);
    if (error) return;
    
    String type = doc["type"];
    
    if (type == "status_update") {
        handleStatusUpdate(doc);
    } else if (type == "rfid_response") {
        handleRfidResponse(doc);
    } else if (type == "control") {
        handleControlMessage(doc);
    } else if (type == "ping") {
        respondToPing();
    }
}

void handleStatusUpdate(DynamicJsonDocument& doc) {
    serverParkingData.availableSlots = doc["data"]["availableSlots"];
    serverParkingData.totalSlots = doc["data"]["totalSlots"];
    serverParkingData.entryGateOpen = doc["data"]["entryGateOpen"];
    serverParkingData.exitGateOpen = doc["data"]["exitGateOpen"];
    serverParkingData.isAdminMode = doc["data"]["isAdminMode"];
    serverParkingData.lastUpdated = doc["data"]["lastUpdate"].as<const char*>();
    serverParkingData.hasData = true;
    
    JsonArray slots = doc["data"]["slots"];
    for (int i = 0; i < 4; i++) {
        serverParkingData.slots[i] = slots[i];
    }
    
    useServerStatus = true;
    lastServerStatusUpdate = millis();
    forceDisplayUpdate = true;
}

void handleRfidResponse(DynamicJsonDocument& doc) {
    String rfidCode = doc["data"]["rfidCode"];
    String status = doc["data"]["status"];
    
    if (status == "ENTRY_RECORDED") {
        for (int i = 0; i < 4; i++) {
            if (!parkingData.slots[i]) { 
                parkingData.slots[i] = true;
                parkingData.availableSlots--;
                break;
            }
        }
        parkingData.lastUpdated = timeClient.getFormattedTime();
        forceDisplayUpdate = true;

    } else if (status == "EXIT_RECORDED") {
        if (doc["data"].containsKey("slotUsed")) {
            String slot = doc["data"]["slotUsed"].as<String>();
            if (slot.startsWith("A") && slot.length() == 2) { 
                int slotIndex = slot.substring(1).toInt() - 1; // "A3" → "3" → 3 → 2
                if (slotIndex >= 0 && slotIndex < 4) {
                    parkingData.slots[slotIndex] = false;
                    parkingData.availableSlots++;
                }
            }
        }
        parkingData.lastUpdated = timeClient.getFormattedTime();
        forceDisplayUpdate = true;
    }

    if (status == "PLATE_MISMATCH") {
        handlePlateMismatch(doc);
    } else if (status == "EXIT_RECORDED") {
        handleExitRecorded(doc);
    } else if (status == "ENTRY_RECORDED") {
        handleEntryRecorded(doc);
    } else if (status == "ERROR") {
        handleDatabaseError(doc);
    }
    
    resetPendingRequest(rfidCode);
}

void handlePlateMismatch(DynamicJsonDocument& doc) {
    String entryPlate = doc["data"]["entryPlate"].as<String>();
    String exitPlate = doc["data"]["exitPlate"].as<String>();
    
    displayMessage("PLATE MISMATCH!", "EXIT DENIED", 3000);
    displayPlateComparison(entryPlate, exitPlate);
    
    Serial.println("DATABASE_RESPONSE:ERROR");
}

void handleExitRecorded(DynamicJsonDocument& doc) {
    int fee = doc["data"]["fee"];
    int duration = doc["data"]["duration"];
    
    displayFeeInfo(fee, duration);
    Serial.println("DATABASE_RESPONSE:SUCCESS");
}

void handleEntryRecorded(DynamicJsonDocument& doc) {
    String rfidCode = doc["data"]["rfidCode"];
    displayMessage("ENTRY RECORDED", rfidCode, 2000);
    
    if (doc["data"].containsKey("licensePlate")) {
        String plate = doc["data"]["licensePlate"].as<String>();
        displayMessage("License Plate:", plate, 2000);
    }
    
    Serial.println("DATABASE_RESPONSE:SUCCESS");
}

void handleDatabaseError(DynamicJsonDocument& doc) {
    String message = doc["data"]["message"];
    
    Serial.println("DATABASE_RESPONSE:ERROR");
    
    Serial.print("DB Error: ");
    Serial.println(message);
}

void handleControlMessage(DynamicJsonDocument& doc) {
    String action = doc["data"]["action"];
    String gateId = doc["data"]["gateId"];
    
    if (action == "toggle_gate") {
        String servoType = (gateId == "gate1" || gateId == "entry_gate") ? "ENTRY_GATE" : "EXIT_GATE";
        
        if (gateId == "gate1" || gateId == "entry_gate") {
            Serial.println("OPEN_ENTRY_GATE");
        } else if (gateId == "gate2" || gateId == "exit_gate") {
            Serial.println("OPEN_EXIT_GATE");
        }
        
        addHistory("WEB_CONTROL", "REMOTE", "OPEN", "WEB", servoType, "WEB");
        notifyServoActivation(servoType, "WEB", "REMOTE", "WEB");
    }
}

void respondToPing() {
    String pongMsg = "{\"type\":\"pong\",\"timestamp\":\"" + timeClient.getFormattedTime() + "\"}";
    wsClient.sendTXT(pongMsg);
}

void displayMessage(String line1, String line2, int delayTime) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcd.setCursor(0, 1);
    lcd.print(line2);
    delay(delayTime);
}

void displayPlateComparison(String entryPlate, String exitPlate) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("In: " + entryPlate.substring(0, 13));
    lcd.setCursor(0, 1);
    lcd.print("Out:" + exitPlate.substring(0, 13));
    delay(3000);
}

void displayFeeInfo(int fee, int duration) {
    displayMessage("EXIT RECORDED", "Fee: " + String(fee) + " VND", 2000);
    displayMessage("Duration:", String(duration) + " minutes", 2000);
}

void updateDisplay() {
    lcd.clear();
    lcd.setCursor(0, 0);
    
    // Số slot trống: Luôn theo server nếu có
    int availableCount;
    int totalCount;
    if (wsConnected && useServerStatus && serverParkingData.hasData) {
        availableCount = serverParkingData.availableSlots;
        totalCount = serverParkingData.totalSlots;
    } else {
        availableCount = parkingData.availableSlots;
        totalCount = parkingData.totalSlots;
    }
    
    lcd.print("Slots: " + String(availableCount) + "/" + String(totalCount));
    
    if (!wsConnected) {
        lcd.print(" (!)");
    }
    
    lcd.setCursor(0, 1);
    if (availableCount > 0) {
        lcd.print("Slot: ");
        for (int i = 0; i < 4; i++) {
            lcd.print(parkingData.slots[i] ? "O" : "X");
        }
    } else {
        lcd.print("PARKING FULL");
    }
}

void displaySlotStatus(ParkingData* data) {
    for (int i = 0; i < 4; i++) {
        if (useServerStatus && serverParkingData.hasData) {
            lcd.print(serverParkingData.slots[i] == 1 ? "O" : 
                     serverParkingData.slots[i] == 2 ? "R" : "X");
        } else {
            lcd.print(data->slots[i] ? "O" : "X");
        }
    }
}

void sendKeepAlive() {
    if (!wsConnected || (millis() - lastPingTime <= PING_INTERVAL)) return;
    
    DynamicJsonDocument pingDoc(256);
    pingDoc["type"] = "ping";
    pingDoc["deviceId"] = "ESP32_PARKING_SYSTEM";
    pingDoc["timestamp"] = timeClient.getFormattedTime();
    
    String pingMsg;
    serializeJson(pingDoc, pingMsg);
    wsClient.sendTXT(pingMsg);
    lastPingTime = millis();
}

void sendStatusResponse() {
    if (!wsConnected) return;
    
    DynamicJsonDocument statusDoc(1024);
    
    // Số slot theo server
    statusDoc["availableSlots"] = useServerStatus && serverParkingData.hasData ? serverParkingData.availableSlots : parkingData.availableSlots;
    statusDoc["totalSlots"] = useServerStatus && serverParkingData.hasData ? serverParkingData.totalSlots : parkingData.totalSlots;
    
    // Gate status và admin mode theo server
    statusDoc["entryGateOpen"] = useServerStatus && serverParkingData.hasData ? serverParkingData.entryGateOpen : parkingData.entryGateOpen;
    statusDoc["exitGateOpen"] = useServerStatus && serverParkingData.hasData ? serverParkingData.exitGateOpen : parkingData.exitGateOpen;
    statusDoc["isAdminMode"] = useServerStatus && serverParkingData.hasData ? serverParkingData.isAdminMode : parkingData.isAdminMode;
    
    statusDoc["lastUpdate"] = parkingData.lastUpdated;
    statusDoc["isRFIDMode"] = true;
    
    // Luôn theo local data
    JsonArray slots = statusDoc.createNestedArray("slots");
    for (int i = 0; i < 4; i++) {
        slots.add(parkingData.slots[i]);
    }
    
    String statusJson;
    serializeJson(statusDoc, statusJson); // Chuyển từ chuỗi doc thành chuỗi String
    wsClient.sendTXT("{\"type\":\"status\",\"data\":" + statusJson + "}"); // availableSlots, totalSlots, slots, entryGateOpen, exitGateOpen, isAdminMode,
}

void sendRfidAccessToDatabase(String rfidCode, String action, String servoType) {
    if (!wsConnected) {
        Serial.println("DATABASE_RESPONSE:ERROR");
        displayMessage("NO CONNECTION", "Entry allowed", 2000);
        return;
    }
    
    setPendingRequest(rfidCode, action);
    
    DynamicJsonDocument doc(512);
    doc["type"] = "rfid_access";
    doc["data"]["rfidCode"] = rfidCode;
    doc["data"]["action"] = action;
    doc["data"]["servoType"] = servoType;
    doc["data"]["timestamp"] = getISOTimestamp();
    doc["data"]["deviceId"] = "ESP32_PARKING_SYSTEM";
    doc["data"]["method"] = "RFID";
    
    String slotInfo = "Unknown";
    if (servoType == "ENTRY_GATE") {
        for (int i = 0; i < 4; i++) {
            if (useServerStatus && serverParkingData.hasData) {
                if (serverParkingData.slots[i] == 0) {
                    slotInfo = "A" + String(i + 1);
                    break;
                }
            } else {
                if (!parkingData.slots[i]) {
                    slotInfo = "A" + String(i + 1);
                    break;
                }
            }
        }
    }
    doc["data"]["slotUsed"] = slotInfo;
    
    String rfidJson;
    serializeJson(doc, rfidJson);
    wsClient.sendTXT(rfidJson);
}

void parseArduinoData(String data) {
    data.trim();
    if (data.length() == 0) return;
    
    //Kiểm tra đầu chuỗi
    if (data.startsWith("RFID_DATABASE:")) { 
        handleRfidDatabase(data); //RFID_DATABASE:event,rfidCode,action,servoType,timestamp
    } else if (data.startsWith("HISTORY:")) {
        handleHistoryMessage(data); //HISTORY:event,idCode,action,servoType
    } else if (data.startsWith("VEHICLE_ENTRY:") || data.startsWith("VEHICLE_EXIT:")) {
        Serial.println(data);
    } else if (data.startsWith("{")) {
        handleJsonData(data);
    } else if (data == "OPEN_ENTRY_GATE" || data == "OPEN_EXIT_GATE") {
        handleGateControl(data);
    } else if (data.startsWith("MODE_CHANGED:")) {
        Serial.println("Mode confirmed: " + data.substring(13));
    }
}

void handleRfidDatabase(String data) {
    String dbData = data.substring(14); // RFID_DATABASE
    int indices[4];
    findCommaIndices(dbData, indices, 4);
    
    if (indices[3] != -1) {
        String event = dbData.substring(0, indices[0]);
        String rfidCode = dbData.substring(indices[0] + 1, indices[1]);
        String action = dbData.substring(indices[1] + 1, indices[2]);
        String servoType = dbData.substring(indices[2] + 1, indices[3]);
        
        displayMessage("Welcome!", rfidCode, 1500);
        displayMessage("Processing...", "Please wait", 0);
        sendRfidAccessToDatabase(rfidCode, action, servoType);
    }
}

void handleHistoryMessage(String data) {
    String historyData = data.substring(8);
    int firstComma = historyData.indexOf(',');
    int secondComma = historyData.indexOf(',', firstComma + 1);
    int thirdComma = historyData.indexOf(',', secondComma + 1);
    
    if (firstComma != -1 && secondComma != -1) {
        String event = historyData.substring(0, firstComma);
        String idCode = historyData.substring(firstComma + 1, secondComma);
        String action = historyData.substring(secondComma + 1, thirdComma != -1 ? thirdComma : historyData.length());
        String servoType = (thirdComma != -1) ? historyData.substring(thirdComma + 1) : 
        (action == "Entry" ? "ENTRY_GATE" : "EXIT_GATE");
        
        addHistory(event, idCode, action, "RFID", servoType, "USER");
        notifyServoActivation(servoType, "RFID", idCode, "USER");
        
        displayMessage(event + ": " + action, idCode, 2000);
        forceDisplayUpdate = true;
    }
}

void handleJsonData(String data) {
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, data)) return;
    
    parkingData.availableSlots = doc["available"];
    parkingData.totalSlots = doc["total"];
    parkingData.entryGateOpen = doc["entryGateOpen"];
    parkingData.exitGateOpen = doc["exitGateOpen"];
    parkingData.lastUpdated = timeClient.getFormattedTime();
    
    JsonArray slots = doc["slots"];
    for (int i = 0; i < 4; i++) {
        parkingData.slots[i] = slots[i];
    }
}

void handleGateControl(String data) {
    String gateType = (data == "OPEN_ENTRY_GATE") ? "ENTRY_GATE" : "EXIT_GATE";
    String action = (data == "OPEN_ENTRY_GATE") ? "ENTER" : "EXIT";
    
    Serial.println(data);
    addHistory("GATE_CONTROL", "MANUAL", action, "RFID", gateType, "WEB");
    notifyServoActivation(gateType, "RFID", "MANUAL", "WEB");
}

void findCommaIndices(String str, int* indices, int count) {
    int lastIndex = -1;
    for (int i = 0; i < count; i++) {
        indices[i] = str.indexOf(',', lastIndex + 1);
        lastIndex = indices[i];
    }
}

String getISOTimestamp() {
    time_t now = timeClient.getEpochTime();
    struct tm *timeinfo = gmtime(&now);
    char isoTime[25];
    strftime(isoTime, sizeof(isoTime), "%Y-%m-%dT%H:%M:%SZ", timeinfo);
    return String(isoTime);
}

void setPendingRequest(String rfidCode, String action) {
    pendingRfidRequest.rfidCode = rfidCode;
    pendingRfidRequest.action = action;
    pendingRfidRequest.requestTime = millis();
    pendingRfidRequest.waitingForResponse = true;
}

void resetPendingRequest(String rfidCode) {
    if (pendingRfidRequest.waitingForResponse && 
        pendingRfidRequest.rfidCode == rfidCode) {
        pendingRfidRequest.waitingForResponse = false;
    }
}

void checkRfidResponseTimeout() {
    if (!pendingRfidRequest.waitingForResponse) return;
    
    if (millis() - pendingRfidRequest.requestTime > RFID_RESPONSE_TIMEOUT) {
        Serial.println("DATABASE_RESPONSE:ERROR");
        pendingRfidRequest.waitingForResponse = false;
    }
}

void addHistory(String event, String idCode, String action, String method, String servoType, String triggerSource) {
    if (historycount < 20) {
        setHistoryEntry(historycount, event, idCode, action, method, servoType, triggerSource); 
        historycount++; //Chưa đầy, thêm vào cuối
    } else {
        shiftHistoryUp(); //Xóa phần tử cũ nhất
        setHistoryEntry(19, event, idCode, action, method, servoType, triggerSource);
    }
    
    if (wsConnected) {
        sendHistoryUpdate();
    }
}

void setHistoryEntry(int index, String event, String idCode, String action,String method, String servoType, String triggerSource) {
    history[index].event = event;
    history[index].idCode = idCode;
    history[index].action = action;
    history[index].method = "RFID";
    history[index].servoType = servoType.isEmpty() ? determineServoType(action) : servoType;
    history[index].triggerSource = triggerSource;
    history[index].timestamp = timeClient.getFormattedTime();
}

String determineServoType(String action) {
    if (action == "ENTER" || action == "Entry") return "ENTRY_GATE";
    if (action == "EXIT" || action == "Exit") return "EXIT_GATE";
    return parkingData.entryGateOpen ? "ENTRY_GATE" : (parkingData.exitGateOpen ? "EXIT_GATE" : "N/A");
}

void shiftHistoryUp() {
    for (int i = 0; i < 19; i++) {
        history[i] = history[i + 1];
    }
}

void sendHistoryUpdate() {
    if (!wsConnected) return;
    
    DynamicJsonDocument historyDoc(3072);
    JsonArray historyArray = historyDoc.createNestedArray("history");
    
    for (int i = 0; i < historycount; i++) {
        JsonObject entry = historyArray.createNestedObject();
        entry["event"] = history[i].event;
        entry["idCode"] = history[i].idCode;
        entry["action"] = history[i].action;
        entry["method"] = "RFID";
        entry["servoType"] = history[i].servoType;
        entry["triggerSource"] = history[i].triggerSource;
        entry["timestamp"] = history[i].timestamp;
    }
    
    String historyJson;
    serializeJson(historyDoc, historyJson);
    wsClient.sendTXT("{\"type\":\"history\",\"data\":" + historyJson + "}");
}

void notifyServoActivation(String servoType, String triggerMethod, String triggerCode, String triggerSource) {
    if (!wsConnected) return;
    
    if (servoType == "PENDING") {
        servoType = parkingData.entryGateOpen ? "ENTRY_GATE" : (parkingData.exitGateOpen ? "EXIT_GATE" : "N/A");
    }
    
    DynamicJsonDocument doc(512);
    doc["type"] = "servo_activation";
    doc["servoType"] = servoType;
    doc["triggerMethod"] = "RFID";
    doc["triggerCode"] = triggerCode;
    doc["triggerSource"] = triggerSource;
    doc["timestamp"] = timeClient.getFormattedTime();
    
    String json;
    serializeJson(doc, json);
    wsClient.sendTXT(json);
}

void checkServerStatusTimeout() {
    if (useServerStatus && (millis() - lastServerStatusUpdate > SERVER_STATUS_TIMEOUT)) {
        useServerStatus = false;
        forceDisplayUpdate = true;
    }
}

void setup() {
    Serial.begin(9600);
    
    Wire.begin(21, 22);
    lcd.init();
    lcd.backlight();
    displayMessage("Smart Parking", "RFID System", 2000);

    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    
    initializeWiFi();
    timeClient.begin();
    initializeParkingData();
    initializeWebSocket();
    
    Serial.println("RFID System started");
    Serial.println("RFID");
    updateDisplay();
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        wsClient.loop();
        sendKeepAlive();
    } else {
        WiFi.begin(ssid, password);
        wsConnected = false;
        delay(1000);
    }
    
    timeClient.update();
    
    if (Serial.available()) {
        String data = Serial.readStringUntil('\n');
        parseArduinoData(data);
    }
    
    checkRfidResponseTimeout();
    checkServerStatusTimeout();
    
    static unsigned long lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate > 200) {
        updateDisplay();
        lastDisplayUpdate = millis();
    }
    
    // Gửi trạng thái định kỳ
    static unsigned long lastPeriodicUpdate = 0;
    if (wsConnected && millis() - lastPeriodicUpdate > 2000) {
        sendStatusResponse();
        lastPeriodicUpdate = millis();
    }
    
    delay(100);
}