#include <SD.h>
#include <soc/soc_caps.h>
#include <math.h>

#include <AudioOutput.h>
#include <AudioFileSourceFS.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>

#include <M5UnitLCD.h>
#include <M5UnitOLED.h>
#include <M5Unified.h>
#include <atomic>
#include <vector>
#include <algorithm>

#include "AudioOutputM5Speaker.h"
#include "AudioUI.h"

static constexpr uint8_t m5spk_virtual_channel = 0;
static constexpr const char* mp3_dir = "/";
static std::vector<String> filename;

static fs::FS* card = nullptr; // &SD_MMC or &SD, whichever mountCard() opened
static AudioFileSourceFS* file = nullptr;
static AudioOutputM5Speaker out(&M5.Speaker, m5spk_virtual_channel);
static AudioGeneratorMP3 mp3;
static AudioGeneratorWAV wav;
static AudioGenerator* generator = nullptr; // &mp3 or &wav while a file is open
static AudioFileSourceID3* id3 = nullptr; // only for *.mp3
static AudioUI ui;
static size_t fileindex = 0; 
static std::atomic<int> track_request { 0 };

static fs::FS* mountCard(void)
{
 SPI.begin(1, 0, 8, 2);
  
  return SD.begin(2, SPI, 25000000) ? (fs::FS*)&SD : nullptr;
}

static void scanFiles(void)
{
  filename.clear();
  auto dir = card->open(mp3_dir);
  if (!dir) { return; }
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile())
  {
    bool is_dir = f.isDirectory();
    String name = f.name(); // a bare name or a full path, depending on the core version
    f.close();
    String lower = name;
    lower.toLowerCase();
    if (is_dir || !(lower.endsWith(".mp3") || lower.endsWith(".wav")) || lower.startsWith(".")) { continue; }
    filename.push_back(name.startsWith("/") ? name : String(mp3_dir) + "/" + name);
  }
  dir.close();
  std::sort(filename.begin(), filename.end());
}

/// ID3 tags: title and artist go on the display, everything is logged.
static void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string)
{
  (void)cbData;
  (void)isUnicode;
  if (string[0] == 0 || strcmp(type, "eof") == 0) { return; }
  if (strcmp(type, "Title") == 0)          { ui.setMeta(1, string, type); }
  else if (strcmp(type, "Performer") == 0) { ui.setMeta(2, string, type); }
  else { M5_LOGI("%s: %s", type, string); }
}

static void stop(void)
{
  if (generator == nullptr) return;
  out.stop();
  generator->stop();
  generator = nullptr;
  if (id3 != nullptr)
  {
    id3->RegisterMetadataCB(nullptr, nullptr);
    id3->close();
    delete id3;
    id3 = nullptr;
  }
  file->close();
}

static bool play(const char* fname)
{
  stop();
  ui.clearMeta(); // nothing from the previous file stays on screen
  const char* base = strrchr(fname, '/');
  ui.setMeta(0, base ? base + 1 : fname, "file");
  bool ok = file->open(fname);
  if (ok)
  {
    String lower = fname;
    lower.toLowerCase();
    if (lower.endsWith(".wav"))
    {
      generator = &wav;
      ok = generator->begin(file, &out);
    }
    else
    {
      id3 = new AudioFileSourceID3(file);
      id3->RegisterMetadataCB(MDCallback, nullptr);
      id3->open(fname);
      generator = &mp3;
      ok = generator->begin(id3, &out);
    }
  }
  if (!ok)
  {
    stop();
    ui.setMeta(1, "(cannot play)", "error");
  }
  return ok;
}

static void playNext(int step)
{
  int n = filename.size();
  for (int tries = 0; tries < n; ++tries)
  {
    int index = ((int)fileindex + step) % n; // step may have accumulated beyond +-n
    if (index < 0) { index += n; }
    fileindex = index;
    if (play(filename[fileindex].c_str())) { return; }
    step = 1;
  }
}

static void decodeTask(void*)
{
  for (;;)
  {
    int step = track_request.exchange(0);
    if (step) { playNext(step); }
    if (generator != nullptr && generator->isRunning())
    {
      if (!generator->loop()) { playNext(1); } // end of file: on to the next one
    }
    else
    {
      M5.delay(1);
    }
  }
}

void setup(void)
{
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Log.setLogLevel(m5::log_target_serial, ESP_LOG_INFO); // file names and tags go to the serial log too

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = 144000; // default:64000 (64kHz)  e.g. 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
    M5.Speaker.config(spk_cfg);
  }

  M5.Speaker.begin();

  ui.setup(&M5.Display, "MP3 player");
  out.setup();
  out.setMonitor([](void* arg, const int16_t* stereo, size_t frames) { ((AudioUI*)arg)->feed(stereo, frames); }, &ui);

  while (nullptr == (card = mountCard()))
  {
    M5.delay(500);
  }
  file = new AudioFileSourceFS(*card);

  scanFiles();
  if (filename.empty())
  {
    ui.setMeta(1, String("no *.mp3 / *.wav in " + String(mp3_dir)).c_str());
    for (;;) { ui.loop(); M5.delay(10); } // keep drawing so the message shows (and scrolls on a short display)
  }
  playNext(0);
  if (pdPASS != xTaskCreatePinnedToCore(decodeTask, "decodeTask", 4096, nullptr, 2, nullptr, (SOC_CPU_CORES_NUM > 1) ? 1 : 0))
  {
    stop();
    ui.setMeta(1, "(cannot start the decode task)", "error");
    for (;;) { ui.loop(); M5.delay(10); }
  }
}

void loop(void)
{
  ui.loop();

  {
    static int prev_frame;
    int frame;
    do
    {
      M5.delay(1);
    } while (prev_frame == (frame = millis() >> 3)); /// 8 msec cycle wait
    prev_frame = frame;
  }

  M5.update();
  auto in = ui.update();
  if (in.track) { track_request += in.track; } // handled by the decode task
}
