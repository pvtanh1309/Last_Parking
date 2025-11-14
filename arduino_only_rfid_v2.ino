#include <Servo.h>
#include <SPI.h>
#include <MFRC522.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

#define RST_PIN         9
#define SS_PIN          10
#define IR_SLOT1        3
#define IR_SLOT2        4
#define IR_SLOT3        7
#define IR_SLOT4        8
#define SERVO_ENTRY_PIN 5
#define SERVO_EXIT_PIN  6

MFRC522 rfid(SS_PIN, RST_PIN);

Servo servoEntry;
Servo servoExit;

bool waitingForDatabaseResponse = false;
unsigned long databaseRequestTime = 0;
const unsigned long DATABASE_RESPONSE_TIMEOUT = 15000; // Chờ phản hồi max 15s

struct PendingDatabaseRequest {
  char rfidCode[9];
  char action[6]; // "ENTER" hoặc "EXIT"
  unsigned long requestTime;
  bool responseReceived;
  bool responseSuccess;
};
PendingDatabaseRequest dbRequest;

bool parkingSlots[4] = {false, false, false, false};
const int totalSlots = 4;
int activeVehicles = 0;
int availableSlots = totalSlots;


char authorizedCards[10][9];
int cardCount = 0;
unsigned long lastUpdate = 0;
unsigned long lastHeartbeat = 0;

unsigned long gateOpenTime = 0;
bool entryGateOpen = false;
bool exitGateOpen = false;
const unsigned long GATE_OPEN_DURATION = 5000; 

unsigned long gateDelayStartTime = 0;
bool entryGateWaitingToOpen = false;
bool exitGateWaitingToOpen = false;
const unsigned long GATE_DELAY_DURATION = 5000; 

struct VehicleInfo {
  char identifier[9];
  unsigned long entryTime;
  bool isActive;
  byte slotNumber;
};
VehicleInfo parkedVehicles[4];

bool databaseMode = true;
unsigned long lastDatabaseSync = 0;
const unsigned long DATABASE_SYNC_INTERVAL = 2000; //Đồng bộ DL mỗi 2s

void loadAuthorizedData();
void updateParkingStatus();
void checkRFID();
void handleEntry(const char* cardID);
void handleExit(const char* cardID);
void sendHistory(const char* event, const char* idCode, const char* action);
void sendRfidToDatabase(const char* event, const char* idCode, const char* action, const char* servoType);
bool isAuthorizedCard(const char* cardID);
void openEntryGate();
void openExitGate();
void closeAllGates();
void checkGateTimeout();
void checkGateDelay();
void sendDataToESP32();
void addAuthorizedCard(const char* cardID);
void removeAuthorizedCard(const char* cardID);
void processSerialCommand(String command);
void handleModeChange();
void handleAddCard(String command);
void handleRemoveCard(String command);
void handleOpenEntryGate();
void handleOpenExitGate();
void handleCloseGates();
void handleListCards();
void handleListVehicles();
void handleClearEeprom();
void handleDatabaseStatus();
void handleDatabaseResponse(String command);
int findVehicleByID(const char* identifier);
int findEmptySlot();
void addVehicle(const char* identifier);
void removeVehicle(int vehicleIndex);
void saveAuthorizedData();
void checkDatabaseResponseStatus();

void setup() {
  Serial.begin(9600);
 
  SPI.begin();
  rfid.PCD_Init();
 
  servoEntry.attach(SERVO_ENTRY_PIN);
  servoExit.attach(SERVO_EXIT_PIN);
  servoEntry.write(0);  
  servoExit.write(0);   
 
  pinMode(IR_SLOT1, INPUT_PULLUP);
  pinMode(IR_SLOT2, INPUT_PULLUP);
  pinMode(IR_SLOT3, INPUT_PULLUP);
  pinMode(IR_SLOT4, INPUT_PULLUP);
 
  loadAuthorizedData();

  for (int i = 0; i < 4; i++) {
    memset(parkedVehicles[i].identifier, 0, 9);
    parkedVehicles[i].entryTime = 0;
    parkedVehicles[i].isActive = false;
    parkedVehicles[i].slotNumber = 0;
  }
 
  Serial.println(F("Hệ thống đã được khởi tạo"));
}

void loop() {
  if (Serial.available()) {                         //Chờ lệnh
    String command = Serial.readStringUntil('\n');
    command.trim();
    processSerialCommand(command); // Gửi đến hàm quản lý
  }
 
  updateParkingStatus();
  checkRFID(); 
  checkGateDelay();
  checkGateTimeout();
  checkDatabaseResponseStatus();
 
  if (millis() - lastUpdate > 1000) {
    sendDataToESP32();
    lastUpdate = millis();
  }

  if (millis() - lastHeartbeat > 10000) {
    Serial.println(F("ALIVE"));
    lastHeartbeat = millis();
  }
  delay(50);
}

void updateParkingStatus() {
  parkingSlots[0] = (digitalRead(IR_SLOT1) == LOW);
  parkingSlots[1] = (digitalRead(IR_SLOT2) == LOW);
  parkingSlots[2] = (digitalRead(IR_SLOT3) == LOW);
  parkingSlots[3] = (digitalRead(IR_SLOT4) == LOW);
}

void checkGateDelay() {
  unsigned long currentTime = millis();
  
  if (entryGateWaitingToOpen) {
    if (currentTime - gateDelayStartTime >= GATE_DELAY_DURATION) {
      if (waitingForDatabaseResponse) {
        Serial.println(F("Chờ phản hồi từ Database..."));
        return;
      }
      
      if (!dbRequest.responseSuccess) {
        Serial.println(F("Kết nối thất bại, không mở cổng"));
        entryGateWaitingToOpen = false;
        return;
      }

      if (availableSlots <= 0) {
        Serial.println(F("Parking FULL"));
        entryGateWaitingToOpen = false;
        return;
      }

      servoEntry.write(90);
      entryGateOpen = true;
      gateOpenTime = millis();
      entryGateWaitingToOpen = false;
      
      Serial.println(F("Chờ 5s"));
    }
  }
  
  if (exitGateWaitingToOpen) {
    if (currentTime - gateDelayStartTime >= GATE_DELAY_DURATION) {
      if (waitingForDatabaseResponse) {
        Serial.println(F("Chờ phản hồi từ Database"));
        return;
      }
      
      if (!dbRequest.responseSuccess) {
        Serial.println(F("Database thất bại"));
        exitGateWaitingToOpen = false;
        return;
      }
      servoExit.write(90);
      exitGateOpen = true;
      gateOpenTime = millis();
      exitGateWaitingToOpen = false;
      
      Serial.println(F("Exit gate opened after 5s delay"));
    }
  }
}

void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }
 
  char cardID[9];
  memset(cardID, 0, 9);
  for (byte i = 0; i < rfid.uid.size && i < 4; i++) {
    sprintf(cardID + i*2, "%02X", rfid.uid.uidByte[i]);
  }
  cardID[8] = '\0';

  Serial.print(F("RFID detected: "));
  Serial.println(cardID);

  rfid.PICC_HaltA();
 
  if (isAuthorizedCard(cardID)) {
    int vehicleIndex = findVehicleByID(cardID);
    
    if (vehicleIndex != -1) {
      handleExit(cardID);
    } else if (availableSlots > 0) {
      handleEntry(cardID);
    } else {
      Serial.println(F("Parking FULL"));
    }
  } else {
    Serial.print(F("Unauthorized RFID: "));
    Serial.println(cardID);
  }
}

void handleEntry(const char* cardID) {
  addVehicle(cardID);
  availableSlots = totalSlots - activeVehicles;
  if (availableSlots < 0) availableSlots = 0;
  
  sendRfidToDatabase("RFID_ACCESS", cardID, "ENTER", "ENTRY_GATE");
  sendHistory("RFID", cardID, "Entry");

  strncpy(dbRequest.rfidCode, cardID, 8);
  dbRequest.rfidCode[8] = '\0';
  strcpy(dbRequest.action, "ENTER");
  dbRequest.requestTime = millis();
  dbRequest.responseReceived = false;
  dbRequest.responseSuccess = false;
  waitingForDatabaseResponse = true;
  
  Serial.print(F("RFID Entry: "));
  Serial.print(cardID);
  Serial.println(F(" Cổng sẽ mở trong 5s "));
  
  entryGateWaitingToOpen = true;
  gateDelayStartTime = millis();
}

void handleExit(const char* cardID) {
  int vehicleIndex = findVehicleByID(cardID);
  removeVehicle(vehicleIndex);
  availableSlots = totalSlots - activeVehicles;
  if (availableSlots < 0) availableSlots = 0;

  sendRfidToDatabase("RFID_ACCESS", cardID, "EXIT", "EXIT_GATE");
  sendHistory("RFID", cardID, "Exit");

  strncpy(dbRequest.rfidCode, cardID, 8);
  dbRequest.rfidCode[8] = '\0';
  strcpy(dbRequest.action, "EXIT");
  dbRequest.requestTime = millis();
  dbRequest.responseReceived = false;
  dbRequest.responseSuccess = false;
  waitingForDatabaseResponse = true;
  
  Serial.print(F("RFID Exit"));
  Serial.print(cardID);
  Serial.println(F(" Exit gate will open in 5 seconds"));
  
  exitGateWaitingToOpen = true;
  gateDelayStartTime = millis();
}

void checkDatabaseResponseStatus() {
  if (!waitingForDatabaseResponse) return;
  
  unsigned long elapsed = millis() - dbRequest.requestTime;
  
  if (dbRequest.responseReceived) {
    if (dbRequest.responseSuccess) {
      Serial.print(F("Database confirmed: "));
      Serial.print(dbRequest.rfidCode);
      Serial.print(F(" "));
      Serial.println(dbRequest.action);
    } else {
      Serial.print(F("Database error for: "));
      Serial.print(dbRequest.rfidCode);
      Serial.println(F(" (vehicle processed)"));
    }
    waitingForDatabaseResponse = false;
    return;
  }
  
  if (elapsed > DATABASE_RESPONSE_TIMEOUT) {
    Serial.print(F("Database timeout for: "));
    Serial.print(dbRequest.rfidCode);
    Serial.println(F(" (vehicle processed)"));
    waitingForDatabaseResponse = false;     
  }
}

void openEntryGate() {
  if (availableSlots <= 0) {
    Serial.println(F("Parking FULL"));
    return;
  }

  servoEntry.write(90);
  entryGateOpen = true;
  gateOpenTime = millis();
  Serial.println(F("Entry gate opened"));
}

void openExitGate() {
  servoExit.write(90);
  exitGateOpen = true;
  gateOpenTime = millis();
  Serial.println(F("Exit gate opened"));
}

void closeAllGates() {
  bool gatesClosed = false;
 
  if (entryGateOpen) {
    servoEntry.write(0);
    entryGateOpen = false;
    Serial.println(F("Entry gate closed"));
    gatesClosed = true;
  }
 
  if (exitGateOpen) {
    servoExit.write(0);
    exitGateOpen = false;
    Serial.println(F("Exit gate closed"));
    gatesClosed = true;
  }

  entryGateWaitingToOpen = false;
  exitGateWaitingToOpen = false;
}

void checkGateTimeout() {
  unsigned long currentTime = millis();
 
  if (entryGateOpen && (currentTime - gateOpenTime > GATE_OPEN_DURATION)) {
    servoEntry.write(0);
    entryGateOpen = false;
    Serial.println(F("Hết thời gian, đóng cổng"));
  }
 
  if (exitGateOpen && (currentTime - gateOpenTime > GATE_OPEN_DURATION)) {
    servoExit.write(0);
    exitGateOpen = false;
    Serial.println(F("Hết thời gian, đóng cổng"));
  }
}

void sendRfidToDatabase(const char* event, const char* idCode, const char* action, const char* servoType) {
  if (databaseMode) {
    Serial.print(F("RFID_DATABASE:"));
    Serial.print(event);
    Serial.print(F(","));
    Serial.print(idCode);
    Serial.print(F(","));
    Serial.print(action);
    Serial.print(F(","));
    Serial.print(servoType);
    Serial.print(F(","));
    Serial.println(millis());
   
    lastDatabaseSync = millis();
  }
}

void sendHistory(const char* event, const char* idCode, const char* action) {
  Serial.print(F("HISTORY:"));
  Serial.print(event);
  Serial.print(F(","));
  Serial.print(idCode);
  Serial.print(F(","));
  Serial.print(action);
  Serial.print(F(","));
  Serial.println(entryGateOpen ? "ENTRY_GATE" : (exitGateOpen ? "EXIT_GATE" : "N/A"));
}

bool isAuthorizedCard(const char* cardID) {
  for (int i = 0; i < cardCount; i++) {
    if (strcmp(cardID, authorizedCards[i]) == 0) {
      return true;
    }
  }
  return false;
}

void sendDataToESP32() {
  StaticJsonDocument<300> doc;
  doc["available"] = availableSlots;
  doc["total"] = 4;
  JsonArray slots = doc.createNestedArray("slots");
  for (int i = 0; i < 4; i++) {
    slots.add(parkingSlots[i]);
  }
  doc["entryGateOpen"] = entryGateOpen;
  doc["exitGateOpen"] = exitGateOpen;
 
  doc["databaseMode"] = databaseMode;
  doc["lastDatabaseSync"] = lastDatabaseSync;
  doc["activeVehicles"] = 0;
 
  for (int i = 0; i < 4; i++) {
    if (parkedVehicles[i].isActive) {
      doc["activeVehicles"] = doc["activeVehicles"].as<int>() + 1;
    }
  }

  String jsonString;
  serializeJson(doc, jsonString);
  Serial.println(jsonString);
}

void loadAuthorizedData() {
  cardCount = EEPROM.read(0);
  if (cardCount > 10 || cardCount < 0) cardCount = 0;
 
  for (int i = 0; i < cardCount; i++) {
    for (int j = 0; j < 8; j++) {
      authorizedCards[i][j] = EEPROM.read(10 + i * 8 + j);
    }
    authorizedCards[i][8] = '\0';
  }
}

void saveAuthorizedData() {
  EEPROM.update(0, cardCount);
 
  for (int i = 0; i < cardCount; i++) {
    for (int j = 0; j < 8; j++) {
      EEPROM.update(10 + i * 8 + j, authorizedCards[i][j]);
    }
  }
}

void addAuthorizedCard(const char* cardID) {
  if (cardCount < 10) {
    if (isAuthorizedCard(cardID)) {
      Serial.print(F("Card already exists: "));
      Serial.println(cardID);
      return;
    }
   
    strncpy(authorizedCards[cardCount], cardID, 8);
    authorizedCards[cardCount][8] = '\0';
   
    for (int i = 0; i < 8; i++) {
      EEPROM.update(10 + cardCount * 8 + i, authorizedCards[cardCount][i]);
    }
   
    cardCount++;
    EEPROM.update(0, cardCount);
   
    Serial.print(F("Added card: "));
    Serial.println(cardID);
  } else {
    Serial.println(F("Card list full"));
  }
}

void removeAuthorizedCard(const char* cardID) {
  for (int i = 0; i < cardCount; i++) {
    if (strcmp(authorizedCards[i], cardID) == 0) {
      for (int j = i; j < cardCount - 1; j++) {
        strcpy(authorizedCards[j], authorizedCards[j + 1]);
        for (int k = 0; k < 8; k++) {
          EEPROM.update(10 + j * 8 + k, authorizedCards[j][k]);
        }
      }
      cardCount--;
      EEPROM.update(0, cardCount);
     
      Serial.print(F("Removed card: "));
      Serial.println(cardID);
      return;
    }
  }
  Serial.println(F("Card not found"));
}

void processSerialCommand(String command) {
  if (command.startsWith("MODE_CHANGE:")) {
    handleModeChange();
  } else if (command.startsWith("ADD_CARD:")) {
    handleAddCard(command);
  } else if (command.startsWith("REMOVE_CARD:")) {
    handleRemoveCard(command);
  } else if (command == "OPEN_ENTRY_GATE") {
    handleOpenEntryGate();
  } else if (command == "OPEN_EXIT_GATE") {
    handleOpenExitGate();
  } else if (command == "CLOSE_GATES") {
    handleCloseGates();
  } else if (command == "LIST_CARDS") {
    handleListCards();
  } else if (command == "LIST_VEHICLES") {
    handleListVehicles();
  } else if (command == "CLEAR_EEPROM") {
    handleClearEeprom();
  } else if (command == "DATABASE_STATUS") {
    handleDatabaseStatus();
  } else if (command.startsWith("DATABASE_RESPONSE:")) {
    handleDatabaseResponse(command);
  }
}

void handleModeChange() {
  Serial.println("MODE_CHANGED:RFID");
}

void handleAddCard(String command) {
  String cardID = command.substring(9);
  addAuthorizedCard(cardID.c_str());
}

void handleRemoveCard(String command) {
  String cardID = command.substring(12);
  removeAuthorizedCard(cardID.c_str());
}

void handleOpenEntryGate() {
  if (availableSlots <= 0) {
    Serial.println(F("Parking FULL"));
    return;
  }
  openEntryGate();
}

void handleOpenExitGate() {
  openExitGate();
}

void handleCloseGates() {
  closeAllGates();
}

void handleListCards() {
  Serial.print(F("Authorized Cards ("));
  Serial.print(cardCount);
  Serial.println(F("):"));
  for (int i = 0; i < cardCount; i++) {
    Serial.print(F("  "));
    Serial.print(i + 1);
    Serial.print(F(": "));
    Serial.println(authorizedCards[i]);
  }
}

void handleListVehicles() {
  Serial.println(F("Parked Vehicles:"));
  for (int i = 0; i < 4; i++) {
    if (parkedVehicles[i].isActive) {
      Serial.print(F("  Slot "));
      Serial.print(i + 1);
      Serial.print(F(": "));
      Serial.print(parkedVehicles[i].identifier);
      Serial.println(F(" (RFID)"));
    }
  }
}

void handleClearEeprom() {
  Serial.println(F("Clearing EEPROM and resetting..."));
  cardCount = 0;
  EEPROM.update(0, 0);
  Serial.println(F("EEPROM cleared. Restart to reload."));
}

void handleDatabaseStatus() {
  Serial.print(F("Database Mode: "));
  Serial.println(databaseMode ? F("ENABLED") : F("DISABLED"));
  Serial.print(F("Last Sync: "));
  Serial.println(lastDatabaseSync);
  Serial.print(F("Sync Interval: "));
  Serial.println(DATABASE_SYNC_INTERVAL);
}

void handleDatabaseResponse(String command) {
  String response = command.substring(18);
  Serial.print(F("Database response: "));
  Serial.println(response);

  if (waitingForDatabaseResponse) {
    dbRequest.responseReceived = true;
 
    if (response == "SUCCESS") {
      dbRequest.responseSuccess = true;
      Serial.println(F("Database sync successful"));
    } else if (response == "ERROR") {
      dbRequest.responseSuccess = false;
      Serial.println(F("Database sync failed - CANCELING GATE"));
      entryGateWaitingToOpen = false;
      exitGateWaitingToOpen = false;
    }
  }
}

int findVehicleByID(const char* identifier) {
  for (int i = 0; i < 4; i++) {
    if (parkedVehicles[i].isActive &&
        strcmp(parkedVehicles[i].identifier, identifier) == 0) {
      return i;
    }
  }
  return -1;
}

int findEmptySlot() {
  for (int i = 0; i < 4; i++) {
    if (!parkedVehicles[i].isActive) {
      return i;
    }
  }
  return -1;
}

void addVehicle(const char* identifier) {
  int slotIndex = findEmptySlot();
  if (slotIndex != -1) {
    parkedVehicles[slotIndex].isActive = true;
    parkedVehicles[slotIndex].slotNumber = slotIndex + 1;
    parkedVehicles[slotIndex].entryTime = millis();
    strncpy(parkedVehicles[slotIndex].identifier, identifier, 8);
    parkedVehicles[slotIndex].identifier[8] = '\0';

    activeVehicles++;
   
    Serial.print(F("Vehicle added to slot "));
    Serial.print(slotIndex + 1);
    Serial.print(F(": "));
    Serial.print(identifier);
    Serial.println(F(" (RFID)"));
   
    Serial.print(F("VEHICLE_ENTRY:"));
    Serial.print(identifier);
    Serial.print(F(","));
    Serial.print(slotIndex + 1);
    Serial.print(F(","));
    Serial.println(millis());
  }
}

void removeVehicle(int vehicleIndex) {
  Serial.print(F("Vehicle removed from slot "));
  Serial.print(vehicleIndex + 1);
  Serial.print(F(": "));
  Serial.print(parkedVehicles[vehicleIndex].identifier);
  Serial.println(F(" (RFID)"));
 
  Serial.print(F("VEHICLE_EXIT:"));
  Serial.print(parkedVehicles[vehicleIndex].identifier);
  Serial.print(F(","));
  Serial.print(vehicleIndex + 1);
  Serial.print(F(","));
  Serial.print(parkedVehicles[vehicleIndex].entryTime);
  Serial.print(F(","));
  Serial.println(millis());
 
  if (activeVehicles > 0) activeVehicles--;

  parkedVehicles[vehicleIndex].isActive = false;
  parkedVehicles[vehicleIndex].slotNumber = 0;
  parkedVehicles[vehicleIndex].entryTime = 0;
  memset(parkedVehicles[vehicleIndex].identifier, 0, 9);
}