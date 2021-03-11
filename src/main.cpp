//SPI SD CARD LIBRARY WARNING:
//CHIP SELECT FEATURES MANUALLY ADJUSTED IN SDFAT LIB (in SdSpiDriver.h). MUST USE LIB INCLUDED WITH REPO!!!

#define BOOTLOADER_VERSION "1.0"
#define FIRMWARE_VERSION "1.1"

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include "SdFat.h"
#include "U8g2lib.h"
#include "menu.h"
#include "menuIO/serialIO.h"
#include "plugin/SdFatMenu.h"
#include "menuIO/u8g2Out.h"
#include "YM2612.h"
#include "SN76489.h"
#include "Adafruit_ZeroTimer.h"
#include "logo.h"
#include "SpinSleep.h"
#include "SerialUtils.h"
#include "clocks.h"
#include "Bounce2.h"
#include "LinkedList.h"
#include "logo.h"
#include "decompress.h"
#include "FreeMem.h"

extern "C" {
  #include "trngFunctions.h" //True random number generation
}

#include "VGMEngine.h"

//Debug variables
#define DEBUG true //Set this to true for a detailed printout of the header data & any errored command bytes
#define DEBUG_LED A4
bool commandFailed = false;
uint8_t failedCmd = 0x00;
#define DISABLE_IRQ_TEST false

//Structs
enum FileStrategy {FIRST_START, NEXT, PREV, RND, REQUEST};
enum PlayMode {LOOP, PAUSE, SHUFFLE_ALL, IN_ORDER, SHUFFLE_DIR};
enum MenuState {IN_MENU, IN_VGM};

//Prototypes
void setup();
void loop();
void handleSerialIn();
void tick44k1();
void set44k1ISR();
void stop44k1();
void handleButtons();
void prepareChips();
void readGD3();
void drawOLEDTrackInfo();
bool startTrack(FileStrategy fileStrategy, String request = "");
bool vgmVerify();
void showIndexProgressOLED();
void findTracksOnRoot();
uint8_t VgmCommandLength(uint8_t Command);
uint32_t countFilesInDir(String dir); 
uint32_t getFileIndexInDir(String dir, String fname, uint32_t dirSize = 0);
String getFilePathFromCurrentDirFileIndex();
uint32_t readFile32(FatFile *f);
String GetPathFromManifest(uint32_t index);
String readStringUntil(String & in, char terminator);
void getDirIndices(String dir, String fname);
void getDirIndicesFromFullPath(String fullPath);
uint32_t freeKB();
void CreateManifest(bool createNew = false);
bool VerifyManifest();
result filePick(eventMask event, navNode& _nav, prompt &item);
result doCreateManifest(); //Required for UI
result onMenuIdle(menuOut& o, idleEvent e);
result onChoosePlaymode(eventMask e,navNode& _nav,prompt& item);
void removeMeta();
void clearRandomHistory();
void IRQ_ISR();
void IRQSelfTest();
void alertErrorState();

const uint32_t MANIFEST_MAGIC = 0x12345678;
#define MANIFEST_FILE_NAME ".MANIFEST"
#define MANIFEST_DIR "_SYS/"
#define MANIFEST_PATH MANIFEST_DIR MANIFEST_FILE_NAME
#define TMP_DECOMPRESSION_FILE_PATH "/" MANIFEST_DIR "/TMP.vgm"

//SD & File Streaming
SdFat SD;
static File file, manifest;
#define MAX_FILE_NAME_SIZE 128
char fileName[MAX_FILE_NAME_SIZE];
uint32_t numberOfFiles = 0;
uint32_t numberOfDirectories = 0;
String currentDir = "";
uint32_t currentDirFileCount = 0;
uint32_t currentDirFileIndex = 0;
uint32_t currentDirDirCount = 0; //How many directories are IN the current directory, mainly used for root only.
uint32_t rootObjectCount = 0; //How many objects (files and folders) are in the root directory

uint32_t dirStartIndex, dirEndIndex, dirCurIndex = 0; //Range inside the manifest file where the current files in the current dir are
LinkedList<int> rootTrackIndicies = LinkedList<int>(); //Valid VGM/VGZ file indicies on the root are stored here
LinkedList<int> randFileList = LinkedList<int>(); //Used to keep a history of file indices when in shuffle mode to allow for forward/backwards playback controls
LinkedList<String> randDirList = LinkedList<String>();
int randIndex = 0; 
#define MAX_RAND_HISTORY_SIZE 25

Adafruit_ZeroTimer timer1 = Adafruit_ZeroTimer(3);
Adafruit_ZeroTimer timer2 = Adafruit_ZeroTimer(4);
//SAMDTimer ITimer(TIMER_TC3);
//SAMDTimer dacStreamTimer(MAX_TIMER);

Bus bus(0, 1, 8, 9, 11, 10, 12, 13);

YM2612 opn(&bus, 3, NULL, 6, 4, 5, 7);
SN76489 sn(&bus, 2);
const uint8_t IRQTestPin = 51;

//VGM Variables
uint16_t loopCount = 0;
uint8_t maxLoops = 3;
bool fetching = false;
volatile bool ready = false;
bool samplePlaying = false;
PlayMode playMode = IN_ORDER;
bool doParse = false;

//UI & Menu
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);
#define fontName u8g2_font_6x12_mr     
#define fontX 6
#define fontY 14
#define offsetX 0
#define offsetY 0
#define U8_Width 128
#define U8_Height 64
const colorDef<uint8_t> colors[6] MEMMODE={
  {{0,0},{0,1,1}},//bgColor
  {{1,1},{1,0,0}},//fgColor
  {{1,1},{1,0,0}},//valColor
  {{1,1},{1,0,0}},//unitColor
  {{0,1},{0,0,1}},//cursorColor
  {{1,1},{1,0,0}},//titleColor
};

using namespace Menu;
SDMenuT<CachedFSO<SdFat,32>> filePickMenu(SD,"Music","/",filePick,enterEvent);

CHOOSE(playMode,modeMenu,"Mode:",onChoosePlaymode,exitEvent,noStyle
  ,VALUE("Loop",PlayMode::LOOP,doNothing,noEvent)
  ,VALUE("In Order",PlayMode::IN_ORDER,doNothing,noEvent)
  ,VALUE("Shuffle All",PlayMode::SHUFFLE_ALL,doNothing,noEvent)
  ,VALUE("Shuffle Dir",PlayMode::SHUFFLE_DIR,doNothing,noEvent)
);

TOGGLE(VGMEngine.loopOneOffs, setLoopOneOff, "Loop One-offs: ", doNothing, noEvent, noStyle
    ,VALUE("YES",true,doNothing,noEvent)
    ,VALUE("NO",false,doNothing,noEvent)
);

#define MAX_DEPTH 2
MENU(mainMenu,"Main menu",doNothing,noEvent,wrapStyle
  ,SUBMENU(filePickMenu)
  ,SUBMENU(modeMenu)
  ,FIELD(VGMEngine.maxLoops,"Loops: ","",1,255,1,10,doNothing,noEvent,noStyle)
  ,SUBMENU(setLoopOneOff)
  ,OP("Rebuild Manifest",doCreateManifest,enterEvent)
  //,EXIT("<Back")
);

MENU_OUTPUTS(out,MAX_DEPTH
  ,U8G2_OUT(u8g2,colors,fontX,fontY,offsetX,offsetY,{0,0,U8_Width/fontX,U8_Height/fontY})
  //,U8X8_OUT(u8x8,{0,0,16,8}) //0,0 Char# x y
  //,SERIAL_OUT(Serial)
  ,NONE//must have 2 items at least
);
serialIn serial(Serial);
NAVROOT(nav,mainMenu,MAX_DEPTH,serial,out);

MenuState menuState = IN_MENU;
#define MENU_TIMEOUT_IN_SECONDS 10

bool isOledOn = true;

//Buttons
const int prev_btn = 47;    //PORT_PB00;
const int down_btn = 48;    //PORT_PB01;
const int next_btn = 49;    //PORT_PB04;
const int up_btn = 50;  //PORT_PB05;
const int select_btn = 19;  //PORT_PB09;
Bounce buttons[5];

//Counters
uint32_t bufferPos = 0;
uint32_t cmdPos = 0;
uint16_t waitSamples = 0;
uint32_t pcmBufferPosition = 0;

void TC3_Handler() 
{
  Adafruit_ZeroTimer::timerHandler(3);
}

void TC4_Handler() 
{
  Adafruit_ZeroTimer::timerHandler(4);
}

void tick44k1(void) //44.1KHz tick
{
  VGMEngine.tick44k1();
}

void stop44k1()
{
  timer1.enable(false);
}

void set44k1ISR()
{
  //44.1KHz target, actual 44,117Hz
  const uint16_t compare = 1088;
  tc_clock_prescaler prescaler = TC_CLOCK_PRESCALER_DIV1;
  timer1.enable(false);
  timer1.configure(prescaler,       // prescaler
        TC_COUNTER_SIZE_16BIT,       // bit width of timer/counter
        TC_WAVE_GENERATION_MATCH_PWM // frequency or PWM mode
        );
  timer1.setCompare(0, compare);
  timer1.setCallback(true, TC_CALLBACK_CC_CHANNEL0, tick44k1);
  timer1.enable(true);
}

void tickDacStream()
{
  VGMEngine.tickDacStream();
}

void stopDacStreamTimer()
{
  timer2.enable(false);
}

void setDacStreamTimer(uint32_t frequency)
{
  float prescale = 1;
  float period = (1000000.0f / frequency);
  float compare = (48000000L / (prescale/(period/1000000L)))-1;

  tc_clock_prescaler prescaler = TC_CLOCK_PRESCALER_DIV1;
  timer2.enable(false);
  timer2.configure(prescaler,       // prescaler
        TC_COUNTER_SIZE_16BIT,       // bit width of timer/counter
        TC_WAVE_GENERATION_MATCH_PWM // frequency or PWM mode
        );
  timer2.setCompare(0, compare);
  timer2.setCallback(true, TC_CALLBACK_CC_CHANNEL0, tickDacStream);
  timer2.enable(true);
  //Serial.println(frequency);
}

void setup()
{
  //COM
  Wire.begin();
  Wire.setClock(600000L);
  SPI.begin();
  Serial.begin(115200);

  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_4MA);
  si5351.drive_strength(SI5351_CLK1, SI5351_DRIVE_4MA);
  si5351.set_freq(NTSC_YMCLK*100ULL, SI5351_CLK0); //CLK0 YM
  si5351.set_freq(NTSC_COLORBURST*100ULL, SI5351_CLK1); //CLK1 PSG - VALUES IN 0.01Hz

  //Timers
  VGMEngine.setDacStreamTimer = &setDacStreamTimer;
  VGMEngine.stopDacStreamTimer = &stopDacStreamTimer;

  //RNG
  trngInit();
  randomSeed(trngGetRandomNumber());

  //DEBUG
  //pinMode(DEBUG_LED, INPUT_PULLUP);
  pinMode(IRQTestPin, INPUT);

  resetSleepSpin();

  //Button configs
  for(uint8_t i = 0; i<5; i++)
  {
    buttons[i] = Bounce();
    buttons[i].interval(25);
  }
  buttons[0].attach(next_btn, INPUT_PULLUP);
  buttons[1].attach(prev_btn, INPUT_PULLUP);
  buttons[2].attach(up_btn, INPUT_PULLUP);
  buttons[3].attach(select_btn, INPUT_PULLUP);
  buttons[4].attach(down_btn, INPUT_PULLUP);

  //Set Chips
  VGMEngine.ym2612 = &opn;
  VGMEngine.sn76489 = &sn;

  opn.reset();
  sn.reset();

  //u8g2 OLED
  u8g2.begin();
  u8g2.setBusClock(600000);
  u8g2.setFont(fontName);

  #if !DISABLE_IRQ_TEST
  IRQSelfTest(); //Test the OPN via it's timers to make sure it's legit
  #endif

  //OLED title logo
  u8g2.drawXBM(0,0, logo_width, logo_height, logo);
  const String bootloaderFWVersion = "BL: " + String(BOOTLOADER_VERSION) + "  FW: " + String(FIRMWARE_VERSION) + " 2021";
  u8g2.drawStr(0, 64, bootloaderFWVersion.c_str());
  u8g2.sendBuffer();
  delay(3000);
  u8g2.clearDisplay();

  //SD
  REG_PORT_DIRSET0 = PORT_PA15; //Set PA15 to output
  if(!SD.begin(PORT_PA15, SPI_FULL_SPEED))
  {
    Serial.println("SD Mount Failed!");
    u8g2.clearBuffer();
    u8g2.drawStr(0,16,"SD Mount");
    u8g2.drawStr(0,32,"failed!");
    u8g2.sendBuffer();
    while(true){Serial.println("SD MOUNT FAILED"); delay(1000);}
  }
  filePickMenu.begin();
  nav.useAccel=true;
  options->invertFieldKeys = true;

  Serial.flush();

  //Prepare files
  removeMeta();
  rootObjectCount = countFilesInDir("/");
  findTracksOnRoot();
  bool manifestError = false;
  do
  {
    CreateManifest(manifestError);
    manifestError = !VerifyManifest();
  }while(manifestError);

  nav.timeOut=0xFFFFFFFF; //This might be a slight issue if you decide to run your player for 50,000 days straight :/
  nav.idleTask = onMenuIdle;
}

uint16_t IRQtestCounter = 0;
void IRQ_ISR()
{
  opn.setYMTimerA(0);
  IRQtestCounter++;
}

void IRQSelfTest() //Use the IRQ pin and the built-in OPN timers to determine if a real chip has been installed (correctly)
{
  Serial.println("Testing IRQ...");
  attachInterrupt(digitalPinToInterrupt(IRQTestPin), IRQ_ISR, FALLING);
  opn.setYMTimerA(0);
  unsigned long s = millis();
  while(true)
  {
    if(millis()-s > 1000) //Warn user to power device down as their chip may be fake and cause damage to the rest of the system
    {
      //fail
      //https://ww1.microchip.com/downloads/en/DeviceDoc/SAM_D5x_E5x_Family_Data_Sheet_DS60001507G.pdf page 805, perhaps tri-state all of the pins?
      Serial.println("DANGER!!! YM2612/YM3438 NOT DETECTED! POWER DOWN AND REMOVE IC!");
      u8g2.clear();
      u8g2.drawStr(0, 10, "DANGER! UNPLUG UNIT!");
      u8g2.drawStr(0, 20, "YM2612/YM3438");
      u8g2.drawStr(0, 30, "NOT DETECTED!");
      u8g2.drawStr(0, 40, "Your chip may be fake");
      u8g2.drawStr(0, 51, "or seated incorrectly.");
      u8g2.drawStr(0, 62, "Halting...");
      u8g2.sendBuffer();
      while(true);
    }
    if(IRQtestCounter >= 10) //The IRQ pin has pulsed 10 times, it's probably a real chip
    {
      break;
    }
  };
  Serial.println("YM IRQ OK!");
  detachInterrupt(digitalPinToInterrupt(IRQTestPin));
  opn.clearYMTimerA();
}

void drawOLEDTrackInfo()
{
  if(isOledOn)
  {
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);
    u8g2.setPowerSave(0);
    u8g2.clearDisplay();
    u8g2.setFont(u8g2_font_helvR08_tr);
    u8g2.sendBuffer();
    if(wstrlen(VGMEngine.gd3.enTrackName) != 1) //No track name was seen in the GD3
    {
      u8g2.drawStr(0,10, widetochar(VGMEngine.gd3.enTrackName));
      u8g2.drawStr(0,20, widetochar(VGMEngine.gd3.enGameName));
      u8g2.drawStr(0,30, widetochar(VGMEngine.gd3.releaseDate));
      u8g2.drawStr(0,40, widetochar(VGMEngine.gd3.enSystemName));
    }
    else
    {
      u8g2.drawStr(0,10, "No GD3 Data");
    }

    char* cstr;
    String playmodeStatus;
    if(playMode == LOOP)
      playmodeStatus = "LOOP";
    else if(playMode == SHUFFLE_ALL)
      playmodeStatus = "SHUFFLE ALL";
    else if(playMode == SHUFFLE_DIR)
      playmodeStatus = "SHUFFLE DIR";
    else if(playMode == IN_ORDER)
    {
      String fileNumberData = "Track: " + String(dirCurIndex-dirStartIndex) + "/" + String(dirEndIndex-dirStartIndex); 
      cstr = &fileNumberData[0u];
      u8g2.drawStr(0,50, cstr);
      playmodeStatus = "IN ORDER";
    }
    cstr = &playmodeStatus[0u];
    u8g2.drawStr(0, 60, cstr);
    u8g2.sendBuffer();
  }
  else
  {
    u8g2.clearDisplay();
    u8g2.setPowerSave(1);
    u8g2.sendBuffer();
  }
  u8g2.setFont(fontName);
}

void findTracksOnRoot()
{
  SD.chdir("/");
  File tmp;
  for(uint32_t i = 0; i<rootObjectCount; i++)
  {
    tmp.close();
    tmp.openNext(SD.vwd(), O_READ);
    if(!tmp.isDirectory())
    {
      if(VGMEngine.header.read(&tmp))
      {
        rootTrackIndicies.add(i);
      }
      else
      {
        uint16_t gzipmagic = 0;
        tmp.seekSet(0);
        tmp.read(&gzipmagic, 2);
        if(gzipmagic == 0x8B1F) //File header starts with gzip magic number
          rootTrackIndicies.add(i);
      }
    }
  }
  tmp.close();
  Serial.print("size of root tracks: "); Serial.println(rootTrackIndicies.size());
}

File getFileFromVwdIndex(uint32_t index)
{
  SD.vwd()->rewind();
  File tmp;
  for(uint32_t i = 0; i<index+1; i++)
  {
    tmp.close();
    tmp.openNext(SD.vwd(), O_READ);
  }
  return tmp;
}

bool VerifyDirectory(File f)
{
  if(!f.isDirectory()) //First, check to see if this object even is a directory in the first place
    return false;
  char tmpName[MAX_FILE_NAME_SIZE];
  f.getName(tmpName, MAX_FILE_NAME_SIZE);
  if((String(tmpName)+"/").startsWith(MANIFEST_DIR)) //Ignore the system dir
    return false;
  if(countFilesInDir(String(tmpName)) == 0) //Then, check to see if anything is even in the directory
    return false;
  return true;
}

//Mount file and prepare for playback. Returns true if file is found.
bool startTrack(FileStrategy fileStrategy, String request)
{
  String filePath = "";
  stop44k1();
  ready = false;
  File nextFile;
  memset(fileName, 0x00, MAX_FILE_NAME_SIZE);
  u8g2.setDrawColor(0);
  u8g2.drawStr(65, 64, " LOADING...");
  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
  switch(fileStrategy)
  {
    case FIRST_START:
    {
      //filePath = GetPathFromManifest(0);
    }
    break;
    case NEXT:
    {
      if(playMode == IN_ORDER)
      {
        if(dirCurIndex != dirEndIndex)
          dirCurIndex++;
        else
          dirCurIndex = dirStartIndex;
        file = getFileFromVwdIndex(dirCurIndex);
      }
      else if(playMode == SHUFFLE_ALL || playMode == SHUFFLE_DIR)
      {
        bool hasDir = false;
        if(randIndex == randFileList.size()-1 || randFileList.size() == 0) //End of random list, generate new random track and add to list
        {
          //Pick the directory first
          char dirName[MAX_FILE_NAME_SIZE];
          if(playMode == SHUFFLE_DIR)
          {
            SD.vwd()->getName(dirName, MAX_FILE_NAME_SIZE); //Get the current dir name and add it to the random dir list for history seeking
            randDirList.add(String(dirName));
          }
          else if(playMode == SHUFFLE_ALL)
          {
            File tmp;
            do
            {
              SD.chdir("/");
              memset(dirName, 0, MAX_FILE_NAME_SIZE);
              uint32_t rngDir = random(0, rootObjectCount+2); //+2 is used for picking the root. random() is exclusive on the max side, so if the roll is rootObjectCount+1, it would otherwise be out-of-bounds, but we will use it to represent a root roll
              if(rngDir == rootObjectCount+1) //+1, not +2 here. Remember, random is exclusive on the max side. If this statement is true, we have just rolled the root dir
              {
                SD.vwd()->getName(dirName, MAX_FILE_NAME_SIZE); //Current dir name right now would be root ("/").
                randDirList.add(String(dirName));
                hasDir = true;
                Serial.println("PICKED ROOT");
              }
              else //Otherwise, start scrolling through the entries to see if your pick is a directory
              {
                Serial.print("Picked DIR at INDEX: "); Serial.println(rngDir);
                for(uint32_t i = 0; i<rngDir; i++)
                {
                  tmp.close();
                  tmp.openNext(SD.vwd(), O_READ);
                }
                tmp.getName(dirName, MAX_FILE_NAME_SIZE);
                Serial.print("DIR NAME: "); Serial.println(dirName);
                if(VerifyDirectory(tmp))
                {
                  tmp.getName(dirName, MAX_FILE_NAME_SIZE);
                  randDirList.add(String(dirName));
                  hasDir = true;
                }
              }
              tmp.close();
            } while (!hasDir);
            
            if(String(dirName) == "/") //Dir is root. Pick from the pre-defiend list of valid VGM files
            {
              Serial.println("Dir is root");
              SD.chdir("/");
              uint32_t rngFile = rootTrackIndicies.get(random(0, rootTrackIndicies.size()));
              randFileList.add(rngFile);
              dirCurIndex = rngFile;
            }
            else //Dir is not root. Count how many files are in your selected dir, then pick a random one
            {
              uint32_t fcount = countFilesInDir("/"+String(dirName));
              SD.chdir(("/"+String(dirName)).c_str());
              Serial.print("fcount: ");Serial.println(fcount);
              uint32_t rng = random(0, fcount);
              randFileList.add(rng);
              dirCurIndex = rng;
            }
          }
          randIndex = randFileList.size()-1;
          file = getFileFromVwdIndex(dirCurIndex);
          //uint32_t rng = playMode == SHUFFLE_ALL ? random(numberOfFiles-1) : random(dirStartIndex, dirEndIndex+1);
        //   if(playMode == SHUFFLE_ALL) //Pick a random dir first, then pick a file
        //   {
        //     SD.chdir("/"); //Go to root
        //     File tmp;
        //     do
        //     {
        //       uint32_t rngDir = random(0, rootObjectCount+1);
              
        //       SD.vwd()->rewind();
        //       if(rngDir != 0) //0 means the root dir was rolled
        //       {
        //         for(uint32_t i = 0; i<rngDir; i++)
        //         {
        //           tmp.close();
        //           tmp.openNext(SD.vwd(), O_READ);
        //         }
        //       }
        //       else
        //       {
        //         Serial.println("!!!!!!ROLLED ROOT!");
        //         rng = rootTrackIndicies.get(random(0, rootTrackIndicies.size()));


        //         // for(uint32_t i = 0; i<rootObjectCount; i++) //If you selected the root, keep going through files until you find one that isn't a directory. 
        //         // {                                                  //If you go through the entire root and find no files, simply randomly pick a new directory in the next loop.
        //         //   tmp.close();
        //         //   tmp.openNext(SD.vwd(), O_READ);
        //         //   char fnametmp[128];
        //         //   tmp.getName(fnametmp, 128);
        //         //   Serial.println(fnametmp);
        //         //   if(!tmp.isDirectory()) //You've found a file on the root directory, get out of the loop
        //         //   {
        //         //     if(VGMEngine.header.read(&tmp)) //Verify the header to see if it's a VGM file
        //         //     {
        //         //       tmp.seekSet(0);
        //         //       hasRootFile = true;
        //         //       randFileList.add(i);
        //         //       dirCurIndex = i;
        //         //       break;
        //         //     }
        //         //   }
        //         // }
        //       }
        //     } while (!VerifyDirectory(tmp) && !hasRootFile);
            
        //     if(hasRootFile) //Did you find a file on the root or did you find a directory
        //     {
        //       file = tmp;
        //       tmp.close();
        //       dirEndIndex = countFilesInDir("/")-1;
        //       SD.chdir("/");
        //       memset(dirName, 0, MAX_FILE_NAME_SIZE);
        //       dirName[0] = '/';
        //     }
        //     else
        //     {
        //       tmp.getName(dirName, MAX_FILE_NAME_SIZE);
        //       tmp.close();
        //       dirEndIndex = countFilesInDir(String(dirName))-1;
        //       SD.chdir(dirName);
        //       rng = random(dirStartIndex, dirEndIndex+1);
        //       randFileList.add(rng);
        //       file = getFileFromVwdIndex(rng);
        //       dirCurIndex = rng;
        //     }
        //   }
        //   else if(playMode == SHUFFLE_DIR)
        //   {

        //   }

        //   SD.vwd()->getName(dirName, MAX_FILE_NAME_SIZE);
        //   Serial.print("ADDED DIR: "); Serial.println(dirName);
        //   randDirList.add(String(dirName));
        //   randIndex = randFileList.size()-1;

        // }
        }
        else //Otherwise, move up in history
        {
          randIndex++;
          dirCurIndex = randFileList.get(randIndex);
          SD.chdir("/");
          SD.chdir(randDirList.get(randIndex).c_str());
          Serial.print("DIR NAME: "); Serial.println(randDirList.get(randIndex));
          file = getFileFromVwdIndex(dirCurIndex);
          //filePath = GetPathFromManifest(randFileList.get(randIndex));
        }
      }
    }
    break;
    case PREV:
    {
      if(playMode == IN_ORDER)
      {
        if(dirCurIndex != dirStartIndex)
          dirCurIndex--;
        else
          dirCurIndex = dirEndIndex;
        file = getFileFromVwdIndex(dirCurIndex);
      }
      else if(playMode == SHUFFLE_ALL || playMode == SHUFFLE_DIR)
      {
        if(randIndex != 0) //If you're not at the end of the history list, go back in history
        {
          randIndex--;
        }
        dirCurIndex = randFileList.get(randIndex);
        SD.chdir("/");
        SD.chdir(randDirList.get(randIndex).c_str());
        Serial.print("DIR FROM LIST: "); Serial.println(randDirList.get(randIndex));
        char dirName[MAX_FILE_NAME_SIZE];
        SD.vwd()->getName(dirName, MAX_FILE_NAME_SIZE);
        Serial.print("CURRENT DIR: "); Serial.println(dirName);
        file = getFileFromVwdIndex(dirCurIndex);
      }
    }
    break;
    case RND:
    { //This request will disrupt the random history and immediatly create a new node at the end of the random history list
      playMode = SHUFFLE_ALL; 
      mainMenu[2].enable(); //Reenable loop # control
      uint32_t rng = random(numberOfFiles-1);
      randFileList.add(rng);
      randIndex = randFileList.size() == 0 ? 0 : randFileList.size()-1;
      filePath = GetPathFromManifest(rng);
      dirCurIndex = rng;
    }
    break;
    case REQUEST:
    {
      request.trim();
      if(SD.exists(request.c_str()))
      {
        file.close();
        filePath = request;
        filePath.trim();
        strncpy(fileName, filePath.c_str(), MAX_FILE_NAME_SIZE);

        file = SD.open(filePath.c_str(), FILE_READ);
        Serial.println("File found!");
      }
      else
      {
        Serial.println("ERROR: File not found! Continuing with current song.");
        goto fail;
      }
    }
    break;
  }

  Serial.print("dirCurIndex: "); Serial.println(dirCurIndex);

  if(!file)
  {
    Serial.println("Failed to read file");
    goto fail;
  }
  else
  {
    //Check for VGZ. Decompress first if true
    uint16_t gzipmagic = 0;
    file.read(&gzipmagic, 2);
    if(gzipmagic == 0x8B1F) //File header starts with gzip magic number
    {
      opn.reset();
      sn.reset();
      u8g2.setDrawColor(0);
      u8g2.drawStr(45, 64, " EXTRACTING...");
      u8g2.setDrawColor(1);
      u8g2.sendBuffer();
      //Serial.println("Found GZIP magic...");
      char inName[MAX_FILE_NAME_SIZE]; 
      file.getName(inName, MAX_FILE_NAME_SIZE);
      //SD.remove(outName);
      //file.getName(inName, MAX_FILE_NAME_SIZE);
      file.close();
      if(!Decompress(inName, TMP_DECOMPRESSION_FILE_PATH))
      {
        Serial.println("Decompression failed");
        goto fail;
      }
      else
      {
        u8g2.setDrawColor(0);
        u8g2.drawStr(50, 64, " LOADING......");
        u8g2.setDrawColor(1);
        u8g2.sendBuffer();
      }
      file = SD.open(TMP_DECOMPRESSION_FILE_PATH, FILE_READ);
      if(!file)
      {
        Serial.println("Failed to read decompressed file");
        goto fail;
      }
    }
    file.seek(0);
    opn.reset();
    sn.reset();
    delay(100);
    if(VGMEngine.begin(&file))
    {
      printlnw(VGMEngine.gd3.enGameName);
      printlnw(VGMEngine.gd3.enTrackName);
      printlnw(VGMEngine.gd3.enSystemName);
      printlnw(VGMEngine.gd3.releaseDate);
      Serial.print("DYNAMIC BYTES FREE: "); Serial.println(freeMemory());
      if(menuState == IN_VGM)
        drawOLEDTrackInfo();
      set44k1ISR();
      return true;
    }
    else
    {
      Serial.println("Header Verify Fail");
      goto fail;
    }
  }

  fail:
  VGMEngine.state = VGMEngineState::IDLE;
  alertErrorState();
  //set44k1ISR();
  return false;
}

//Poll the serial port
void handleSerialIn()
{
  while(Serial.available())
  {
    stop44k1();
    char serialCmd = Serial.read();
    switch(serialCmd)
    {
      case '+':
        if(playMode == IN_ORDER || playMode == SHUFFLE_ALL)
          startTrack(NEXT);
      break;
      case '-':
        if(playMode == IN_ORDER || playMode == SHUFFLE_ALL)
          startTrack(PREV);
      break;
      case '*':
        startTrack(RND);
      break;
      case '/':
        playMode = SHUFFLE_ALL;
        //drawOLEDTrackInfo();
      break;
      case '.':
        playMode = LOOP;
        //drawOLEDTrackInfo();
      break;
      case '?':
        printlnw(VGMEngine.gd3.enGameName);
        printlnw(VGMEngine.gd3.enTrackName);
        printlnw(VGMEngine.gd3.enSystemName);
        printlnw(VGMEngine.gd3.releaseDate);
      break;
      case '!':
        isOledOn = !isOledOn;
        //drawOLEDTrackInfo();
      break;
      case 'r':
      {
        String req = Serial.readString();
        req.remove(0, 1); //Remove colon character
        startTrack(REQUEST, req);
      }
      break;
      case 'x': //Just used for debugging
      {
        u8g2.writeBufferXBM(Serial);
      }
      break;
      default:
        continue;
    }
  }
  Serial.flush();
  set44k1ISR();
}

void clearRandomHistory()
{
  randFileList.clear();
  randIndex = 0;
}

//Returns the number of files in a directory. Note that dirs will also increment the index
uint32_t countFilesInDir(String dir) 
{
  SD.chdir(dir.c_str());
  File countFile;
  uint32_t count = 0;
  currentDirDirCount = 0;
  char firstName[MAX_FILE_NAME_SIZE] = "";
  char curName[MAX_FILE_NAME_SIZE] = "";
  while (countFile.openNext(SD.vwd(), O_READ))
  {
    if(currentDirFileCount == 0) //Get the name of the first file in the dir
      countFile.getName(firstName, MAX_FILE_NAME_SIZE);
    else
    {
      countFile.getName(curName, MAX_FILE_NAME_SIZE);
      if(strcmp(curName, firstName) == 0) //If the current file name is the same as the first, we've looped in our dir and we can exit
      {
        countFile.close();
        SD.chdir("/"); //Go back to root
        return count;
      }
    }
    if(countFile.isDir())
      currentDirDirCount++;
    count++;
    countFile.close();
  }
  SD.chdir("/");
  return count;
  //If the dir is empty, count will be 0
}

void getDirIndices(String dir, String fname)
{
  //fname += '\r'; //stupid invisible carriage return
  // if(dir.startsWith("/"))
  //   dir.replace("/", "");
  // if(dir == "")
  //   dir = "/";
  // Serial.print("INCOMING DIR: "); Serial.println(dir);
  // Serial.print("INCOMING FNAME: "); Serial.println(fname);
  dirStartIndex = 0;
  dirEndIndex = countFilesInDir(dir)-1; 
  dirCurIndex = 0;

  SD.chdir(dir.c_str());
  File tmp;

  if(fname != "")
  {
    for(uint32_t i = 0; i<dirEndIndex; i++)
    {
      tmp.openNext(SD.vwd(), O_READ);
      char tmpName[MAX_FILE_NAME_SIZE];
      tmp.getName(tmpName, MAX_FILE_NAME_SIZE);
      Serial.println(tmpName);
      if(strncmp(fname.c_str(), tmpName, sizeof(fname.c_str())) == 0)
      {
        dirCurIndex = i;
      }
      tmp.close();
    }
  }
    Serial.print("DIR: "); Serial.println(dir);
    Serial.print("FNAME: "); Serial.println(fname);
    Serial.print("dirStartIndex: "); Serial.println(dirStartIndex);
    Serial.print("dirEndIndex: "); Serial.println(dirEndIndex);
    Serial.print("dirCurIndex: "); Serial.println(dirCurIndex);


  // manifest.open(MANIFEST_PATH, O_READ);
  // manifest.seek(0);
  // manifest.readStringUntil('\n'); //Skip machine generated preamble
  // String cur;
  // if(dir == "~/") //Files are on the root
  // {
  //   for(uint32_t i = 0; i<numberOfFiles; i++)
  //   {
  //     cur = manifest.readStringUntil('\n');
  //     cur.replace(String(i)+":", "");
  //     if(cur.startsWith(dir))
  //     {
  //       if(dirStartIndex == 0xFFFFFFFF)
  //         dirStartIndex = i;
  //       if(cur.endsWith(fname)) 
  //         dirCurIndex = i;
  //       dirEndIndex = i;
  //     }
  //     else if(dirStartIndex != 0xFFFFFFFF)
  //       break;
  //   }
  //   if(dirStartIndex == 0xFFFFFFFF)
  //     dirStartIndex = 0;
  // }
  // else  //Files are in a dir
  // {
  //   for(uint32_t i = 0; i<numberOfFiles; i++)
  //   {
  //     cur = manifest.readStringUntil('\n');
  //     cur.replace(String(i)+":", "");
  //     if(cur.startsWith(dir)) //This line is problematic if two dirs begin with the same strings
  //     {
  //       String curDir = readStringUntil(cur, '/'); //that's what this check is for
  //       if(curDir.equals(dir))
  //       {
  //         Serial.print("CUR: "); Serial.println(cur);
  //         Serial.print("DIR: "); Serial.println(dir);
  //         Serial.print("CURDIR: "); Serial.println(curDir);
  //         Serial.print("FNAME: "); Serial.println(fname);
  //         if(dirStartIndex == 0xFFFFFFFF)
  //           dirStartIndex = i;
  //         if(cur.endsWith(fname)) 
  //           dirCurIndex = i;
  //         dirEndIndex = i;
  //         //break;
  //       }
  //     }
  //     else if(dirStartIndex != 0xFFFFFFFF)
  //       break;
  //   }
  //   if(dirStartIndex == 0xFFFFFFFF)
  //     dirStartIndex = 0;
  //}
  
}

bool contains(String & in, char find)
{
  unsigned int l = in.length()-1;
  for(unsigned int i = 0; i<l; i++)
  {
    if(in[i] == find) 
      return true;
  }
  return false;
}

String readStringUntil(String & in, char terminator)
{
  uint32_t i = 0;
  String out = "";
  while(in[i] != '\n' && in[i] != terminator)
  {
    out += in[i++];
  }
  return out;
}

void getDirIndicesFromFullPath(String fullPath)
{
  Serial.println(fullPath);
  String path, fname = "";
  fullPath.replace(String(dirCurIndex)+":", "");
  if(fullPath.startsWith("~/"))
  {
    path = "~/";
    fullPath.replace(path, "");
  }
  else if(!contains(fullPath, '/')) //File is on root but doesn't contain the ~/ marker
  {
    path = "~/";
  }
  else
  {
    path = readStringUntil(fullPath, '/');
    fullPath.replace(path, "");
    path+='/';
  }
  fname = fullPath;
  fname.remove(0, 1);
  fname.trim();
  getDirIndices(path, fname);
}

//Get the file's index inside of a dir. If you pass 0, this function will count the files in the dir, otherwise, you can specify a dir size in advance if you've already ran "countFilesInDir()" to save time
uint32_t getFileIndexInDir(String dir, String fname, uint32_t dirSize)
{
  SD.chdir(dir.c_str());
  if(dirSize == 0)
    dirSize = countFilesInDir(dir);
  File countFile;
  char curName[MAX_FILE_NAME_SIZE] = "";
  char searchName[MAX_FILE_NAME_SIZE] = "";
  fname.toCharArray(searchName, MAX_FILE_NAME_SIZE);
  for(uint32_t i = 0; i<dirSize; i++)
  {
    countFile.openNext(SD.vwd(), O_READ);
    countFile.getName(curName, MAX_FILE_NAME_SIZE);
    if(strcmp(curName, searchName) == 0)
    {
        countFile.close();
        SD.chdir("/");
        return i;
    }
    countFile.close();
  }
  SD.chdir("/");
  countFile.close();
  return 0xFFFFFFFF; //Int max = error
}

// String getFilePathFromCurrentDirFileIndex()
// {
//   uint32_t index = 0;
//   File nextFile;
//   SD.chdir(currentDir.c_str()); //Make sure the system is on the same dir we are
//   memset(fileName, 0x00, MAX_FILE_NAME_SIZE);
//   while(nextFile.openNext(SD.vwd(), O_READ))
//   {
//     if(index == currentDirFileIndex) //This set of ifs is to account for dirs that might be in the root directory. We need to skip over those, so we'll just push the current index further if we encounter a dir
//     {
//       if(nextFile.isDir())
//       {
//         if(currentDirFileIndex+1 >= currentDirFileCount)
//         {
//           currentDirFileIndex = 0;
//         }
//         else
//           currentDirFileIndex++;
//       }
//       else
//         break; //Found the correct file
//     }
//     index++;
//     nextFile.close(); 
//   }

//   nextFile.getName(fileName, MAX_FILE_NAME_SIZE);
//   nextFile.close();
//   return currentDir + String(fileName);
// }

uint32_t readFile32(FatFile *f)
{
  uint32_t d = 0;
  uint8_t v0 = f->read();
  uint8_t v1 = f->read();
  uint8_t v2 = f->read();
  uint8_t v3 = f->read();
  d = uint32_t(v0 + (v1 << 8) + (v2 << 16) + (v3 << 24));
  return d;
}

String GetPathFromManifest(uint32_t index) //Gives a VGM file path back from the manifest file
{
  String selection;
  manifest.open(MANIFEST_PATH, O_READ);
  manifest.seek(0);
  manifest.readStringUntil('\n'); //Skip machine generated preamble
  uint32_t i = 0;
  while(true) //byte-wise string reads for bulk of seeking to be a little nicer to the RAM
  {           //This part just skips every entry until we arrive to the line we want
    if(i == index)
      break;
    if(manifest.read() == '\n')
      i++;
    if(i > numberOfFiles)
      return "ERROR";
  }
  selection = manifest.readStringUntil('\n');
  selection.replace(String(index) + ":", "");
  selection.replace("~/", ""); //for root dirs
  SD.chdir("/");
  manifest.close();
  return selection;
}

uint32_t freeKB()
{
  uint32_t kb = SD.vol()->freeClusterCount();
  kb *= SD.vol()->blocksPerCluster()/2;
  return kb;
}

bool VerifyManifest()
{
  SD.chdir("/");
  manifest.open(MANIFEST_PATH, O_READ);
  if(!manifest)
    return false;
  manifest.readStringUntil('\n'); //Skip machine generated preamble
  for(uint32_t i = 0; i<numberOfFiles; i++)
  {
    if(!manifest.readStringUntil('\n').startsWith(String(i)))
      return false;
  }
  return true;
}

void CreateManifest(bool createNew)
{
  //Manifest format
  //Preamble (String)
  //<file paths>(Strings)
  //...
  //Magic Number (uint32_t BIN)
  //Total # files (uint32_t BIN)
  //Last SD Free Space in Blocks (uint32_t BIN)
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.drawStr(0,16,"Indexing files");
  u8g2.drawStr(0,32,"Please wait...");
  u8g2.sendBuffer();
  FatFile d, f;
  int32_t prevBlocks;
  String path = "";
  char name[MAX_FILE_NAME_SIZE];
  Serial.println("Checking file manifest...");
  bool createNewManifest = createNew;

  SD.chdir("/");
  if(!SD.exists(MANIFEST_PATH))
    createNewManifest = true;
  else
  {
    manifest.open(MANIFEST_PATH, O_READ | O_WRITE);
    manifest.seekEnd(-12); //Verify magic number to make sure file isn't completely corrupted
    if(readFile32(&manifest) != MANIFEST_MAGIC)
    {
      Serial.println("MANIFEST MAGIC BAD!");
      createNewManifest = true;
    }
    else
    {
      Serial.println("MANIFEST MAGIC OK");
      manifest.seekEnd(-8);
      numberOfFiles = readFile32(&manifest);
      manifest.seekEnd(-4); //Read-in old manifest size
      prevBlocks = readFile32(&manifest);
      SD.remove(TMP_DECOMPRESSION_FILE_PATH);

      if(prevBlocks != SD.vol()->freeClusterCount())
        createNewManifest = true;
    }
  }

  if(createNewManifest)
  {
    if(!SD.exists(MANIFEST_DIR))
      SD.mkdir(MANIFEST_DIR);
    if(!manifest.remove())
    {
      if(SD.exists(MANIFEST_PATH))
        Serial.println("Failed to remove old file");
    }
    if(manifest.isOpen())
      manifest.close();
    manifest.open(MANIFEST_PATH, O_RDWR | O_CREAT);
    numberOfFiles = 0;
    prevBlocks = 0;

    u8g2.drawStr(0,48,"Changes Detected...");
    u8g2.drawStr(0,64,"Rebuilding Manifest...");
    u8g2.sendBuffer();
    Serial.println("File changes detected! Re-indexing. Please wait...");
    manifest.seek(0);
    manifest.println("MACHINE GENERATED FILE. DO NOT MODIFY");
    while(d.openNext(SD.vwd(), O_READ)) //Go through root directories
    {
      if(d.isDir() && !d.isRoot()) //Include all dirs except root
      {
        d.getName(name, MAX_FILE_NAME_SIZE);
        path = String(name);
        if(path == MANIFEST_DIR) //Ignore the system file holding the manifest
          continue;
        numberOfDirectories++;
        while(f.openNext(&d, O_READ)) //Once you're in a dir, go through each file and record them
        {
          f.getName(name, MAX_FILE_NAME_SIZE);
          // if(strcmp(name, MANIFEST_FILE_NAME) == 0)
          //   continue;
          //Serial.println(path + "/" + String(name)); //Replace with manifest file right
          manifest.print(numberOfFiles++);
          manifest.print(":");
          manifest.println(path + "/" + String(name));
          f.close();
        } 
      }
      // else //Get any files in the root dir here
      // {
        // d.getName(name, MAX_FILE_NAME_SIZE);
        // manifest.print(numberOfFiles++);
        // manifest.print(":");
        // manifest.println(String(name));
      // }
      d.close();
    }
    SD.vwd()->rewind();
    while(d.openNext(SD.vwd(), O_READ)) //Handle files in root separatly at the end to keep them in order
    {
      if(!d.isDir() && !d.isRoot())
      {
        d.getName(name, MAX_FILE_NAME_SIZE);
        manifest.print(numberOfFiles++);
        manifest.print(":~/");
        manifest.println(String(name));
      }
      d.close();
    }
    manifest.close();
    manifest.open(MANIFEST_PATH, O_AT_END | O_WRITE);
    uint32_t tmp = SD.vol()->freeClusterCount();
    manifest.write(&MANIFEST_MAGIC, 4);
    manifest.write(&numberOfFiles, 4);
    manifest.write(&tmp, 4);
  }
  else
    Serial.println("No change in files, continuing...");
  manifest.close();
  Serial.println("Indexing complete");
  u8g2.clearDisplay();
  u8g2.sendBuffer();
  nav.refresh();
}

void removeMeta() //Remove useless meta files
{
  File tmpFile;
  while ( tmpFile.openNext( SD.vwd(), O_READ ))
  {
    memset(fileName, 0x00, MAX_FILE_NAME_SIZE);
    tmpFile.getName(fileName, MAX_FILE_NAME_SIZE);
    if(fileName[0]=='.')
    {
      if(!SD.remove(fileName))
      if(!tmpFile.rmRfStar())
      {
        Serial.print("FAILED TO DELETE META FILE"); Serial.println(fileName);
      }
    }
    if(String(fileName) == "System Volume Information")
    {
      if(!tmpFile.rmRfStar())
        Serial.println("FAILED TO REMOVE SVI");
    }
    tmpFile.close();
  }
  tmpFile.close();
  SD.vwd()->rewind();
}

void alertErrorState()
{
    menuState = IN_VGM;
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);
    u8g2.drawStr(0,16,"Invalid File Loaded!");
    u8g2.drawStr(0,32,fileName);
    u8g2.drawStr(0,48,"Press any button to");
    u8g2.drawStr(0,64,"return to menu...");
    u8g2.sendBuffer();
    while(true)
    {
      bool pressed = false;
      //Debounced buttons
      for(uint8_t i = 0; i<5; i++)
      {
        buttons[i].update();
        if(buttons[i].fell())
          pressed = true;
      }
      if(pressed)
        break;
    }
    delay(100);
    menuState = IN_MENU;
    VGMEngine.state = VGMEngineState::IDLE;
    nav.refresh();
}

void loop()
{    
  switch(VGMEngine.play())
  {
    case VGMEngineState::IDLE:
    break;
    case VGMEngineState::END_OF_TRACK:
      if(playMode == IN_ORDER || playMode == SHUFFLE_ALL || playMode == SHUFFLE_DIR)
        startTrack(NEXT);
    break;
    case VGMEngineState::PLAYING:
    break;
    case  VGMEngineState::ERROR:
      alertErrorState();
    break;
  }

  if(Serial.available() > 0) 
    handleSerialIn();

  //Debounced buttons
  for(uint8_t i = 0; i<5; i++)
  {
    buttons[i].update();
  }

  if(buttons[0].fell()) //Next
  {
    if(menuState == IN_MENU)
      nav.doNav(navCmd(enterCmd));
    else if(menuState == IN_VGM)
    {
      if(playMode == IN_ORDER || playMode == SHUFFLE_ALL || playMode == SHUFFLE_DIR)
        startTrack(NEXT);
      // else if(playMode == SHUFFLE_ALL)
      //   startTrack(RND);
    }
  }
  if(buttons[1].fell()) //Prev
  {
    if(menuState == IN_MENU)
        nav.doNav(navCmd(escCmd));
    else if(menuState == IN_VGM)
    {
      if(playMode == IN_ORDER || playMode == SHUFFLE_ALL || playMode == SHUFFLE_DIR)
        startTrack(PREV);
    }
  }
  if(buttons[2].fell()) //Up
  {
    if(menuState == IN_MENU)
      nav.doNav(navCmd(downCmd));
  }
  if(buttons[3].fell())                         //Select
    {
      if(menuState == IN_MENU) //If you're in the file-picker, the select button is used to enter dirs
        nav.doNav(navCmd(enterCmd)); 
      else if(menuState == IN_VGM) //Otherwise, you can use the select key to go back to the file picker where you left off
      {
        menuState = IN_MENU;
        nav.timeOut=MENU_TIMEOUT_IN_SECONDS;
        nav.idleOff();
        nav.refresh();
      }
    }
  if(buttons[4].fell())//Down
  {
    if(menuState == IN_MENU)
      nav.doNav(navCmd(upCmd));
  }

  //UI
  if (nav.changed(0) && menuState == IN_MENU) {//only draw if menu changed for gfx device
    //change checking leaves more time for other tasks
    u8g2.firstPage();
    do nav.doOutput(); while(u8g2.nextPage());
  }
}




//UI

//implementing the handler here after filePick is defined...
result filePick(eventMask event, navNode& _nav, prompt &item) 
{
  // switch(event) {//for now events are filtered only for enter, so we dont need this checking
  //   case enterCmd:
      if (_nav.root->navFocus==(navTarget*)&filePickMenu) {
        // Serial.println();
        // Serial.print("selected file:");
        // Serial.println(filePickMenu.selectedFile);
        // Serial.print("from folder:");
        // Serial.println(filePickMenu.selectedFolder);
        if(filePickMenu.selectedFile == MANIFEST_FILE_NAME)
          return proceed;
        menuState = IN_VGM;
        if(filePickMenu.selectedFolder != currentDir)
        {
          currentDir = filePickMenu.selectedFolder;
          if(playMode == SHUFFLE_DIR || playMode == SHUFFLE_ALL)
            clearRandomHistory();
        }
        getDirIndices(filePickMenu.selectedFolder, filePickMenu.selectedFile);
        if(playMode == SHUFFLE_DIR || playMode == SHUFFLE_ALL)
        {
          if(randFileList.size() == 0)
            randIndex = 0;
          else
            randIndex++;
          randFileList.add(dirCurIndex);
        }
        startTrack(REQUEST, filePickMenu.selectedFolder+filePickMenu.selectedFile);
      }
  return proceed;
}

result doCreateManifest()
{
  do
  {
    CreateManifest(true);
  }while(!VerifyManifest());
  return proceed;
}

result onMenuIdle(menuOut& o,idleEvent e) 
{
  if (VGMEngine.state == PLAYING) 
  {
    switch(e)
    {
      case idleStart:
        
      break;
      case idling:
      {
        menuState = IN_VGM;
        drawOLEDTrackInfo();  
        nav.timeOut=0xFFFFFFFF;
        nav.refresh();
      }
      break;
      case idleEnd:
      nav.timeOut=MENU_TIMEOUT_IN_SECONDS;
      break;
    }
  }
  return proceed;
}

result onChoosePlaymode(eventMask e,navNode& _nav,prompt& item)
{
  if(e == exitEvent)
  {
    switch(playMode)
    {
      case PlayMode::LOOP:
      VGMEngine.maxLoops = 0xFFFF;
      mainMenu[2].disable();
      break;
      case PlayMode::SHUFFLE_ALL:
      VGMEngine.maxLoops = 3;
      mainMenu[2].enable();
      menuState = IN_VGM;
      nav.timeOut=0xFFFFFFFF;
      clearRandomHistory();
      startTrack(NEXT);
      drawOLEDTrackInfo();
      break;
      case PlayMode::IN_ORDER:
      VGMEngine.maxLoops = 3;
      mainMenu[2].enable();
      getDirIndicesFromFullPath(GetPathFromManifest(dirCurIndex));
      break;
      case PlayMode::SHUFFLE_DIR:
      VGMEngine.maxLoops = 3;
      mainMenu[2].enable();
      clearRandomHistory();
      getDirIndicesFromFullPath(GetPathFromManifest(dirCurIndex));
      break;
      case PlayMode::PAUSE:
      break;
      default:
      break;
    }
  }
  return proceed;
}

  //Handy old code

  //Direct IO examples for reading pins
  //if (REG_PORT_IN0 & PORT_PA19)  // if (digitalRead(12) == HIGH)
  //if (!(REG_PORT_IN0 | ~PORT_PA19)) // if (digitalRead(12) == LOW) FOR NON-PULLED PINS!
  //if(!(REG_PORT_IN1 & next_btn)) //LOW for PULLED PINS

  // //Group 1 is port B, PINCFG just wants the literal pin number on the port, DIRCLR and OUTSET want the masked PORT_XX00 *not* the PIN_XX00
  // //Prev button input pullup
  // PORT->Group[1].PINCFG[0].reg=(uint8_t)(PORT_PINCFG_INEN|PORT_PINCFG_PULLEN);
  // PORT->Group[1].DIRCLR.reg = PORT_PB00;
  // PORT->Group[1].OUTSET.reg = PORT_PB00;

  // //Rand button input pullup
  // PORT->Group[1].PINCFG[1].reg=(uint8_t)(PORT_PINCFG_INEN|PORT_PINCFG_PULLEN);
  // PORT->Group[1].DIRCLR.reg = PORT_PB01;
  // PORT->Group[1].OUTSET.reg = PORT_PB01;

  // //Next button input pullup
  // PORT->Group[1].PINCFG[4].reg=(uint8_t)(PORT_PINCFG_INEN|PORT_PINCFG_PULLEN);
  // PORT->Group[1].DIRCLR.reg = PORT_PB04;
  // PORT->Group[1].OUTSET.reg = PORT_PB04;

  // //Option button input pullup
  // PORT->Group[1].PINCFG[5].reg=(uint8_t)(PORT_PINCFG_INEN|PORT_PINCFG_PULLEN); 
  // PORT->Group[1].DIRCLR.reg = PORT_PB05;
  // PORT->Group[1].OUTSET.reg = PORT_PB05;

  // //Select button input pullup
  // PORT->Group[1].PINCFG[9].reg=(uint8_t)(PORT_PINCFG_INEN|PORT_PINCFG_PULLEN); 
  // PORT->Group[1].DIRCLR.reg = PORT_PB09;
  // PORT->Group[1].OUTSET.reg = PORT_PB09;
