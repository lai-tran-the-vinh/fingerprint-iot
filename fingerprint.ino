#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include <time.h>

// ================= CẤU HÌNH =================

const char WIFI_SSID[] = "Spring Orchid";
const char WIFI_PASSWORD[] = "motconvitxoera2caicanh";

const char BASE_URL[] =
  "http://louie-articulative-rema.ngrok-free.dev";

const char DEVICE_CODE[] = "FP002";
const char DEVICE_SECRET[] = "thietbiphongc206";

constexpr int BUTTON_PIN = 13;

constexpr int BUZZER_PIN = 19;

// Nếu còi kêu ngược (bình thường kêu, báo thì im),
// đổi HIGH thành LOW.
constexpr uint8_t BUZZER_ACTIVE_LEVEL = LOW;

constexpr int FINGER_RX = 4;
constexpr int FINGER_TX = 5;

// Mẫu AS608 native: 768 byte -> 1024 ký tự Base64.
constexpr size_t TEMPLATE_SIZE = 768;
constexpr size_t BASE64_SIZE = 4 * ((TEMPLATE_SIZE + 2) / 3);

// fingerFastSearch() của thư viện Adafruit chỉ tìm 163 slot đầu.
constexpr int MAX_SLOTS = 160;
// Số sinh viên tối đa lưu trong một lô điểm danh (NVS).
constexpr int MAX_ATTENDEES = 60;

const char BATCH_KEY[] = "attB2";
// true: gửi thêm "status": "present" | "late" cho mỗi lượt quét.
// Đặt false nếu backend tự tính late từ timestamp và từ chối field lạ.
const bool SEND_ATTENDANCE_STATUS = true;

HardwareSerial sensorSerial(2);
Adafruit_Fingerprint finger(&sensorSerial);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences preferences;

enum Mode {
  REGISTER_MODE,
  ATTENDANCE_MODE,
  POWER_OFF_MODE
};

Mode currentMode = REGISTER_MODE;

bool sensorReady = false;
bool storageReady = false;
bool uploadBlocked = false;

String registrationId;
String studentCode;
String pendingUpload;

time_t registrationExpiresAt = 0;

// 0: đặt lần 1, 1: chờ nhấc tay, 2: đặt lần 2
int registrationStage = 0;

unsigned long lastRegistrationPoll = 0;
unsigned long lastUploadAttempt = 0;
unsigned long lastSensorPoll = 0;
unsigned long lastAttendancePoll = 0;
unsigned long lastWiFiAttempt = 0;





// Mỗi tiếng kêu 80 ms, khoảng nghỉ giữa hai tiếng 100 ms.
bool buzzerEnabled = false;

void silenceBuzzer() {
  // Còi kích LOW nên HIGH là tắt.
  digitalWrite(
    BUZZER_PIN,
    BUZZER_ACTIVE_LEVEL == LOW ? HIGH : LOW);
}

void beep(uint8_t count) {
  if (!buzzerEnabled) {
    silenceBuzzer();
    return;
  }

  for (uint8_t i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LEVEL);
    delay(80);

    silenceBuzzer();

    if (i + 1 < count) {
      delay(100);
    }
  }
}

// Chỉ báo các lỗi xử lý.
// Không báo lỗi khi đang chờ Wi-Fi, chờ lịch hoặc chưa có lượt đăng ký.
bool isErrorMessage(const String &message) {
  return message.startsWith("Loi ") || message.startsWith("Thieu ") || message == "Khong xuat mau" || message == "Khong nhan ra" || message == "Khong co trong" || message == "Mau bi trung" || message == "Day bo nho" || message == "Chua gui duoc" || message == "Chua gui ket qua" || message == "Chua tai duoc mau" || message == "Chua co mau dung" || message == "Buoi khong co mau" || message == "Server tu choi" || message == "Server chua nhan" || message == "Hang doi loi" || message == "Response khac" || message == "Luot da het han" || message == "Het gio diem danh" || message == "Chua cau hinh";
}


// ================= LCD =================
void showMessage(String line1, String line2 = "") {
  static String previous1;
  static String previous2;

  // Thông báo không đổi thì không cập nhật LCD và không kêu lại.
  if (line1 == previous1 && line2 == previous2) {
    return;
  }

  previous1 = line1;
  previous2 = line2;

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));

  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));

  Serial.println(line1 + " | " + line2);

  // Đăng ký được server xác nhận hoặc đã lưu lượt điểm danh.
  if (
    line1 == "Dang ky server OK" || line1 == "Diem danh OK" || line1 == "Diem danh TRE" || line1 == "Da gui diem danh") {
    beep(2);
  } else if (isErrorMessage(line1)) {
    beep(3);
  }
}

void showMode() {
  if (currentMode == REGISTER_MODE) {
    showMessage("DANG KY", "Cho luot tren web");
  } else if (currentMode == ATTENDANCE_MODE) {
    showMessage("DIEM DANH", "Cho lay buoi hoc");
  } else {
    showMessage("DA TAT", "Bam nut de bat");
  }
}

// ================= THỜI GIAN =================

String currentTimestamp() {
  time_t now = time(nullptr);

  if (now < 1700000000) {
    return "";
  }

  struct tm utc;
  gmtime_r(&now, &utc);

  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", &utc);

  return String(buffer);
}

time_t parseUtcTimestamp(const String &value) {
  if (!value.endsWith("Z")) {
    return 0;
  }

  int year, month, day, hour, minute, second;

  if (sscanf(
        value.c_str(),
        "%d-%d-%dT%d:%d:%d",
        &year, &month, &day,
        &hour, &minute, &second)
      != 6) {
    return 0;
  }

  struct tm parsed = {};
  parsed.tm_year = year - 1900;
  parsed.tm_mon = month - 1;
  parsed.tm_mday = day;
  parsed.tm_hour = hour;
  parsed.tm_min = minute;
  parsed.tm_sec = second;
  parsed.tm_isdst = 0;

  // setup() cấu hình múi giờ UTC.
  return mktime(&parsed);
}

String scanTimestamp(int64_t seconds) {
  time_t value = static_cast<time_t>(seconds);
  struct tm utc = {};
  gmtime_r(&value, &utc);

  char text[25];
  strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
  return String(text);
}

// ================= GỌI API HTTP =================

// Che mẫu và thông tin bí mật trước khi in JSON.
void redactJson(JsonVariant value) {
  if (value.is<JsonObject>()) {
    for (JsonPair field : value.as<JsonObject>()) {
      String key = field.key().c_str();

      if (
        key == "templateData" || key == "templateBase64" || key == "authSecret" || key == "deviceSecret") {
        field.value().set("[REDACTED]");
      } else {
        redactJson(field.value());
      }
    }
  } else if (value.is<JsonArray>()) {
    for (JsonVariant item : value.as<JsonArray>()) {
      redactJson(item);
    }
  }
}

int callApi(
  const char *method,
  const char *endpoint,
  const String &body,
  String &response) {
  response = "";

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Chua ket noi WiFi");
    return -100;
  }

  if (currentTimestamp().isEmpty()) {
    Serial.println("Chua dong bo thoi gian");
    return -101;
  }

  if (
    String(DEVICE_SECRET).isEmpty() || String(DEVICE_SECRET) == "DIEN_DEVICE_SECRET") {
    showMessage("Chua cau hinh", "DEVICE_SECRET");
    return -102;
  }

  WiFiClient client;
  HTTPClient http;

  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  http.useHTTP10(false);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  String url = String(BASE_URL) + endpoint;

  if (!http.begin(client, url)) {
    Serial.println("Khong khoi tao duoc HTTP");
    return -103;
  }

  http.addHeader("X-Device-Code", DEVICE_CODE);
  http.addHeader("X-Device-Secret", DEVICE_SECRET);
  http.addHeader("ngrok-skip-browser-warning", "1");
  http.addHeader("Accept", "application/json");

  if (!body.isEmpty()) {
    http.addHeader("Content-Type", "application/json");
  }

  int status = http.sendRequest(method, body);

  Serial.printf("%s %s -> HTTP %d\n", method, endpoint, status);

  if (status <= 0) {
    Serial.print("Loi ket noi: ");
    Serial.println(HTTPClient::errorToString(status));
    Serial.print("WiFi status: ");
    Serial.println(WiFi.status());

    http.end();
    return status;
  }

  if (status >= 300 && status < 400) {
    Serial.println("Server chuyen huong. Kiem tra endpoint HTTP cua ngrok.");
    http.end();
    return status;
  }

  // Giới hạn response tối đa 140000 byte (xử lý được cả chunked).
  class ResponseBuffer : public Stream {
  public:
    String &output;
    bool failed = false;

    explicit ResponseBuffer(String &target)
      : output(target) {}

    size_t write(uint8_t value) override {
      return write(&value, 1);
    }

    size_t write(const uint8_t *data, size_t size) override {
      const size_t limit = 140000;

      if (size > limit - output.length()) {
        failed = true;
        return 0;
      }

      if (size == 0) {
        return 0;
      }

      if (!output.concat(
            reinterpret_cast<const char *>(data),
            static_cast<unsigned int>(size))) {
        failed = true;
        return 0;
      }

      return size;
    }

    int available() override {
      return 0;
    }
    int read() override {
      return -1;
    }
    int peek() override {
      return -1;
    }
    void flush() override {}
  };

  if (status != 204) {
    int length = http.getSize();

    if (length > 140000) {
      Serial.println("Response qua lon");
      http.end();
      return -104;
    }

    ResponseBuffer buffer(response);
    int received = http.writeToStream(&buffer);

    if (received < 0 || buffer.failed) {
      Serial.println("Loi doc response hoac khong du RAM");
      response = "";
      http.end();
      return -105;
    }

    if (length >= 0 && response.length() != static_cast<size_t>(length)) {
      Serial.println("Response chua nhan du");
      response = "";
      http.end();
      return -106;
    }
  }

  http.end();

  if (status >= 400) {
    JsonDocument error;

    if (!deserializeJson(error, response)) {
      redactJson(error.as<JsonVariant>());
      serializeJson(error, Serial);
      Serial.println();
    } else {
      Serial.println("Server tra loi khong phai JSON");
    }
  }

  return status;
}

// ================= NÚT CHUYỂN CHẾ ĐỘ =================

void resetRegistration() {
  registrationId = "";
  studentCode = "";
  registrationExpiresAt = 0;
  registrationStage = 0;
}

void handleButton() {
  // INPUT + dien tro pull-down ngoai: tha nut = LOW, nhan nut = HIGH.
  static int previousReading = LOW;
  static int stableState = LOW;
  static unsigned long changedAt = 0;

  int reading = digitalRead(BUTTON_PIN);

  if (reading != previousReading) {
    previousReading = reading;
    changedAt = millis();
  }

  if (millis() - changedAt >= 40 && reading != stableState) {
    stableState = reading;

    if (stableState == HIGH) {
      currentMode = static_cast<Mode>(
        (static_cast<int>(currentMode) + 1) % 3);

      // Hủy lượt quét chưa hoàn thành.
      // Giữ mẫu đã lưu trong hàng đợi gửi server.
      resetRegistration();

      lastRegistrationPoll = millis() - 5000;
      lastAttendancePoll = millis() - 60000;

      showMode();
      beep(1);
    }
  }
}

// ================= LẤY LƯỢT ĐĂNG KÝ =================

void fetchPendingRegistration() {
  String response;

  int status = callApi(
    "GET", "/devices/me/pending-registration", "", response);

  response.trim();

  if (
    status == 204 || (status == 200 && (response == "null" || response.isEmpty()))) {
    showMessage("DANG KY", "Cho luot tren web");
    return;
  }

  if (status != 200) {
    showMessage("Loi API dang ky", String(status));
    return;
  }

  JsonDocument document;

  if (
    deserializeJson(document, response) || !document["registrationId"].is<const char *>() || document["status"].as<String>() != "pending") {
    showMessage("Response khac", "Kiem tra backend");
    return;
  }

  time_t expires = parseUtcTimestamp(document["expiresAt"].as<String>());

  if (expires <= time(nullptr)) {
    showMessage("Luot da het han");
    return;
  }

  registrationId = document["registrationId"].as<String>();
  studentCode = document["studentCode"].as<String>();
  registrationExpiresAt = expires;
  registrationStage = 0;

  showMessage(studentCode, "Dat tay lan 1");
}

// ================= UART AS608: ĐỌC / GHI MẪU =================

bool readUartBytes(uint8_t *destination, size_t count, unsigned long started) {
  for (size_t index = 0; index < count; index++) {
    while (!sensorSerial.available()) {
      if (millis() - started > 5000) {
        return false;
      }

      delay(1);
    }

    destination[index] = sensorSerial.read();
  }

  return true;
}

// Xuất mẫu trong CharBuffer 1 (768 byte) thành Base64.
bool exportTemplateBase64(String &result) {
  static uint8_t data[TEMPLATE_SIZE];
  static unsigned char encoded[BASE64_SIZE + 1];

  result = "";
  Serial.println("[XUAT MAU] Bat dau doc mau 768 byte");

  uint8_t status = finger.getModel();
  if (status != FINGERPRINT_OK) {
    Serial.printf("[XUAT MAU] getModel loi: 0x%02X\n", status);
    return false;
  }

  size_t total = 0;
  unsigned packet = 0;

  while (true) {
    packet++;
    uint8_t header[9];
    uint8_t tail[2];
    unsigned long started = millis();

    if (!readUartBytes(header, sizeof(header), started)) {
      Serial.printf("[XUAT MAU] Timeout header goi %u, da doc %u byte\n",
                    packet, (unsigned)total);
      return false;
    }

    uint8_t type = header[6];
    uint16_t length = (uint16_t(header[7]) << 8) | header[8];

    if (header[0] != 0xEF || header[1] != 0x01 || (type != 0x02 && type != 0x08)) {
      Serial.printf("[XUAT MAU] Header sai o goi %u: %02X %02X, loai %02X\n",
                    packet, header[0], header[1], type);
      return false;
    }

    // length gồm dữ liệu và 2 byte checksum.
    if (length < 2 || length > 258) {
      Serial.printf("[XUAT MAU] Goi %u co do dai bat thuong: %u\n",
                    packet, (unsigned)length);
      return false;
    }

    size_t count = length - 2;
    if (count > sizeof(data) - total) {
      Serial.printf("[XUAT MAU] Mau lon hon 768 byte o goi %u\n", packet);
      return false;
    }

    if (!readUartBytes(data + total, count, started) || !readUartBytes(tail, sizeof(tail), started)) {
      Serial.printf("[XUAT MAU] Thieu du lieu/checksum o goi %u\n", packet);
      return false;
    }

    uint16_t sum = type + header[7] + header[8];
    for (size_t i = 0; i < count; i++) {
      sum += data[total + i];
    }

    uint16_t received = (uint16_t(tail[0]) << 8) | tail[1];
    if (sum != received) {
      Serial.printf("[XUAT MAU] Sai checksum o goi %u\n", packet);
      return false;
    }

    total += count;
    if (type == 0x08) break;  // Gói cuối
  }

  if (total != TEMPLATE_SIZE) {
    Serial.printf("[XUAT MAU] Nhan du goi cuoi nhung mau co %u byte\n",
                  (unsigned)total);
    return false;
  }

  size_t written = 0;
  int rc = mbedtls_base64_encode(
    encoded, sizeof(encoded), &written, data, total);

  if (rc != 0 || written != BASE64_SIZE) {
    Serial.printf("[XUAT MAU] Loi Base64: rc=%d, do dai=%u\n",
                  rc, (unsigned)written);
    return false;
  }

  encoded[written] = '\0';
  result = reinterpret_cast<const char *>(encoded);

  if (result.length() != written) {
    Serial.println("[XUAT MAU] Khong du RAM de giu chuoi Base64");
    return false;
  }

  Serial.printf("[XUAT MAU] OK: %u byte -> %u ky tu Base64\n",
                (unsigned)total, (unsigned)written);
  return true;
}

// Nạp 768 byte mẫu vào CharBuffer 1 bằng lệnh DownChar (0x09).
bool downloadTemplateToBuffer(const uint8_t *data, size_t total) {
  uint16_t chunk = finger.packet_len;
  if (chunk < 32 || chunk > 256) {
    chunk = 128;  // Mặc định của AS608
  }

  while (sensorSerial.available()) {
    sensorSerial.read();
  }

  // Lệnh DownChar, CharBuffer 1. Checksum = 0x01 + 0x0004 + 0x09 + 0x01.
  const uint8_t command[] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0x04, 0x09, 0x01, 0x00, 0x0F
  };
  sensorSerial.write(command, sizeof(command));
  sensorSerial.flush();

  uint8_t ack[12];
  if (!readUartBytes(ack, sizeof(ack), millis())) {
    Serial.println("[NAP MAU] Timeout cho ACK DownChar");
    return false;
  }

  if (ack[0] != 0xEF || ack[1] != 0x01 || ack[6] != 0x07 || ack[9] != 0x00) {
    Serial.printf("[NAP MAU] DownChar bi tu choi: 0x%02X\n", ack[9]);
    return false;
  }

  uint8_t packet[9 + 256 + 2];
  size_t offset = 0;

  while (offset < total) {
    size_t count = min<size_t>(chunk, total - offset);
    bool last = (offset + count >= total);
    uint8_t type = last ? 0x08 : 0x02;
    uint16_t length = count + 2;

    packet[0] = 0xEF;
    packet[1] = 0x01;
    packet[2] = packet[3] = packet[4] = packet[5] = 0xFF;
    packet[6] = type;
    packet[7] = length >> 8;
    packet[8] = length & 0xFF;
    memcpy(packet + 9, data + offset, count);

    uint16_t sum = type + packet[7] + packet[8];
    for (size_t i = 0; i < count; i++) {
      sum += data[offset + i];
    }

    packet[9 + count] = sum >> 8;
    packet[10 + count] = sum & 0xFF;

    sensorSerial.write(packet, 11 + count);
    sensorSerial.flush();
    delay(10);

    offset += count;
  }

  delay(30);
  return true;
}

// ================= GỬI ĐĂNG KÝ =================

void sendPendingRegistration() {
  if (pendingUpload.isEmpty() || uploadBlocked) {
    return;
  }

  String response;

  int status = callApi("POST", "/fingerprints/create", pendingUpload, response);

  JsonDocument request;
  JsonDocument acknowledgment;

  if (deserializeJson(request, pendingUpload)) {
    uploadBlocked = true;
    showMessage("Hang doi loi", "Go DISCARD");
    return;
  }

  bool confirmed =
    (status == 200 || status == 201) && !deserializeJson(acknowledgment, response) && acknowledgment["registrationId"].as<String>() == request["registrationId"].as<String>() && acknowledgment["status"].as<String>() == "completed";

  if (confirmed) {
    if (!preferences.remove("upload")) {
      showMessage("Loi xoa hang doi");
      return;
    }

    pendingUpload = "";
    uploadBlocked = false;
    resetRegistration();

    lastRegistrationPoll = millis();

    showMessage("Dang ky server OK");
    return;
  }

  if (status >= 400 && status < 500 && status != 408 && status != 429) {
    uploadBlocked = true;
    showMessage("Server tu choi", String(status));

    Serial.println("Da giu mau. Sua cau hinh/backend roi go RETRY.");
    Serial.println("Go DISCARD neu muon huy mau dang cho.");
  } else {
    showMessage("Chua gui duoc", "Se thu lai");
  }
}

// ================= XỬ LÝ ĐĂNG KÝ =================

void handleRegistration() {
  if (!sensorReady) {
    showMessage("Loi cam bien", "Kiem tra UART");
    return;
  }

  if (!storageReady) {
    showMessage("Loi luu tru NVS");
    return;
  }

  if (!pendingUpload.isEmpty()) {
    return;
  }

  if (registrationId.isEmpty()) {
    if (millis() - lastRegistrationPoll >= 5000) {
      lastRegistrationPoll = millis();
      fetchPendingRegistration();
    }

    return;
  }

  if (time(nullptr) >= registrationExpiresAt) {
    resetRegistration();
    showMessage("Luot da het han");
    return;
  }

  if (millis() - lastSensorPoll < 150) {
    return;
  }

  lastSensorPoll = millis();

  uint8_t status = finger.getImage();

  // Đã quét lần 1: chờ nhấc tay trước khi quét lần 2.
  if (registrationStage == 1) {
    if (status == FINGERPRINT_NOFINGER) {
      registrationStage = 2;
      showMessage(studentCode, "Dat tay lan 2");
    }

    return;
  }

  if (status == FINGERPRINT_NOFINGER) {
    return;
  }

  if (status != FINGERPRINT_OK) {
    showMessage("Loi doc anh", "Thu lai");
    return;
  }

  uint8_t bufferId = registrationStage == 0 ? 1 : 2;

  if (finger.image2Tz(bufferId) != FINGERPRINT_OK) {
    showMessage("Loi tao dac trung", "Thu lai");
    return;
  }

  if (registrationStage == 0) {
    registrationStage = 1;
    showMessage("Da quet lan 1", "Nhac ngon tay");
    return;
  }

  if (finger.createModel() != FINGERPRINT_OK) {
    resetRegistration();
    showMessage("Loi ghep mau", "Dang ky lai");
    return;
  }

  String templateBase64;

  if (!exportTemplateBase64(templateBase64)) {
    resetRegistration();
    showMessage("Khong xuat mau", "Xem Serial");
    Serial.println("Khong doc duoc mau UART; kiem tra lenh UpChar/profile.");
    return;
  }

  JsonDocument body;

  body["registrationId"] = registrationId;
  body["templateData"] = templateBase64;
  body["templateFormat"] = "AS608-NATIVE-768";
  body["algorithmVersion"] = "as608-native-v1";

  String payload;
  serializeJson(body, payload);

  // Lưu trước khi gửi để có thể thử lại sau khi khởi động.
  if (preferences.putString("upload", payload) != payload.length()) {
    resetRegistration();
    showMessage("Loi luu mau", "Chua gui server");
    return;
  }

  pendingUpload = payload;
  uploadBlocked = false;
  lastUploadAttempt = millis() - 10000;

  showMessage("Da tao mau", "Cho gui server");
}

// ================= ĐIỂM DANH =================

struct AttendanceEntry {
  char studentId[25];
  uint8_t late;  // 0: present, 1: late
  int64_t scannedAt;
};

struct AttendanceBatch {
  char sessionId[25];
  int64_t closesAt;  // attendanceCloseTime: sau mốc này là late
  int64_t endsAt;    // giữa buổi: sau mốc này hết điểm danh, gửi lên server
  uint16_t count;
  AttendanceEntry entries[MAX_ATTENDEES];
};

AttendanceBatch attendanceBatch = {};

// Slot cảm biến -> studentId (24 ký tự). Slot trống: chuỗi rỗng.
char rosterStudent[MAX_SLOTS + 1][25] = {};
bool ambiguousFinger[MAX_SLOTS + 1] = {};

bool batchRestored = false;
bool rosterLoaded = false;
bool waitForLift = false;
unsigned long lastFingerRead = 0;
unsigned long nextUploadTry = 0;

bool saveAttendanceBatch() {
  return preferences.putBytes(BATCH_KEY, &attendanceBatch,
                              sizeof(attendanceBatch))
         == sizeof(attendanceBatch);
}

bool restoreAttendanceBatch() {
  if (batchRestored) return true;

  if (!storageReady) {
    showMessage("Loi luu tru NVS");
    return false;
  }

  preferences.remove("attBatch");

  if (preferences.isKey(BATCH_KEY)) {
    if (preferences.getBytesLength(BATCH_KEY) != sizeof(attendanceBatch)) {
      Serial.println("Du lieu diem danh cu khac dinh dang, da xoa");
      preferences.remove(BATCH_KEY);
    } else if (preferences.getBytes(BATCH_KEY, &attendanceBatch,
                                    sizeof(attendanceBatch))
               != sizeof(attendanceBatch)) {
      showMessage("Loi doc diem danh", "Kiem tra NVS");
      return false;
    } else if (attendanceBatch.count > MAX_ATTENDEES) {
      attendanceBatch = {};
      preferences.remove(BATCH_KEY);
    }
  }

  batchRestored = true;
  return true;
}

void clearRoster() {
  for (int id = 0; id <= MAX_SLOTS; id++) {
    rosterStudent[id][0] = '\0';
    ambiguousFinger[id] = false;
  }
  rosterLoaded = false;
  waitForLift = false;
}

// Xóa thư viện trên cảm biến, tải mẫu của buổi học và nạp vào từng slot.
bool loadSessionRoster(const String &sessionId) {
  JsonDocument request;
  request["sessionId"] = sessionId;

  String body, response;
  serializeJson(request, body);

  int status = callApi(
    "POST", "/attendance/device/session-templates", body, response);

  if (status != 200 && status != 201) {
    Serial.printf("Tai mau cho buoi %s: %d\n", sessionId.c_str(), status);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response) || doc["sessionId"].as<String>() != sessionId || !doc["templates"].is<JsonArray>()) {
    showMessage("Loi JSON mau");
    return false;
  }

  response = "";  // Giải phóng RAM trước khi nạp mẫu.

  // Chỉ dùng cảm biến để so khớp. Mẫu gốc nằm ở server.
  if (finger.emptyDatabase() != FINGERPRINT_OK) {
    showMessage("Loi xoa cam bien", "Xem Serial");
    return false;
  }

  clearRoster();

  static uint8_t bytes[TEMPLATE_SIZE + 4];
  int nextSlot = 1;
  int total = doc["templates"].as<JsonArray>().size();
  int index = 0;
  int usable = 0;

  for (JsonObject item : doc["templates"].as<JsonArray>()) {
    index++;
    showMessage("Nap mau cam bien", String(index) + "/" + String(total));

    const char *data = item["templateData"] | "";
    const char *student = item["studentId"] | "";

    if (strlen(student) != 24 || strlen(data) != BASE64_SIZE) {
      Serial.printf("Bo qua mau %d: sai dinh dang\n", index);
      continue;
    }

    size_t size = 0;
    if (mbedtls_base64_decode(
          bytes, sizeof(bytes), &size,
          reinterpret_cast<const unsigned char *>(data),
          strlen(data))
          != 0
        || size != TEMPLATE_SIZE) {
      Serial.printf("Bo qua mau %d: Base64/kich thuoc sai\n", index);
      continue;
    }

    if (nextSlot > MAX_SLOTS || nextSlot > finger.capacity) {
      Serial.println("Cam bien het slot, bo qua cac mau con lai");
      break;
    }

    if (!downloadTemplateToBuffer(bytes, size)) {
      Serial.printf("Bo qua mau %d: nap buffer loi\n", index);
      continue;
    }

    // Kiểm tra mẫu này đã có trong cảm biến chưa.
    bool ambiguous = false;
    if (finger.fingerFastSearch() == FINGERPRINT_OK) {
      int match = finger.fingerID;

      if (match >= 1 && match <= MAX_SLOTS) {
        if (strcmp(rosterStudent[match], student) == 0) {
          continue;  // Trùng mẫu của cùng sinh viên, không cần lưu thêm.
        }

        // Cùng một vân tay nhưng thuộc hai sinh viên khác nhau.
        ambiguousFinger[match] = true;
        ambiguous = true;
        Serial.printf("Mau %d trung voi slot %d (khac sinh vien)\n",
                      index, match);
      }
    }

    if (finger.storeModel(nextSlot) != FINGERPRINT_OK) {
      Serial.printf("Bo qua mau %d: luu slot %d loi\n", index, nextSlot);
      continue;
    }

    strlcpy(rosterStudent[nextSlot], student, sizeof(rosterStudent[nextSlot]));
    ambiguousFinger[nextSlot] = ambiguous;
    nextSlot++;
    usable++;
  }

  Serial.printf("Buoi %s: nap %d/%d mau\n", sessionId.c_str(), usable, total);

  if (!usable) {
    showMessage("Chua co mau dung", "Kiem tra lop/mau");
    return false;
  }

  rosterLoaded = true;
  showMessage("DIEM DANH", "Dat ngon tay");
  return true;
}

// Thêm code vô đây
// ================= LUU LICH CA NGAY =================

JsonDocument cachedSessions;
String cachedDate;

bool dailyScheduleReady = false;
bool scheduleRequestStarted = false;
unsigned long lastScheduleRequest = 0;

// Thời gian chờ thử lại khi tải mẫu thất bại.
bool rosterRetryPending = false;
unsigned long lastRosterRetry = 0;

// Ngày Việt Nam; đồng hồ hệ thống vẫn giữ UTC.
String getScheduleDate() {
  time_t now = time(nullptr);

  if (now < 1700000000) return "";

  time_t vietnamTime = now + 7 * 60 * 60;

  struct tm date = {};
  gmtime_r(&vietnamTime, &date);

  char buffer[11];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", &date);

  return String(buffer);
}

bool ensureDailySchedule() {
  String today = getScheduleDate();

  if (today.isEmpty()) return false;

  // Đã lấy lịch thành công hôm nay: không gọi server nữa.
  if (dailyScheduleReady && cachedDate == today) {
    return true;
  }

  // Sang ngày mới.
  if (cachedDate != today) {
    cachedSessions.clear();
    cachedDate = today;

    dailyScheduleReady = false;
    scheduleRequestStarted = false;
    rosterRetryPending = false;
  }

  // Nếu gọi lỗi, thử lại sau 15 giây.
  if (
    scheduleRequestStarted && millis() - lastScheduleRequest < 15000) {
    return false;
  }

  scheduleRequestStarted = true;
  lastScheduleRequest = millis();

  JsonDocument request;
  request["timestamp"] = currentTimestamp();

  String body, response;
  serializeJson(request, body);

  Serial.printf(
    "[LICH] Lay lich ngay %s\n",
    today.c_str());

  int status = callApi(
    "POST",
    "/attendance/device/sessions",
    body,
    response);

  if (status != 200 && status != 201) {
    Serial.printf("[LICH] Loi HTTP %d\n", status);
    return false;
  }

  cachedSessions.clear();

  DeserializationError error =
    deserializeJson(cachedSessions, response);

  if (
    error || !cachedSessions["sessions"].is<JsonArray>()) {
    Serial.println("[LICH] JSON khong hop le");
    cachedSessions.clear();
    return false;
  }

  dailyScheduleReady = true;

  Serial.printf(
    "[LICH] Da luu %u buoi. Khong goi lai lich dinh ky.\n",
    (unsigned)cachedSessions["sessions"].as<JsonArray>().size());

  return true;
}

//




void findOpenSession() {
  // Có buổi được khôi phục từ NVS: nạp lại đúng buổi đó.
  if (attendanceBatch.sessionId[0]) {
    if (rosterLoaded) return;

    if (
      rosterRetryPending && millis() - lastRosterRetry < 60000) {
      return;
    }

    lastRosterRetry = millis();

    bool loaded = loadSessionRoster(
      String(attendanceBatch.sessionId));

    rosterRetryPending = !loaded;
    return;
  }

  if (!ensureDailySchedule()) {
    showMessage("Chua lay duoc lich", "Dang thu lai");
    return;
  }

  time_t now = time(nullptr);

  JsonObject selected;
  time_t selectedOpen = 0;
  time_t selectedClose = 0;
  time_t selectedEnd = 0;
  time_t nextOpen = 0;

  bool waitingForRetry = false;

  for (
    JsonObject session :
    cachedSessions["sessions"].as<JsonArray>()) {
    String id = session["_id"] | "";

    if (id.length() != 24) continue;

    const char *startText = session["startTime"] | "";
    const char *openText = session["attendanceOpenTime"];
    const char *closeText = session["attendanceCloseTime"];
    const char *endText = session["endTime"];

    if (!openText) openText = startText;
    if (!closeText) closeText = endText ? endText : "";
    if (!endText) endText = closeText;

    time_t starts = parseUtcTimestamp(String(startText));
    time_t opens = parseUtcTimestamp(String(openText));
    time_t closes = parseUtcTimestamp(String(closeText));
    time_t ends = parseUtcTimestamp(String(endText));

    // Giữ nguyên quy tắc kết thúc ở giữa buổi của bạn.
    time_t cutoff =
      (starts && ends > starts)
        ? starts + (ends - starts) / 2
        : ends;

    if (
      !opens || !closes || !cutoff || cutoff <= opens) {
      continue;
    }

    // Buổi đã hết thời gian nhận điểm danh.
    if (now >= cutoff) continue;

    // Buổi chưa mở: nhớ giờ mở gần nhất.
    if (now < opens) {
      if (!nextOpen || opens < nextOpen) {
        nextOpen = opens;
      }
      continue;
    }

    // Mỗi buổi tải mẫu lỗi được thử lại sau 60 giây.
    // Đây không phải gọi lại API lấy lịch.
    int64_t retryAt =
      session["_retryAt"] | (int64_t)0;

    if (retryAt > (int64_t)now) {
      waitingForRetry = true;
      continue;
    }

    // Không phụ thuộc thứ tự danh sách server trả về.
    if (selected.isNull() || opens < selectedOpen) {
      selected = session;
      selectedOpen = opens;
      selectedClose = closes;
      selectedEnd = cutoff;
    }
  }

  if (selected.isNull()) {
    // Không ghi đè lỗi nạp mẫu trong thời gian chờ thử lại.
    if (waitingForRetry) return;

    if (nextOpen) {
      time_t vietnamTime = nextOpen + 7 * 60 * 60;

      struct tm clock = {};
      gmtime_r(&vietnamTime, &clock);

      char text[17];
      strftime(
        text,
        sizeof(text),
        "Mo luc %H:%M",
        &clock);

      showMessage("Cho buoi ke tiep", String(text));
    } else {
      showMessage("Khong con buoi", "trong lich da lay");
    }

    return;
  }

  String id = selected["_id"].as<String>();

  Serial.printf(
    "[LICH] Den gio buoi %s. Bat dau tai mau.\n",
    id.c_str());

  if (!loadSessionRoster(id)) {
    selected["_retryAt"] = (int64_t)time(nullptr) + 60;

    Serial.printf(
      "[MAU] Buoi %s tai loi. Thu lai sau 60 giay.\n",
      id.c_str());

    // Giữ nguyên thông báo lỗi của loadSessionRoster().
    return;
  }

  // Kiểm tra lại vì nạp mẫu có thể mất thời gian.
  if (time(nullptr) >= selectedEnd) {
    clearRoster();
    showMessage("Buoi da het gio", "Cho buoi tiep");
    return;
  }

  id.toCharArray(
    attendanceBatch.sessionId,
    sizeof(attendanceBatch.sessionId));

  attendanceBatch.closesAt = selectedClose;
  attendanceBatch.endsAt = selectedEnd;

  rosterRetryPending = false;

  Serial.printf(
    "[LICH] San sang diem danh. Ket thuc UTC: %s\n",
    scanTimestamp(selectedEnd).c_str());
}




void scanAttendanceFinger() {
  if (millis() - lastFingerRead < 150) return;
  lastFingerRead = millis();

  uint8_t status = finger.getImage();

  if (status == FINGERPRINT_NOFINGER) {
    if (waitForLift) {
      waitForLift = false;
      showMessage("DIEM DANH", "Dat ngon tay");
    }
    return;
  }

  if (waitForLift) return;

  if (status != FINGERPRINT_OK) {
    showMessage("Loi doc van tay", "Thu lai");
    return;
  }

  waitForLift = true;

  if (finger.image2Tz(1) != FINGERPRINT_OK || finger.fingerFastSearch() != FINGERPRINT_OK) {
    showMessage("Khong nhan ra", "Nhac ngon tay");
    return;
  }

  int id = finger.fingerID;
  if (id < 1 || id > MAX_SLOTS || !rosterStudent[id][0]) {
    showMessage("Khong co trong", "danh sach buoi");
    return;
  }

  if (ambiguousFinger[id]) {
    showMessage("Mau bi trung", "Dang ky lai");
    return;
  }

  const char *student = rosterStudent[id];

  // Một sinh viên chỉ được ghi nhận một lần trong buổi,
  // kể cả khi đã đăng ký nhiều ngón.
  for (int i = 0; i < attendanceBatch.count; i++) {
    if (strcmp(attendanceBatch.entries[i].studentId, student) == 0) {
      showMessage("Da diem danh", "Nhac ngon tay");
      return;
    }
  }

  if (attendanceBatch.count >= MAX_ATTENDEES) {
    showMessage("Day bo nho", "Bao giao vien");
    return;
  }

  time_t scanned = time(nullptr);
  if (scanned >= attendanceBatch.endsAt) {
    showMessage("Het gio diem danh");
    return;
  }

  bool late = scanned >= attendanceBatch.closesAt;

  AttendanceEntry &entry = attendanceBatch.entries[attendanceBatch.count];
  strlcpy(entry.studentId, student, sizeof(entry.studentId));
  entry.scannedAt = scanned;
  entry.late = late ? 1 : 0;
  attendanceBatch.count++;

  if (!saveAttendanceBatch()) {
    attendanceBatch.count--;
    showMessage("Loi luu luot quet", "Nhac tay thu lai");
    return;
  }

  Serial.printf("Da ghi nhan slot %d, studentId=%s, %s, luc %s\n",
                id, student, late ? "LATE" : "present",
                scanTimestamp(scanned).c_str());

  showMessage(late ? "Diem danh TRE" : "Diem danh OK",
              "Tong: " + String(attendanceBatch.count));
}

bool serverConfirmedStudent(JsonDocument &doc, const char *student) {
  for (const char *id : doc["checkedInStudentIds"].as<JsonArray>()) {
    if (id && strcmp(id, student) == 0) return true;
  }

  for (const char *id : doc["alreadyCheckedInStudentIds"].as<JsonArray>()) {
    if (id && strcmp(id, student) == 0) return true;
  }

  return false;
}

void finishAttendanceSession() {
  int count = attendanceBatch.count;

  if (count > 0) {
    // Thử lại sau 15 giây nếu mất mạng hoặc server chưa xác nhận.
    if (nextUploadTry && static_cast<int32_t>(millis() - nextUploadTry) < 0) return;
    nextUploadTry = millis() + 15000;

    // Cảm biến chỉ giữ ID slot, không giữ chuỗi template. Backend đối chiếu
    // chuỗi template đã gửi xuống, nên tải lại danh sách mẫu của buổi và gửi
    // đúng chuỗi của từng sinh viên đã quét (như luồng mock trước đây).
    String body, response;
    {
      JsonDocument req;
      req["sessionId"] = String(attendanceBatch.sessionId);
      String reqBody;
      serializeJson(req, reqBody);

      int s = callApi(
        "POST", "/attendance/device/session-templates", reqBody, response);

      if (s != 200 && s != 201) {
        showMessage("Loi tai mau", String(s));
        return;
      }
    }

    {
      JsonDocument roster;
      if (deserializeJson(roster, response) || !roster["templates"].is<JsonArray>()) {
        showMessage("Loi JSON mau");
        return;
      }
      response = "";

      if (!body.reserve(count * (BASE64_SIZE + 90) + 80)) {
        showMessage("Thieu RAM", "Chua gui ket qua");
        return;
      }

      body = "{\"sessionId\":\"";
      body += attendanceBatch.sessionId;
      body += "\",\"templates\":[";

      for (int i = 0; i < count; i++) {
        const char *found = nullptr;

        for (JsonObject item : roster["templates"].as<JsonArray>()) {
          const char *sid = item["studentId"] | "";
          if (strcmp(sid, attendanceBatch.entries[i].studentId) == 0) {
            found = item["templateData"] | static_cast<const char *>(nullptr);
            break;
          }
        }

        if (!found || strlen(found) != BASE64_SIZE) {
          showMessage("Thieu mau server", "Xem Serial");
          Serial.printf("Khong tim thay mau cho studentId=%s\n",
                        attendanceBatch.entries[i].studentId);
          return;
        }

        if (i) body += ",";
        body += "{\"template\":\"";
        body += found;
        body += "\",\"timestamp\":\"";
        body += scanTimestamp(attendanceBatch.entries[i].scannedAt);
        body += "\"";

        if (SEND_ATTENDANCE_STATUS) {
          body += ",\"status\":\"";
          body += attendanceBatch.entries[i].late ? "late" : "present";
          body += "\"";
        }

        body += "}";
      }

      body += "]}";
    }

    int status = callApi(
      "POST", "/attendance/device/check-in", body, response);

    JsonDocument ack;
    if ((status != 200 && status != 201) || deserializeJson(ack, response) || ack["sessionId"].as<String>() != attendanceBatch.sessionId) {
      showMessage("Chua gui ket qua", String(status));
      return;
    }

    for (int i = 0; i < count; i++) {
      if (!serverConfirmedStudent(ack, attendanceBatch.entries[i].studentId)) {
        showMessage("Server chua nhan", "du sinh vien");
        return;
      }
    }

    if (!preferences.remove(BATCH_KEY)) {
      showMessage("Loi xoa hang doi");
      return;
    }
  }

  attendanceBatch = {};
  clearRoster();
  nextUploadTry = 0;
  lastAttendancePoll = millis();

  showMessage(count ? "Da gui diem danh" : "Ket thuc buoi",
              "Tong: " + String(count));
}

void handleAttendance() {
  if (!sensorReady) {
    showMessage("Loi cam bien", "Kiem tra UART");
    return;
  }

  if (!restoreAttendanceBatch()) return;

  if (attendanceBatch.sessionId[0] && time(nullptr) >= attendanceBatch.endsAt) {
    finishAttendanceSession();
    return;
  }

  if (!rosterLoaded) {
    if (millis() - lastAttendancePoll < 1000) return;
    lastAttendancePoll = millis();
    findOpenSession();
    return;
  }

  scanAttendanceFinger();
}

// Dùng cho lệnh SESSION / TEMPLATES qua Serial.
void refreshSession() {
  if (!sensorReady || !restoreAttendanceBatch()) return;

  if (
    attendanceBatch.sessionId[0] && time(nullptr) >= attendanceBatch.endsAt) {
    finishAttendanceSession();
    return;
  }

  rosterRetryPending = false;

  if (attendanceBatch.sessionId[0]) {
    bool loaded = loadSessionRoster(
      String(attendanceBatch.sessionId));

    rosterRetryPending = !loaded;
    lastRosterRetry = millis();
    return;
  }

  // SESSION / TEMPLATES khi chưa có buổi:
  // yêu cầu tải lại lịch sau khi sửa lịch trên web.
  cachedSessions.clear();
  dailyScheduleReady = false;
  scheduleRequestStarted = false;

  lastAttendancePoll = millis() - 1000;
  findOpenSession();
}

// ================= LỆNH SERIAL =================

void handleSerial() {
  static String input;

  while (Serial.available()) {
    char character = Serial.read();

    if (character == '\r') {
      continue;
    }

    if (character != '\n') {
      if (input.length() < 40) {
        input += character;
      }

      continue;
    }

    String command = input;
    input = "";

    command.trim();
    command.toUpperCase();

    if (command.isEmpty()) {
      continue;
    }

    if (currentMode == POWER_OFF_MODE) {
      Serial.println("May dang tat. Bam nut de bat.");
      continue;
    }

    if (command == "RETRY") {
      uploadBlocked = false;
      lastUploadAttempt = millis() - 10000;

      Serial.println("Se thu gui lai khi o che do DANG KY.");
    } else if (command == "DISCARD") {
      if (pendingUpload.isEmpty() || preferences.remove("upload")) {
        pendingUpload = "";
        uploadBlocked = false;
        resetRegistration();

        showMessage("Da huy mau cho");
      } else {
        showMessage("Loi xoa hang doi");
      }
    } else if (
      currentMode == ATTENDANCE_MODE && (command == "SESSION" || command == "TEMPLATES")) {
      refreshSession();
    } else {
      Serial.println("Lenh: SESSION | TEMPLATES | RETRY | DISCARD");
    }
  }
}

// ================= SETUP =================

void setup() {
  buzzerEnabled = false;

  // Đặt mức tắt trước khi bật chế độ OUTPUT.
  silenceBuzzer();
  pinMode(BUZZER_PIN, OUTPUT);
  silenceBuzzer();

  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT);  // Dung dien tro pull-down 10k ngoai

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  storageReady = preferences.begin("fp-api", false);

  if (storageReady) {
    pendingUpload = preferences.getString("upload", "");
  }

  sensorSerial.setRxBufferSize(2048);
  sensorSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);

  delay(1000);

  sensorReady = finger.verifyPassword();

  if (sensorReady) {
    finger.getParameters();  // Lấy capacity và packet_len của cảm biến.
    Serial.printf("Cam bien: OK (capacity=%u, packet=%u)\n",
                  (unsigned)finger.capacity, (unsigned)finger.packet_len);
  } else {
    Serial.println("Cam bien: LOI - kiem tra TX/RX va baud");
  }

  Serial.println(storageReady ? "NVS: OK" : "NVS: LOI");

  Serial.print("API: ");
  Serial.println(BASE_URL);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  setenv("TZ", "UTC0", 1);
  tzset();

  configTime(0, 0, "pool.ntp.org", "time.google.com");

  currentMode = REGISTER_MODE;

  lastRegistrationPoll = millis() - 5000;
  lastAttendancePoll = millis() - 60000;
  lastWiFiAttempt = millis();

  showMode();
}

// ================= LOOP =================

void loop() {

  // Chỉ cho còi hoạt động khi đã có Wi-Fi và giờ chuẩn.
  buzzerEnabled =
    WiFi.status() == WL_CONNECTED && !currentTimestamp().isEmpty();

  if (!buzzerEnabled) {
    silenceBuzzer();
  }
  handleButton();

  // Tắt logic xử lý, vẫn đọc nút để bật lại.
  if (currentMode == POWER_OFF_MODE) {
    while (Serial.available()) {
      Serial.read();
    }

    delay(5);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiAttempt >= 15000) {
      lastWiFiAttempt = millis();
      WiFi.reconnect();
    }

    showMessage(
      currentMode == REGISTER_MODE ? "DANG KY" : "DIEM DANH",
      "Cho WiFi");

    delay(10);
    return;
  }

  if (currentTimestamp().isEmpty()) {
    showMessage(
      currentMode == REGISTER_MODE ? "DANG KY" : "DIEM DANH",
      "Dong bo gio");

    delay(10);
    return;
  }

  handleSerial();

  if (
    String(DEVICE_SECRET).isEmpty() || String(DEVICE_SECRET) == "DIEN_DEVICE_SECRET") {
    showMessage("Chua cau hinh", "DEVICE_SECRET");
    delay(10);
    return;
  }


  // Lấy lịch ngay khi máy có mạng và giờ chuẩn,
  // kể cả lúc đang ở chế độ đăng ký.
  // Nếu đã lấy thành công hôm nay, hàm trả về ngay.
  ensureDailySchedule();

  if (currentMode == REGISTER_MODE) {
    if (
      !pendingUpload.isEmpty() && !uploadBlocked && millis() - lastUploadAttempt >= 10000) {
      lastUploadAttempt = millis();
      sendPendingRegistration();
    }

    handleRegistration();
  } else if (currentMode == ATTENDANCE_MODE) {
    handleAttendance();
  }

  delay(5);
}
