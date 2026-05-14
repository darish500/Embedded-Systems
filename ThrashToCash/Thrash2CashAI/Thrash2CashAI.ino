/* esp32cam_classifier.ino
   ESP32-CAM inference sketch (Edge Impulse)
   - Listens on Serial (UART0) for "CAP" command (sent by master via Serial2)
   - When it receives CAP, it captures, runs classifier, then writes single-line JSON to Serial:
     {"prediction":"Bottle","confidence":0.87}\n
   Wiring: use AI-Thinker camera module wiring and ensure PSRAM enabled.
*/
// Camera frame buffer sizes
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS 320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS 240
#define EI_CAMERA_FRAME_BYTE_SIZE 3
#include <waste_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"
#include <ArduinoJson.h>

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

static bool debug_nn = false;
static bool is_initialised = false;
uint8_t *snapshot_buf;

// camera config (same as your previous file)
static camera_config_t camera_config = {
    .pin_pwdn = PWDN_GPIO_NUM,
    .pin_reset = RESET_GPIO_NUM,
    .pin_xclk = XCLK_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl = SIOC_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM,
    .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href = HREF_GPIO_NUM,
    .pin_pclk = PCLK_GPIO_NUM,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

// Forward declare camera helper functions (we use same functions as your EI example)
bool ei_camera_init(void);
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf);
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr);

// Serial buffer for commands
String cmdBuf = "";

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); } // wait for serial if needed
  Serial.println("ESP32-CAM Classifier (waiting for CAP command)");

  if (!ei_camera_init()) {
    Serial.println("Camera init failed.");
  } else {
    Serial.println("Camera init OK");
  }
}

// Main loop: wait for 'CAP' on Serial, then capture and infer
void loop() {
  // Read serial commands (from master over UART)
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      String line = cmdBuf;
      cmdBuf = "";
      line.trim();
      if (line.length() > 0) {
        if (line.equalsIgnoreCase("CAP")) {
          // run capture + inference
          capture_and_send_prediction();
        }
      }
    } else {
      cmdBuf += c;
      if (cmdBuf.length() > 200) cmdBuf = "";
    }
  }
  delay(10);
}

void capture_and_send_prediction() {
  Serial.println("CAP received - capturing");
  // allocate snapshot buffer (RGB888)
  snapshot_buf = (uint8_t*)malloc(EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);
  if (snapshot_buf == nullptr) {
    Serial.println("ERR: No mem for snapshot");
    return;
  }

  // run capture & get data into snapshot_buf
  if (!ei_camera_capture((size_t)EI_CLASSIFIER_INPUT_WIDTH, (size_t)EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf)) {
    Serial.println("Capture failed");
    free(snapshot_buf);
    return;
  }

  // run classifier
  ei::signal_t signal;
  signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data = &ei_camera_get_data;

  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
  if (err != EI_IMPULSE_OK) {
    Serial.printf("ERR: classifier failed (%d)\n", err);
    free(snapshot_buf);
    return;
  }

  // choose best label
  String bestLabel = "unknown";
  float bestScore = 0.0f;
  for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    float s = result.classification[i].value;
    if (s > bestScore) {
      bestScore = s;
      bestLabel = String(ei_classifier_inferencing_categories[i]);
    }
  }

  // Build JSON and send to master as single line
  DynamicJsonDocument doc(256);
  doc["prediction"] = bestLabel;
  doc["confidence"] = bestScore;
  String out; serializeJson(doc, out);
  Serial.println(out); // master listens on its Serial2 RX for this line

  free(snapshot_buf);
}

/* ---------- Camera helper functions (copied/adapted from your EI example) ---------- */

bool ei_camera_init(void) {
    if (is_initialised) return true;

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
      Serial.printf("Camera init failed with error 0x%x\n", err);
      return false;
    }

    sensor_t * s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1);
      s->set_brightness(s, 1);
      s->set_saturation(s, 0);
    }
    is_initialised = true;
    return true;
}

bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
    if (!is_initialised) {
        Serial.println("ERR: Camera not init");
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        return false;
    }

   bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, out_buf);
   esp_camera_fb_return(fb);
   if(!converted){
       Serial.println("Conversion failed");
       return false;
   }

    // if resize required, EI example handles crop_and_interpolate - Edge Impulse provides that
    if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS) || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) {
        ei::image::processing::crop_and_interpolate_rgb888(
            out_buf,
            EI_CAMERA_RAW_FRAME_BUFFER_COLS,
            EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
            out_buf,
            img_width,
            img_height);
    }

    return true;
}

static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr)
{
    // convert RGB888 snapshot_buf into float-packed pixel for EI
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;

    while (pixels_left != 0) {
        // pack to a single float as EI expects (uint24 -> float as their example used)
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16) + (snapshot_buf[pixel_ix + 1] << 8) + snapshot_buf[pixel_ix];
        out_ptr_ix++;
        pixel_ix += 3;
        pixels_left--;
    }
    return 0;
}
