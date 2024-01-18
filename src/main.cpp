#include "esp_camera.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESP32Servo.h>

#include "camera_pins.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "fd_forward.h"
#include "fb_gfx.h"

#include <sstream>

#define FACE_COLOR_GREEN 0x0000FF00
const int numReadings = 5;
int readings[numReadings]; // the readings from the analog input
int readIndex = 0;         // the index of the current reading
int total = 0;             // the running total
int average_face_size = 0; // the average
int face_distance;

static inline mtmn_config_t app_mtmn_config()
{
  mtmn_config_t mtmn_config = {0};
  mtmn_config.type = FAST;
  mtmn_config.min_face = 60; // 80 default
  mtmn_config.pyramid = 0.707;
  mtmn_config.pyramid_times = 4;
  mtmn_config.p_threshold.score = 0.6;
  mtmn_config.p_threshold.nms = 0.7;
  mtmn_config.p_threshold.candidate_number = 20;
  mtmn_config.r_threshold.score = 0.7;
  mtmn_config.r_threshold.nms = 0.7;
  mtmn_config.r_threshold.candidate_number = 10;
  mtmn_config.o_threshold.score = 0.7;
  mtmn_config.o_threshold.nms = 0.7;
  mtmn_config.o_threshold.candidate_number = 1;
  return mtmn_config;
}
mtmn_config_t mtmn_config = app_mtmn_config();

const char *ssid = "ESP32_AP";

WiFiManager manager;

Servo dummyServo1;
Servo dummyServo2;
Servo panServo;
Servo tiltServo;

AsyncWebServer server(80);
AsyncWebSocket servoInput("/ServoInput");
AsyncWebSocket camera("/Camera");
uint32_t cameraClientId = 0;

const char htmlHomePage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
    <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
    <style>
        .noselect {
            -webkit-touch-callout: none;
            -webkit-user-select: none;
            -khtml-user-select: none;
            -moz-user-select: none;
            -ms-user-select: none;
            user-select: none;
        }

        body {
            text-align: center;
            font-family: "Trebuchet MS", Arial;
            margin-left: auto;
            margin-right: auto;
        }

        .slidecontainer {
            width: 100%;
        }

        .slider {
            -webkit-appearance: none;
            appearance: none;
            width: 100%;
            height: 10px;
            border-radius: 5px;
            background: #d3d3d3;
            outline: none;
            opacity: 0.7;
            -webkit-transition: .2s;
            transition: opacity .2s;
            vertical-align: middle;
        }

        .slider:hover {
            opacity: 1;
        }
    </style>

</head>

<body class="noselect" align="center" style="background-color:white">

    <h2 style="color: teal;text-align:center;">Wi-Fi Camera Control</h2>
    <table id="mainTable" style="width:400px;margin:auto;table-layout:fixed" CELLSPACING=10>
        <tr>
            <div style="display: flex; justify-content: center;">
                <img id="cameraImage" src="" style="width:400px;height:250px"></td>
            </div>
        </tr>
        <tr />
        <tr />
        <tr>
            <td style="text-align:left"><b>Pan:</b></td>
            <td colspan=2>
                <div class="slidecontainer">
                    <input type="range" min="0" max="180" value="90" class="slider" id="Pan"
                        oninput='sendButtonInput("Pan", value)'>
                </div>
            </td>
        </tr>
        <tr />
        <tr />
        <tr>
            <td style="text-align:left"><b>Tilt:</b></td>
            <td colspan=2>
                <div class="slidecontainer">
                    <input type="range" min="0" max="180" value="90" class="slider" id="Tilt"
                        oninput='sendButtonInput("Tilt", value)'>
                </div>
            </td>
        </tr>
        <tr />
        <tr />
        <tr>
            <td style="text-align:left"><b>Light:</b></td>
            <td colspan=2>
                <div class="slidecontainer">
                    <input type="range" min="0" max="255" value="0" class="slider" id="Light"
                        oninput=' sendButtonInput("Light",value)'>
                </div>
            </td>
        </tr>
    </table>

    <script>
        var websocketCameraUrl = `ws://${window.location.hostname}/Camera`;
        var websocketServoInputUrl = `ws://${window.location.hostname}/ServoInput`;
        var websocketCamera;
        var websocketServoInput;

        function initCameraWebSocket() {
            console.log('Opening camera connection...');
            websocketCamera = new WebSocket(websocketCameraUrl);
            websocketCamera.binaryType = 'blob';
            websocketCamera.onopen = event => { };
            websocketCamera.onclose = event => setTimeout(initCameraWebSocket, 2000);
            websocketCamera.onmessage = event => {
                var image = document.getElementById("cameraImage");
                image.src = URL.createObjectURL(event.data);

                // const reader = new FileReader();
                // reader.readAsDataURL(event.data);
                // reader.onloadend = () => {console.log(reader.result);}
            }
        }

        function initServoInputWebSocket() {
            console.log('Connecting led light and servo');
            websocketServoInput = new WebSocket(websocketServoInputUrl);
            websocketServoInput.onopen = event => {
                sendButtonInput("Pan", document.getElementById("Pan").value);
                sendButtonInput("Tilt", document.getElementById("Tilt").value);
                sendButtonInput("Light", document.getElementById("Light").value);
            }
            websocketServoInput.onclose = event => setTimeout(initServoInputWebSocket, 2500);
            websocketServoInput.onmessage = event => { };
        }

        function initWebSocket() {
            initCameraWebSocket();
            initServoInputWebSocket();
        }

        function sendButtonInput(key, value) {
            var data = key + "," + value;
            console.log(data);
            websocketServoInput.send(data);
        }

        window.onload = initWebSocket;
    </script>
</body></html>
)rawliteral";

void handleRoot(AsyncWebServerRequest *request)
{
  request->send_P(200, "text/html", htmlHomePage);
}

void handleNotFound(AsyncWebServerRequest *request)
{
  request->send(404, "text/plain", "File Not Found");
}

void onCameraWebsocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
    Serial.printf("Camera client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    cameraClientId = client->id();
    break;
  case WS_EVT_DISCONNECT:
    Serial.printf("Camera client #%u disconnected\n", client->id());
    cameraClientId = 0;
    break;
  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  default:
    break;
  }
}

void onServoInputWebsocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
    Serial.printf("Input Servo client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    break;
  case WS_EVT_DISCONNECT:
    Serial.printf("Input Servo client #%u disconnected\n", client->id());
    ledcWrite(PWMLightChannel, 0);
    break;
  case WS_EVT_DATA:
    AwsFrameInfo *info;
    info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
    {
      std::string inputData = "";
      inputData.assign((const char *)data, len);
      Serial.printf("Key,Value = [%s]\n", inputData.c_str());
      std::istringstream ss(inputData);
      std::string key, value;
      std::getline(ss, key, ',');
      std::getline(ss, value, ',');
      if (value != "")
      {
        int valueInt = atoi(value.c_str());
        if (key == "Light")
        {
          ledcWrite(PWMLightChannel, valueInt);
        }
        else if (key == "Pan")
        {
          panServo.write(valueInt);
          delay(6);
        }
        else if (key == "Tilt")
        {
          tiltServo.write(valueInt);
          delay(6);
        }
      }
    }
    break;
  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  default:
    break;
  }
}

void setupPinModes();
void setupCamera();
void sendCameraImage();
void setupWiFi();

void setup()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);

  setupPinModes();
  setupWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.onNotFound(handleNotFound);

  camera.onEvent(onCameraWebsocketEvent);
  server.addHandler(&camera);

  servoInput.onEvent(onServoInputWebsocketEvent);
  server.addHandler(&servoInput);

  server.begin();
  setupCamera();
}

void loop()
{
  camera.cleanupClients();
  servoInput.cleanupClients();
  sendCameraImage();
}

void setupPinModes()
{
  // Setup servo
  dummyServo1.attach(DUMMY_SERVO1_PIN);
  dummyServo2.attach(DUMMY_SERVO2_PIN);

  panServo.attach(PAN_PIN);
  tiltServo.attach(TILT_PIN);

  panServo.write(90);
  tiltServo.write(90);

  // Setup flash
  ledcSetup(PWMLightChannel, 1000, 8);
  pinMode(LIGHT_PIN, OUTPUT);
  ledcAttachPin(LIGHT_PIN, PWMLightChannel);
}

void setupWiFi()
{
  // WiFi connection
  WiFi.mode(WIFI_STA);
  bool connection = manager.autoConnect(ssid);

  if (!connection)
  {
    Serial.println("Failed to connect");
    manager.resetSettings();
  }

  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi connected!");
  Serial.print("http://");
  Serial.println(WiFi.localIP());
}

void setupCamera()
{
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size = FRAMESIZE_SVGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("Camera init failed with error 0x%x", err);
  }
  Serial.println("Camera init success");

  sensor_t *sensor = esp_camera_sensor_get();
  // sensor->set_framesize(sensor, FRAMESIZE_QVGA);
}

static void detect_face(camera_fb_t *frame);
static void draw_face_boxes(dl_matrix3du_t *image_matrix, box_array_t *boxes);

void sendCameraImage()
{
  if (cameraClientId == 0)
    return;

  camera_fb_t *frame = esp_camera_fb_get();

  if (!frame)
  {
    Serial.println("Frame buffer could not be acquired.");
    return;
  }

  // detect_face(frame);

  camera.binary(cameraClientId, frame->buf, frame->len);
  esp_camera_fb_return(frame);

  // Wait for message to be delivered
  while (true)
  {
    AsyncWebSocketClient *clientPointer = camera.client(cameraClientId);
    if (!clientPointer || !clientPointer->queueIsFull())
      break;
    delay(1);
  }
}

static void detect_face(camera_fb_t *frame)
{
  dl_matrix3du_t *image_matrix = NULL;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;

  _jpg_buf_len = frame->len;
  _jpg_buf = frame->buf;
  image_matrix = dl_matrix3du_alloc(1, frame->width, frame->height, 3);

  fmt2rgb888(frame->buf, frame->len, frame->format, image_matrix->item);

  box_array_t *net_boxes = face_detect(image_matrix, &mtmn_config);
  Serial.println(net_boxes->len);

  if (net_boxes)
  {
    Serial.println("Detected!");
    draw_face_boxes(image_matrix, net_boxes);
    free(net_boxes->score);
    free(net_boxes->box);
    free(net_boxes->landmark);
    free(net_boxes);
  }

  fmt2jpg(image_matrix->item, frame->width * frame->height * 3, frame->width, frame->height, PIXFORMAT_RGB888, 90, &_jpg_buf, &_jpg_buf_len);
  camera.binary(cameraClientId, (const char *)_jpg_buf, _jpg_buf_len);

  esp_camera_fb_return(frame);
  frame = NULL;
  dl_matrix3du_free(image_matrix);
  free(_jpg_buf);
  _jpg_buf = NULL;
}

static void draw_face_boxes(dl_matrix3du_t *image_matrix, box_array_t *boxes)
{
  int x, y, w, h, i, half_width, half_height;
  uint32_t color = FACE_COLOR_GREEN;
  fb_data_t frame;
  frame.width = image_matrix->w;
  frame.height = image_matrix->h;
  frame.data = image_matrix->item;
  frame.bytes_per_pixel = 3;
  frame.format = FB_BGR888;

  for (i = 0; i < boxes->len; i++)
  {
    x = ((int)boxes->box[i].box_p[0]);
    w = (int)boxes->box[i].box_p[2] - x + 1;
    half_width = w / 2;
    int face_center_pan = x + half_width;

    y = ((int)boxes->box[i].box_p[1]);
    h = (int)boxes->box[i].box_p[3] - y + 1;
    half_height = h / 2;
    int face_center_tilt = y + half_height;

    Serial.println(h);

    fb_gfx_drawFastHLine(&frame, x, y, w, color);
    fb_gfx_drawFastHLine(&frame, x, y, w, color);
    fb_gfx_drawFastHLine(&frame, x, y + h - 1, w, color);
    fb_gfx_drawFastVLine(&frame, x, y, h, color);
    fb_gfx_drawFastVLine(&frame, x + w - 1, y, h, color);
  }
}