
/*
  PIXIDRIVER Pro — WS2812FX Buttons + Universe=0 + Mobile UX polish + FullHD scale
  - WS2812FX sve modove prikazuje kao gumbe (fetch sa /wsfx), startaju odmah na klik
  - Universe moze biti 0 (UI min=0), spremanje stabilno
  - Hamburger zatvara sidebar kad se izabere tab (na mobitelu)
  - Header: naslov centriran, IP/SSID/RSSI red ispod, kompaktniji na mobu
  - UI veći, koristi širinu 1920x1080
  - DMX/Art-Net/sACN imaju prioritet nad internim efektima
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_DotStar.h>
#include <LPD8806.h>
#include <WS2812FX.h>

// WS2812FX: suppress internal show during DMX by providing a no-op custom show
void wsfx_noop_show() { /* no-op: central render via dmxTryShow() */ }

// ===== CPU metrics (global) =====
volatile float avgLoopMs = 1.0f;
volatile float loadPct   = 0.0f;


#define MAX_PORTS 4
#define ARTNET_PORT_DEFAULT 6454
#define SACN_PORT 5568
#define MAX_TEMPLATES 24
#define TPL_NS "pxpro_tpl"
#define CFG_NS "pxpro_cfg"

static const char ARTNET_ID[] = "Art-Net";
static const uint16_t OPCODE_ARTDMX = 0x5000;
static const uint16_t CHANNELS_PER_UNIVERSE = 512;

enum LedModel  : uint8_t { LED_RGB=0, LED_RGBW=1, LED_APA102=2, LED_LPD8806=3 };
enum ColorOrder: uint8_t { ORDER_RGB=0, ORDER_GRB=1, ORDER_BGR=2, ORDER_GBR=3, ORDER_RBG=4, ORDER_BRG=5 };
enum InputProto: uint8_t { PROTO_AUTO=0, PROTO_ARTNET=1, PROTO_SACN=2 };

struct PortCfg {
  bool       enabled;
  uint8_t    dataPin;
  uint8_t    clkPin;
  uint16_t   startUniverse;
  uint16_t   ledCount;
  LedModel   model;
  ColorOrder order;
  uint8_t    brightness;
};

struct NetCfg {
  bool dhcp;
  String ssid, pass;
  String hostname;
  IPAddress ip, gw, mask, dns1, dns2;
  String apSsid, apPass;
};

struct AppCfg {
  uint16_t artnetPort;
  PortCfg  ports[MAX_PORTS];
  NetCfg   net;
  uint8_t  globalBrightness;
  InputProto proto = PROTO_AUTO;
};

Preferences prefs;
Preferences tplstore;
WebServer   server(80);
WiFiUDP     UdpArtnet, UdpSACN;

// ===== Strips =====
struct StripBase { virtual ~StripBase(){}; virtual void begin()=0; virtual void setBrightness(uint8_t)=0; virtual void setPixel(uint16_t,uint8_t,uint8_t,uint8_t,uint8_t=0)=0; virtual void show()=0; };
struct StripNeo : public StripBase{
  Adafruit_NeoPixel neo; bool isRGBW;
  StripNeo(uint16_t n,uint8_t pin,bool rgbw,neoPixelType t):neo(n,pin,t),isRGBW(rgbw){}
  void begin()override{ neo.begin(); neo.show(); }
  void setBrightness(uint8_t b)override{ neo.setBrightness(b); }
  void setPixel(uint16_t i,uint8_t r,uint8_t g,uint8_t b,uint8_t w=0)override{ if(isRGBW) neo.setPixelColor(i,r,g,b,w); else neo.setPixelColor(i,r,g,b); }
  void show()override{ neo.show(); }
  void updateType(neoPixelType t){ neo.updateType(t); }
};
struct StripAPA: public StripBase{
  Adafruit_DotStar ds; StripAPA(uint16_t n,uint8_t dp,uint8_t cp):ds(n,dp,cp,DOTSTAR_BGR){}
  void begin()override{ ds.begin(); ds.show(); }
  void setBrightness(uint8_t b)override{ ds.setBrightness(b); }
  void setPixel(uint16_t i,uint8_t r,uint8_t g,uint8_t b,uint8_t w=0)override{ (void)w; ds.setPixelColor(i,r,g,b); }
  void show()override{ ds.show(); }
};
struct StripLPD: public StripBase{
  LPD8806 lpd; StripLPD(uint16_t n,uint8_t dp,uint8_t cp):lpd(n,dp,cp){}
  void begin()override{ lpd.begin(); lpd.show(); }
  void setBrightness(uint8_t b)override{ (void)b; }
  void setPixel(uint16_t i,uint8_t r,uint8_t g,uint8_t b,uint8_t w=0)override{ (void)w; lpd.setPixelColor(i,r,g,b); }
  void show()override{ lpd.show(); }
};

StripBase* strips[MAX_PORTS]={nullptr};
WS2812FX* fx[MAX_PORTS]={nullptr};
AppCfg cfg;
static bool g_wsfxMuted = false;


// ===== Helpers =====
uint8_t bytesPerPixel_u8(uint8_t m){ return m==LED_RGBW?4:3; }
uint16_t pixelsPerUniverse_u8(uint8_t m){ return m==LED_RGBW? (CHANNELS_PER_UNIVERSE/4):(CHANNELS_PER_UNIVERSE/3); }
static uint8_t clampu8(int v){ return v<0?0:(v>255?255:v); }
static uint16_t clampu16_any(int v,int mx){ if(v<0)return 0; if(v>mx)return mx; return v; }
uint8_t mapIndex_u8(uint8_t o,uint8_t idx){
  switch(o){case ORDER_RGB:{static const uint8_t m[3]={0,1,2};return m[idx];}
    case ORDER_GRB:{static const uint8_t m[3]={1,0,2};return m[idx];}
    case ORDER_BGR:{static const uint8_t m[3]={2,1,0};return m[idx];}
    case ORDER_GBR:{static const uint8_t m[3]={1,2,0};return m[idx];}
    case ORDER_RBG:{static const uint8_t m[3]={0,2,1};return m[idx];}
    case ORDER_BRG:{static const uint8_t m[3]={2,0,1};return m[idx];}
  } return idx;
}
// === DMX FRAME SYNC (anti-flicker) ===========================================
// Build full frames across universes, then render once (or after a tiny timeout).
// This eliminates partial refresh flicker when multiple universes feed the same port.
struct DMXFrameSync {
  uint64_t uniMask[8];            // up to 8 ports supported; only [0..MAX_PORTS-1] used
  uint64_t uniMaskExpected[8];
  bool     pendingShow;
  uint32_t lastAnyMs;
  uint8_t  lastSeq;
};
static DMXFrameSync g_sync = {{0},{0},false,0,0};

#ifndef SHOW_TIMEOUT_MS
#define SHOW_TIMEOUT_MS 30  // 20–30ms typical; higher tolerates jitter, lower reduces latency
#endif

static uint16_t _universesForPortCalc(int p){
  if(p<0 || p>=MAX_PORTS) return 0;
  const uint32_t bytesNeeded = (uint32_t)cfg.ports[p].ledCount * bytesPerPixel_u8(cfg.ports[p].model);
  const uint16_t perUni = 510; // use 510 to align RGB triplets
  return bytesNeeded ? ( (bytesNeeded + (perUni-1)) / perUni ) : 0;
}

static void dmxRecomputeExpectedMasks(){
  for(int p=0;p<MAX_PORTS && p<8;p++){
    uint64_t m=0; uint16_t need=_universesForPortCalc(p);
    for(uint16_t i=0;i<need && i<64;i++) m |= (uint64_t)1<<i;
    g_sync.uniMaskExpected[p]=m;
    g_sync.uniMask[p]=0;
  }
}

static inline bool _universeBelongsToPort(int p, uint16_t rxUni, uint16_t &indexWithinPort){
  const uint16_t start = cfg.ports[p].startUniverse;
  const uint16_t cnt   = _universesForPortCalc(p);
  if(cnt==0) return false;
  if(rxUni<start) return false;
  uint32_t d = (uint32_t)rxUni - (uint32_t)start;
  if(d>=cnt) return false;
  indexWithinPort = (uint16_t)d;
  return true;
}

// Call after writing DMX data into buffers
static inline void dmxNoteUniverseArrived(uint16_t rxUniverse){
  for(int p=0;p<MAX_PORTS && p<8;p++){
    if(!cfg.ports[p].enabled) continue;
    uint16_t idx=0;
    if(_universeBelongsToPort(p, rxUniverse, idx)){
      if(idx<64) g_sync.uniMask[p] |= (uint64_t)1<<idx;
      g_sync.pendingShow = true;
      g_sync.lastAnyMs = millis();
    }
  }
}

static inline bool _allPortsComplete(){
  for(int p=0;p<MAX_PORTS && p<8;p++){
    if(!cfg.ports[p].enabled) continue;
    if(g_sync.uniMaskExpected[p]==0) continue;
    if(g_sync.uniMask[p] != g_sync.uniMaskExpected[p]) return false;
  }
  return true;
}

// Single place that renders strips to prevent partial refresh
static void dmxTryShow(){
  if(!g_sync.pendingShow) return;
  const uint32_t now = millis();
  bool doShow = _allPortsComplete() || ((now - g_sync.lastAnyMs) >= SHOW_TIMEOUT_MS);
  if(!doShow) return;

  for(int p=0;p<MAX_PORTS;p++) if(cfg.ports[p].enabled && strips[p]) strips[p]->show();
  for(int p=0;p<MAX_PORTS && p<8;p++) if(g_sync.uniMaskExpected[p]!=0) g_sync.uniMask[p]=0;
  g_sync.pendingShow=false;
}
// === END DMX FRAME SYNC

static void unpackRGB(uint32_t c,uint8_t& r,uint8_t& g,uint8_t& b){ r=(c>>16)&0xFF; g=(c>>8)&0xFF; b=c&0xFF; }
uint32_t parseHexColor(const String& s){
  if(s.length()!=7||s[0]!='#') return 0xFFFFFF;
  auto hex=[](char c)->uint8_t{ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return 10+c-'a'; if(c>='A'&&c<='F')return 10+c-'A'; return 0; };
  uint8_t r=(hex(s[1])<<4)|hex(s[2]), g=(hex(s[3])<<4)|hex(s[4]), b=(hex(s[5])<<4)|hex(s[6]);
  return ((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}

// ===== Patterns =====
enum PatName:uint8_t{ PAT_NONE=0, PAT_SOLID, PAT_RAINBOW, PAT_COLORWIPE, PAT_BLINK, PAT_FADE, PAT_THEATER, PAT_COMET, PAT_TWINKLE, PAT_WAVE, PAT_WSFX };
struct PatternState{ PatName pat=PAT_NONE; uint32_t color=0x000000; uint8_t speed=80; bool mirror=false; uint32_t tLast=0; int32_t step=0; bool on=true; };
PatternState pat[MAX_PORTS];

// DMX priority
bool dmxActive=false; IPAddress lastDMXSrc;
static const uint32_t DMX_HOLD_MS = 1000;
static uint32_t lastDmxMs = 0;

void hsvToRgb(uint16_t h,uint8_t s,uint8_t v,uint8_t& r,uint8_t& g,uint8_t& b){
  uint8_t region=h/43; uint8_t rem=(h-(region*43))*6;
  uint8_t p=(v*(255-s))>>8, q=(v*(255-((s*rem)>>8)))>>8, t=(v*(255-((s*(255-rem))>>8)))>>8;
  switch(region){case 0:r=v;g=t;b=p;break;case 1:r=q;g=v;b=p;break;case 2:r=p;g=v;b=t;break;case 3:r=p;g=q;b=v;break;case 4:r=t;g=p;b=v;break;default:r=v;g=p;b=q;break;}
}

void applyPatternOne(int p){
  if(!cfg.ports[p].enabled || !strips[p]) return;
  if(dmxActive && (millis()-lastDmxMs<DMX_HOLD_MS)) return;
  if(pat[p].pat==PAT_NONE || pat[p].pat==PAT_WSFX) return;

  uint32_t now=millis(); uint16_t n=cfg.ports[p].ledCount;
  uint32_t interval = 5 + (205 - pat[p].speed);
  if(now - pat[p].tLast < interval) return; pat[p].tLast=now;
  uint8_t cr,cg,cb; unpackRGB(pat[p].color,cr,cg,cb); uint16_t half=n/2;
  auto setIdx=[&](uint16_t i,uint8_t r,uint8_t g,uint8_t b){ if(pat[p].mirror && i<half){ strips[p]->setPixel(i,r,g,b); strips[p]->setPixel(n-1-i,r,g,b);} else strips[p]->setPixel(i,r,g,b); };
  switch(pat[p].pat){
    case PAT_SOLID: for(uint16_t i=0;i<(pat[p].mirror?half:n);i++) setIdx(i,cr,cg,cb); break;
    case PAT_RAINBOW:{
      for(uint16_t i=0;i<(pat[p].mirror?half:n);i++){ uint16_t hue=(i*256/(pat[p].mirror?half:n)+pat[p].step)&0xFF; uint8_t r,g,b; hsvToRgb(hue,255,255,r,g,b); setIdx(i,r,g,b);} pat[p].step=(pat[p].step+1)&0xFF;
    }break;
    case PAT_COLORWIPE:{
      for(uint16_t i=0;i<(pat[p].mirror?half:n);i++){ if(i<=pat[p].step) setIdx(i,cr,cg,cb); else setIdx(i,0,0,0);} if(pat[p].step<((pat[p].mirror?half:n)-1)) pat[p].step++; else pat[p].step=0;
    }break;
    case PAT_BLINK:{ pat[p].on=!pat[p].on; uint8_t rr=pat[p].on?cr:0, gg=pat[p].on?cg:0, bb=pat[p].on?cb:0; for(uint16_t i=0;i<(pat[p].mirror?half:n);i++) setIdx(i,rr,gg,bb); }break;
    case PAT_FADE:{ pat[p].step=(pat[p].step+4)&0xFF; uint8_t v=pat[p].step; for(uint16_t i=0;i<(pat[p].mirror?half:n);i++) setIdx(i,(uint8_t)((cr*v)>>8),(uint8_t)((cg*v)>>8),(uint8_t)((cb*v)>>8)); }break;
    case PAT_THEATER:{ uint8_t spacing=3; for(uint16_t i=0;i<(pat[p].mirror?half:n);i++){ if(((i+pat[p].step)%spacing)==0) setIdx(i,cr,cg,cb); else setIdx(i,0,0,0);} pat[p].step=(pat[p].step+1)%spacing; }break;
    case PAT_COMET:{ int head=pat[p].step%(pat[p].mirror?half:n); uint8_t tail=10; for(uint16_t i=0;i<(pat[p].mirror?half:n);i++) setIdx(i,0,0,0); for(uint8_t t=0;t<tail;t++){ int idx=head-t; if(idx<0) idx+=(pat[p].mirror?half:n); float k=1.0f-(float)t/(float)tail; setIdx(idx,(uint8_t)(cr*k),(uint8_t)(cg*k),(uint8_t)(cb*k)); } pat[p].step++; }break;
    case PAT_TWINKLE:{ for(uint16_t i=0;i<(pat[p].mirror?half:n);i++) setIdx(i,0,0,0); uint8_t k=(uint8_t)max(2,(int)(cfg.ports[p].ledCount/30)); for(uint8_t j=0;j<k;j++){ uint16_t idx=(uint16_t)random(pat[p].mirror?half:n); uint8_t v=150+random(106); setIdx(idx,(uint8_t)((cr*v)>>8),(uint8_t)((cg*v)>>8),(uint8_t)((cb*v)>>8)); } }break;
    case PAT_WAVE:{ pat[p].step=(pat[p].step+1)&0xFFFF; float w=2.0f*3.14159f/(float)(pat[p].mirror?half:n); for(uint16_t i=0;i<(pat[p].mirror?half:n);i++){ float s=0.5f*(1.0f+sinf((float)(i+pat[p].step)*w)); setIdx(i,(uint8_t)(cr*s),(uint8_t)(cg*s),(uint8_t)(cb*s)); } }break;
    default: break;
  }
  strips[p]->show();
}

// ===== Storage =====
void loadDefaults(){
  cfg.artnetPort=ARTNET_PORT_DEFAULT; cfg.globalBrightness=220; cfg.proto=PROTO_AUTO;
  for(int i=0;i<MAX_PORTS;i++){
    cfg.ports[i].enabled=(i==0);
    cfg.ports[i].dataPin=(i==0)?5:(i==1?18:(i==2?19:21));
    cfg.ports[i].clkPin=14;
    cfg.ports[i].startUniverse=i; // dozvoli 0 za port0
    cfg.ports[i].ledCount=60;
    cfg.ports[i].model=LED_RGB;
    cfg.ports[i].order=ORDER_GRB;
    cfg.ports[i].brightness=255;
  }
  cfg.net.dhcp=true; cfg.net.hostname="pixidriver"; cfg.net.ssid=""; cfg.net.pass="";
  cfg.net.ip=IPAddress(192,168,8,100); cfg.net.gw=IPAddress(192,168,8,1);
  cfg.net.mask=IPAddress(255,255,255,0); cfg.net.dns1=IPAddress(8,8,8,8); cfg.net.dns2=IPAddress(1,1,1,1);
  cfg.net.apSsid="PIXIDRIVER"; cfg.net.apPass="smartspot";
}
void saveConfig(){
  prefs.begin(CFG_NS,false);
  prefs.putUShort("aport",cfg.artnetPort);
  prefs.putString("host",cfg.net.hostname);
  prefs.putBool("dhcp",cfg.net.dhcp);
  prefs.putString("ssid",cfg.net.ssid);
  prefs.putString("pass",cfg.net.pass);
  prefs.putUInt("ip",(uint32_t)cfg.net.ip); prefs.putUInt("gw",(uint32_t)cfg.net.gw);
  prefs.putUInt("mask",(uint32_t)cfg.net.mask); prefs.putUInt("dns1",(uint32_t)cfg.net.dns1); prefs.putUInt("dns2",(uint32_t)cfg.net.dns2);
  prefs.putString("apssid",cfg.net.apSsid); prefs.putString("appass",cfg.net.apPass);
  prefs.putUChar("gbr",cfg.globalBrightness);
  prefs.putUChar("proto",(uint8_t)cfg.proto);
  for(int i=0;i<MAX_PORTS;i++){
    char k[16];
    snprintf(k,sizeof(k),"p%den",i); prefs.putBool(k,cfg.ports[i].enabled);
    snprintf(k,sizeof(k),"p%ddp",i); prefs.putUChar(k,cfg.ports[i].dataPin);
    snprintf(k,sizeof(k),"p%dc", i); prefs.putUChar(k,cfg.ports[i].clkPin);
    snprintf(k,sizeof(k),"p%dsu",i); prefs.putUShort(k,cfg.ports[i].startUniverse);
    snprintf(k,sizeof(k),"p%dlc",i); prefs.putUShort(k,cfg.ports[i].ledCount);
    snprintf(k,sizeof(k),"p%dlm",i); prefs.putUChar(k,cfg.ports[i].model);
    snprintf(k,sizeof(k),"p%dco",i); prefs.putUChar(k,cfg.ports[i].order);
    snprintf(k,sizeof(k),"p%db", i); prefs.putUChar(k,cfg.ports[i].brightness);
  }
  prefs.end();
}
void loadConfig(){
  loadDefaults();
  prefs.begin(CFG_NS,true);
  if(prefs.isKey("aport")){
    cfg.artnetPort=prefs.getUShort("aport",ARTNET_PORT_DEFAULT);
    cfg.net.hostname=prefs.getString("host","pixidriver");
    cfg.net.dhcp=prefs.getBool("dhcp",true);
    cfg.net.ssid=prefs.getString("ssid",""); cfg.net.pass=prefs.getString("pass","");
    cfg.net.ip=(uint32_t)prefs.getUInt("ip",(uint32_t)cfg.net.ip); cfg.net.gw=(uint32_t)prefs.getUInt("gw",(uint32_t)cfg.net.gw);
    cfg.net.mask=(uint32_t)prefs.getUInt("mask",(uint32_t)cfg.net.mask); cfg.net.dns1=(uint32_t)prefs.getUInt("dns1",(uint32_t)cfg.net.dns1); cfg.net.dns2=(uint32_t)prefs.getUInt("dns2",(uint32_t)cfg.net.dns2);
    cfg.net.apSsid=prefs.getString("apssid","PIXIDRIVER"); cfg.net.apPass=prefs.getString("appass","smartspot");
    cfg.globalBrightness=prefs.getUChar("gbr",220);
    cfg.proto=(InputProto)prefs.getUChar("proto",(uint8_t)PROTO_AUTO);
    for(int i=0;i<MAX_PORTS;i++){
      char k[16];
      snprintf(k,sizeof(k),"p%den",i); cfg.ports[i].enabled=prefs.getBool(k,cfg.ports[i].enabled);
      snprintf(k,sizeof(k),"p%ddp",i); cfg.ports[i].dataPin=prefs.getUChar(k,cfg.ports[i].dataPin);
      snprintf(k,sizeof(k),"p%dc", i); cfg.ports[i].clkPin=prefs.getUChar(k,cfg.ports[i].clkPin);
      snprintf(k,sizeof(k),"p%dsu",i); cfg.ports[i].startUniverse=prefs.getUShort(k,cfg.ports[i].startUniverse);
      snprintf(k,sizeof(k),"p%dlc",i); cfg.ports[i].ledCount=prefs.getUShort(k,cfg.ports[i].ledCount);
      snprintf(k,sizeof(k),"p%dlm",i); cfg.ports[i].model=(LedModel)prefs.getUChar(k,cfg.ports[i].model);
      snprintf(k,sizeof(k),"p%dco",i); cfg.ports[i].order=(ColorOrder)prefs.getUChar(k,cfg.ports[i].order);
      snprintf(k,sizeof(k),"p%db", i); cfg.ports[i].brightness=prefs.getUChar(k,cfg.ports[i].brightness);
    }
  }
  prefs.end();
}

// ===== Templates store =====
bool saveTemplateSlot(uint8_t slot,const String& name,const String& jsonPayload,const String& btnColor){
  if(slot>=MAX_TEMPLATES) return false;
  tplstore.begin(TPL_NS,false);
  char kn[8], kj[8], kb[8]; snprintf(kn,sizeof(kn),"n%u",slot); snprintf(kj,sizeof(kj),"j%u",slot); snprintf(kb,sizeof(kb),"b%u",slot);
  tplstore.putString(kn,name); tplstore.putString(kj,jsonPayload); tplstore.putString(kb,btnColor);
  tplstore.end(); return true;
}
bool loadTemplateSlot(uint8_t slot,String& name,String& jsonPayload,String& btnColor){
  if(slot>=MAX_TEMPLATES) return false;
  tplstore.begin(TPL_NS,true);
  char kn[8], kj[8], kb[8]; snprintf(kn,sizeof(kn),"n%u",slot); snprintf(kj,sizeof(kj),"j%u",slot); snprintf(kb,sizeof(kb),"b%u",slot);
  if(!tplstore.isKey(kn) || !tplstore.isKey(kj)){ tplstore.end(); return false; }
  name=tplstore.getString(kn,""); jsonPayload=tplstore.getString(kj,""); btnColor=tplstore.getString(kb,"#222222");
  tplstore.end(); return true;
}
bool deleteTemplateSlot(uint8_t slot){
  if(slot>=MAX_TEMPLATES) return false;
  tplstore.begin(TPL_NS,false);
  char kn[8], kj[8], kb[8]; snprintf(kn,sizeof(kn),"n%u",slot); snprintf(kj,sizeof(kj),"j%u",slot); snprintf(kb,sizeof(kb),"b%u",slot);
  tplstore.remove(kn); tplstore.remove(kj); tplstore.remove(kb);
  tplstore.end(); return true;
}

// ===== WiFi/AP/OTA =====
void startAPAlways(){
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(cfg.net.apSsid.c_str(), cfg.net.apPass.c_str());
  Serial.printf("[AP] %s / %s IP: %s\n", cfg.net.apSsid.c_str(), cfg.net.apPass.c_str(), WiFi.softAPIP().toString().c_str());
}
void applyNetConfig(){
  if(!cfg.net.dhcp){
    if(!WiFi.config(cfg.net.ip,cfg.net.gw,cfg.net.mask,cfg.net.dns1,cfg.net.dns2)){
      Serial.println("[WiFi] Static IP config failed");
    }
  }
}
void startOTA(){ ArduinoOTA.setHostname(cfg.net.hostname.c_str()); ArduinoOTA.begin(); Serial.printf("[OTA] Ready: %s.local\n", cfg.net.hostname.c_str()); }
void connectSTA(){
  WiFi.mode(WIFI_AP_STA);
  if(cfg.net.hostname.length()) WiFi.setHostname(cfg.net.hostname.c_str());
  applyNetConfig();
  if(cfg.net.ssid.length()) WiFi.begin(cfg.net.ssid.c_str(), cfg.net.pass.c_str()); else WiFi.begin();
  unsigned long t0=millis(); Serial.print("[WiFi] STA connect");
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000){ delay(500); Serial.print("."); }
  Serial.println(); if(WiFi.status()==WL_CONNECTED){ Serial.printf("[WiFi] Connected: %s RSSI=%d IP=%s\n", WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str()); }
  startOTA();
}

// ===== Strips init =====
void initStrips(){
  for(int i=0;i<MAX_PORTS;i++){ if(strips[i]){ delete strips[i]; strips[i]=nullptr; } if(fx[i]){ delete fx[i]; fx[i]=nullptr; } }
  for(int i=0;i<MAX_PORTS;i++){
    if(!cfg.ports[i].enabled) continue;
    uint16_t n=cfg.ports[i].ledCount; if(n==0) continue;
    if(cfg.ports[i].model==LED_RGB){
      auto sn=new StripNeo(n,cfg.ports[i].dataPin,false,NEO_GRB+NEO_KHZ800);
      switch(cfg.ports[i].order){
        case ORDER_RGB: sn->updateType(NEO_RGB+NEO_KHZ800); break;
        case ORDER_GRB: sn->updateType(NEO_GRB+NEO_KHZ800); break;
        case ORDER_BGR: sn->updateType(NEO_BGR+NEO_KHZ800); break;
        case ORDER_GBR: sn->updateType(NEO_GBR+NEO_KHZ800); break;
        case ORDER_RBG: sn->updateType(NEO_RBG+NEO_KHZ800); break;
        case ORDER_BRG: sn->updateType(NEO_BRG+NEO_KHZ800); break;
      } strips[i]=sn;
    } else if(cfg.ports[i].model==LED_RGBW){
      strips[i]=new StripNeo(n,cfg.ports[i].dataPin,true,NEO_GRBW+NEO_KHZ800);
    } else if(cfg.ports[i].model==LED_APA102){
      strips[i]=new StripAPA(n,cfg.ports[i].dataPin,cfg.ports[i].clkPin);
    } else {
      strips[i]=new StripLPD(n,cfg.ports[i].dataPin,cfg.ports[i].clkPin);
    }
    strips[i]->begin(); strips[i]->setBrightness(cfg.globalBrightness);
    // clear to black
    for(uint16_t px=0;px<n;px++) strips[i]->setPixel(px,0,0,0); strips[i]->show(); delay(3);
    for(uint16_t px=0;px<n;px++) strips[i]->setPixel(px,0,0,0); strips[i]->show();
    if(cfg.ports[i].model == LED_RGB || cfg.ports[i].model == LED_RGBW){
      uint8_t t = (cfg.ports[i].model==LED_RGBW) ? (NEO_GRBW+NEO_KHZ800) : (NEO_GRB+NEO_KHZ800);
      fx[i] = new WS2812FX(n, cfg.ports[i].dataPin, t);
      fx[i]->init(); fx[i]->setBrightness(cfg.globalBrightness); fx[i]->stop();
    }
  }
}

// ===== Art-Net + sACN =====
uint8_t udpArtnetBuf[1536], udpSACNBuf[1536];

void applyDMXToPort(int p,const uint8_t* data,uint16_t dlen,uint16_t universe){
  if(!cfg.ports[p].enabled || !strips[p]) return;
  if(fx[p]) fx[p]->stop();
  pat[p].pat = PAT_NONE;

  uint8_t bpp=bytesPerPixel_u8(cfg.ports[p].model); uint16_t ppu=pixelsPerUniverse_u8(cfg.ports[p].model);
  uint16_t su=cfg.ports[p].startUniverse; if(universe<su) return; uint16_t uOff=universe-su;
  uint32_t firstPixel=(uint32_t)uOff*ppu; if(firstPixel>=cfg.ports[p].ledCount) return;
  uint16_t maxPixels=min<uint32_t>(ppu, (uint32_t)(cfg.ports[p].ledCount-firstPixel));
  uint16_t havePixels=min<uint16_t>(maxPixels,(uint16_t)(dlen/bpp));
  for(uint16_t i=0;i<havePixels;i++){
    uint16_t di=i*bpp;
    if(cfg.ports[p].model==LED_RGBW){
      strips[p]->setPixel(firstPixel+i, data[di+0],data[di+1],data[di+2],data[di+3]);
    } else {
      uint8_t r=data[mapIndex_u8(cfg.ports[p].order,0)+di];
      uint8_t g=data[mapIndex_u8(cfg.ports[p].order,1)+di];
      uint8_t b=data[mapIndex_u8(cfg.ports[p].order,2)+di];
      strips[p]->setPixel(firstPixel+i, r,g,b);
    }
  }
}

void handleArtnetPacket(const uint8_t* buf,size_t len,const IPAddress& src){
  if(len<18) return; if(memcmp(buf,ARTNET_ID,7)!=0||buf[7]!=0x00) return;
  uint16_t opcode=buf[8]|(buf[9]<<8); if(opcode!=OPCODE_ARTDMX) return;
  uint8_t seq = buf[12];
  uint16_t universe=buf[14]|(buf[15]<<8); uint16_t dlen=(buf[16]<<8)|buf[17]; if(18+dlen>len) return;
  const uint8_t* data=buf+18;
  if(seq!=0 && seq!=g_sync.lastSeq){
    for(int p=0;p<MAX_PORTS && p<8;p++){ if(g_sync.uniMaskExpected[p]!=0) g_sync.uniMask[p]=0; }
    g_sync.lastSeq = seq;
  } for(int p=0;p<MAX_PORTS;p++) applyDMXToPort(p,data,dlen,universe);
  dmxNoteUniverseArrived(universe);
dmxActive=true; lastDmxMs=millis(); }
bool parseE131(const uint8_t* buf,size_t len,uint16_t& universe,const uint8_t*& data,uint16_t& dlen){
  if(len<126) return false;
  if(buf[0]!=0x00||buf[1]!=0x10) return false;
  if(memcmp(buf+4,"ASC-E1.17",10)!=0) return false;
  uint32_t vecRoot=(buf[18]<<24)|(buf[19]<<16)|(buf[20]<<8)|buf[21]; if(vecRoot!=0x00000004) return false;
  uint32_t vecFrame=(buf[40]<<24)|(buf[41]<<16)|(buf[42]<<8)|buf[43]; if(vecFrame!=0x00000002) return false;
  universe=(buf[113]<<8)|buf[114]; if(buf[117]!=0x02) return false;
  uint16_t pvc=(buf[123]<<8)|buf[124]; if(pvc==0) return false; const uint8_t* prop=buf+125; if(prop[0]!=0x00) return false;
  data=prop+1; dlen=pvc-1; return true;
}

// ===== UI (HTML/CSS/JS) =====
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PIXIDRIVER Pro</title>
<style>
:root{
  --bg:#0b0f14; --panel:#0f1623; --pane2:#121b2a; --accent:#22d3ee; --accent2:#a78bfa;
  --text:#e6eef7; --mut:#8fa6c2; --line:#203047; --ok:#34d399; --warn:#f59e0b; --bad:#ef4444;
}
*{box-sizing:border-box}
html{font-size:18px}
@media (min-width:1600px){ html{font-size:19px} }
@media (min-width:1900px){ html{font-size:20px} }
body{margin:0;background:linear-gradient(135deg,#0a0f17,#0b1220 40%,#0a0f17);color:var(--text);font-family:Inter,system-ui,Segoe UI,Roboto,Ubuntu}
header{display:flex;flex-direction:column;gap:8px;align-items:center;padding:16px 18px;border-bottom:1px solid var(--line);backdrop-filter:blur(3px);background:#0b0f14a8;position:sticky;top:0;z-index:5}
h1{margin:0;font-size:24px;letter-spacing:.6px;text-align:center}
.badges{display:flex;gap:8px;flex-wrap:wrap;justify-content:center}
.badge{display:inline-flex;gap:8px;align-items:center;padding:6px 12px;border-radius:12px;background:#0c1320;border:1px solid var(--line);color:var(--mut);font-size:12px}

#wrapper{display:grid;grid-template-columns:280px 1fr;gap:16px;max-width:1800px;margin:16px auto;padding:0 16px}
#side{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:10px}
#main{display:grid;gap:16px}
.nav-title{padding:10px 12px;color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.1em}
.nav{display:flex;flex-direction:column;gap:8px}
.tab{display:flex;align-items:center;gap:10px;padding:12px 14px;border-radius:12px;border:1px solid var(--line);background:#0c1422;color:var(--text);cursor:pointer;transition:.15s}
.tab:hover{transform:translateY(-1px)}
.tab.active{outline:2px solid var(--accent)}
.card{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:18px}
.card h3{margin:0 0 12px 0;color:var(--accent2);font-size:18px}
label{display:block;margin:8px 0 4px 0;font-size:14px;color:var(--mut)}
input,select,button{width:100%;padding:12px 14px;border-radius:12px;border:1px solid var(--line);background:#0c1422;color:var(--text);font-size:15px}
button{background:var(--accent);color:#061219;border:0;font-weight:700;cursor:pointer;transition:.15s}
button:hover{filter:brightness(1.1)}
button.ghost{background:transparent;border:1px solid var(--line);color:var(--text)}
button.sm{padding:8px 12px;border-radius:10px;font-size:13px}
.hr{border-top:1px solid var(--line);margin:12px 0}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.grid3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px}
.hidden{display:none !important}
.badge2{display:inline-flex;gap:6px;align-items:center;padding:4px 10px;border-radius:999px;border:1px solid var(--line);background:#0c1422;color:var(--mut);font-size:12px}

/* Outputs table */
.table-wrap{overflow:auto}
table{width:100%;border-collapse:separate;border-spacing:0 10px;min-width:1100px}
th,td{text-align:left;padding:12px 12px}
th{color:var(--mut);font-weight:600;font-size:12px}
tr{background:#0d1728;border:1px solid var(--line)}
tr td:first-child{border-top-left-radius:12px;border-bottom-left-radius:12px}
tr td:last-child{border-top-right-radius:12px;border-bottom-right-radius:12px}
.col-center{text-align:center}

/* Buttons grids */
.btn-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}
.tpl-btn{position:relative;display:flex;align-items:center;justify-content:center;height:74px;border-radius:14px;border:1px solid var(--line);background:#111a2b;color:#fff;font-weight:700;font-size:16px;cursor:pointer;transition:.15s}
.tpl-btn:hover{transform:translateY(-1px);filter:brightness(1.06)}
.tpl-wrap{position:relative}
.tpl-dots{position:absolute;right:8px;top:8px;padding:4px 10px;border-radius:999px;border:1px solid var(--line);background:#0c1422;color:var(--text);cursor:pointer;font-size:12px;opacity:0;transition:.15s}
.tpl-wrap:hover .tpl-dots{opacity:1}
@media (max-width: 900px){ .tpl-dots{opacity:1} } /* mobile: always visible */

/* Sliders */
.slider{appearance:none;width:100%;height:10px;border-radius:999px;background:linear-gradient(90deg,#f00,#ff0,#0f0,#0ff,#00f,#f0f,#f00)}

.info{display:flex;gap:8px;flex-wrap:wrap;justify-content:flex-start}
.kv{display:flex;align-items:center;gap:6px;padding:6px 10px;border:1px solid var(--line);border-radius:12px;background:#0d1728;font-size:12px;color:var(--mut)}
.status{color:var(--ok)}
.warn{color:var(--warn)}

/* Responsive */
.hrow{display:flex;justify-content:space-between;align-items:center;width:100%}
.hrow .title{display:flex;align-items:center;gap:10px}
.hamb{display:none}
@media (max-width: 980px){
  #wrapper{ grid-template-columns: 1fr; }
  #side{ position: fixed; top: 92px; left: 0; right: 0; z-index: 40; transform: translateY(-130%); transition: .2s; }
  #side.open{ transform: translateY(0); }
  .hamb{display:inline-block}
  .btn-grid{ grid-template-columns: repeat(2,minmax(0,1fr)); }
  header{gap:6px}
  h1{font-size:20px}
  .badges{justify-content:flex-start}
}
@media (max-width: 560px){
  .btn-grid{ grid-template-columns: 1fr; }
  .grid2, .grid3{ grid-template-columns: 1fr; }
}

/* Modal */
.modal{position:fixed;inset:0;background:#0007;display:flex;align-items:center;justify-content:center;z-index:50}

/* Header adjustments */
header{display:flex;align-items:center;justify-content:center;gap:12px;padding:14px 18px;border-bottom:1px solid var(--line);position:relative}
header .title{margin:0;font-size:22px;letter-spacing:.5px;text-align:center}
.burger{display:none}
@media(max-width:520px){ header{flex-direction:column;align-items:center} .burger{display:inline-block;margin-top:8px;align-self:center} } .burger{display:none} }

/* Home metrics */
.home-metrics{display:flex;flex-direction:column;gap:14px;max-width:520px}
.metric{padding:16px;border:1px solid var(--line);border-radius:14px;background:#0d1728}
.metric .label{font-size:12px;color:var(--mut);margin-bottom:6px}
.metric .value{font-size:20px;font-weight:700}

/* Sidebar responsive */
@media(max-width:1024px){ #wrapper{grid-template-columns:1fr} #side{position:fixed;left:12px;top:68px;z-index:5;transform:translateX(-120%);transition:.2s;width:260px;max-height:75vh;overflow:auto} #side.open{transform:none} main{margin-top:8px} }

#effect-grid.btn-grid{grid-template-columns:repeat(5,minmax(0,1fr));}
#effect-grid .tpl-btn{aspect-ratio:1/1; height:auto; min-height:96px; border-radius:14px;}

/* Effects grid: 10 per row, square buttons, hover & active */
#effect-grid.btn-grid{grid-template-columns:repeat(10,minmax(0,1fr));gap:12px}
#effect-grid .tpl-btn{aspect-ratio:1/1; height:auto; min-height:84px; border-radius:14px; transition:transform .15s ease, box-shadow .15s ease, outline-color .15s ease}
#effect-grid .tpl-btn:hover{transform:translateY(-2px) scale(1.02); box-shadow:0 6px 18px rgba(0,0,0,.35); outline:2px solid var(--accent)}
#effect-grid .tpl-btn.active{outline:2px solid var(--accent)}


/* Effects grid — 10 per row on desktop, no placeholder squares */
#effect-grid.btn-grid{grid-template-columns:repeat(10,minmax(0,1fr));gap:12px}
@media (max-width: 1400px){#effect-grid.btn-grid{grid-template-columns:repeat(8,minmax(0,1fr));}}
@media (max-width: 1100px){#effect-grid.btn-grid{grid-template-columns:repeat(6,minmax(0,1fr));}}
@media (max-width: 900px){#effect-grid.btn-grid{grid-template-columns:repeat(4,minmax(0,1fr));}}
@media (max-width: 640px){#effect-grid.btn-grid{grid-template-columns:repeat(3,minmax(0,1fr));}}
@media (max-width: 480px){#effect-grid.btn-grid{grid-template-columns:repeat(2,minmax(0,1fr));}}

/* Square buttons with rounded corners, same for built-in and WS2812FX */
#effect-grid .tpl-btn{aspect-ratio:1/1; height:auto; min-height:84px; border-radius:14px; transition:transform .15s ease, box-shadow .15s ease, outline-color .15s ease}
#effect-grid .tpl-btn:hover{transform:translateY(-2px) scale(1.02); box-shadow:0 6px 18px rgba(0,0,0,.35); outline:2px solid var(--accent)}
#effect-grid .tpl-btn.active{outline:2px solid var(--accent)}


/* --- EFFECTS GRID: hard 10 per row on desktop, unified button look, no placeholders --- */
#effect-grid{display:grid !important;grid-template-columns:repeat(10,minmax(0,1fr)) !important;gap:12px !important}
@media (max-width: 1400px){#effect-grid{grid-template-columns:repeat(8,minmax(0,1fr)) !important}}
@media (max-width: 1100px){#effect-grid{grid-template-columns:repeat(6,minmax(0,1fr)) !important}}
@media (max-width: 900px){#effect-grid{grid-template-columns:repeat(4,minmax(0,1fr)) !important}}
@media (max-width: 640px){#effect-grid{grid-template-columns:repeat(3,minmax(0,1fr)) !important}}
@media (max-width: 480px){#effect-grid{grid-template-columns:repeat(2,minmax(0,1fr)) !important}}

/* Square + rounded for ALL effect buttons (built-in + WS2812FX) */
#effect-grid .tpl-btn, #effect-grid .wsfx-btn{
  aspect-ratio:1/1;
  height:auto;
  min-height:84px;
  border-radius:14px;
  transition:transform .15s ease, box-shadow .15s ease, outline-color .15s ease;
}
#effect-grid .tpl-btn:hover, #effect-grid .wsfx-btn:hover{
  transform:translateY(-2px) scale(1.02);
  box-shadow:0 6px 18px rgba(0,0,0,.35);
  outline:2px solid var(--accent);
}
#effect-grid .tpl-btn.active, #effect-grid .wsfx-btn.active{outline:2px solid var(--accent)}


/* Hide any legacy wsfx grid if still present */



/* Merge WS2812FX and built-in effects into a single grid */
#wsfx-section{display:none !important}


.slider.plain{background:linear-gradient(90deg,var(--line),var(--line)) !important}

.ok{color:var(--ok)}
</style></head>
<body>
<header>
  <button class="burger" onclick="toggleSide()">☰</button>
  <h1 class="title">PIXIDRIVER Pro</h1>
</header>

<div id="wrapper">
  <aside id="side">
    <div class="nav-title">Navigation</div>
    <div class="nav">
      <div class="tab active" data-tab="home">🏠 Home</div>
      <div class="tab" data-tab="net">🌐 Network</div>
      <div class="tab" data-tab="outs">🔌 Outputs</div>
      <div class="tab" data-tab="fx">✨ Effects</div>
      <div class="tab" data-tab="tpl">💾 Templates</div>
      <div class="tab" data-tab="info">📡 Art-Net</div>
    </div>
  </aside>

  <main id="main">
    <!-- HOME -->
    <section class="card tabpane" id="tab-home">
      <h3>System</h3>
      <div class="home-metrics">
        <div class="metric"><div class="label">IP</div><div id="m-ip" class="value">-</div></div>
        <div class="metric"><div class="label">SSID</div><div id="m-ssid" class="value">-</div></div>
        <div class="metric"><div class="label">RSSI</div><div id="m-rssi" class="value">-</div></div>
        <div class="metric"><div class="label">CPU Load</div><div id="m-cpu" class="value">-</div></div>
      </div>
    </section>

    <!-- NETWORK -->
    <section class="card tabpane hidden" id="tab-net">
      <h3>Network</h3>
      <div class="grid2">
        <div><label>Hostname</label><input id="hostname" placeholder="pixidriver"></div>
        <div><label>Art-Net Port</label><input id="aport" type="number" min="1" max="65535" value="6454"></div>
      </div>
      <div class="grid2">
        <div>
          <label>Input Protocol</label>
          <select id="proto">
            <option value="0">Auto (Art-Net & sACN)</option>
            <option value="1">Art-Net only</option>
            <option value="2">sACN (E1.31) only</option>
          </select>
        </div>
        <div></div>
      </div>
      
      <label>Networks</label><select id="wifi-list"></select>
      <div style="margin-top:10px"><button onclick="scanWiFi()">Scan Wi-Fi</button></div>
      <div class="grid2">
        <div><label>Password</label><input id="wifi-pass" type="password" placeholder="••••••••"></div>
        <div style="display:flex;align-items:end"><button onclick="saveWiFi()">Save & Reboot</button></div>
      </div>
      <div class="hr"></div>
      <label><input id="dhcp" type="checkbox"> Use DHCP</label>
      <div class="grid3"><div><label>Static IP</label><input id="ipaddr"></div><div><label>Gateway</label><input id="gw"></div><div><label>Subnet</label><input id="mask"></div></div>
      <div class="grid2"><div><label>DNS 1</label><input id="dns1"></div><div><label>DNS 2</label><input id="dns2"></div></div>
      <div class="hr"></div>
      <div class="grid2"><div><label>AP SSID</label><input id="ap-ssid" placeholder="PIXIDRIVER"></div><div><label>AP Password</label><input id="ap-pass" type="password" placeholder="smartspot"></div></div>
      <div id="wifi-msg" class="kv"></div>
    </section>

    <!-- OUTPUTS -->
    <section class="card tabpane hidden" id="tab-outs">
      <h3>Outputs</h3>
      <div class="table-wrap">
        <table>
          <colgroup>
            <col style="width:60px"><col style="width:70px"><col style="width:220px">
            <col style="width:120px"><col style="width:120px"><col style="width:150px"><col style="width:140px"><col style="width:140px">
          </colgroup>
          <thead><tr><th>#</th><th class="col-center">En</th><th>Model</th><th>Data</th><th>Clock</th><th>Order</th><th>Universe</th><th>LEDs</th></tr></thead>
          <tbody id="output-table"></tbody>
        </table>
      </div>
      <div class="hr"></div>
      <label>Global Brightness <span id="gbr-pct" class="muted">100%</span></label>
      <input id="gbr" class="slider plain" type="range" min="0" max="100" value="100" oninput="thGbrPct(this.value)">
      <div style="display:flex;gap:10px;flex-wrap:wrap"><button class="ghost sm" onclick="reload()">Reload</button><button class="sm" onclick="saveOutputs()">Save & Reboot</button></div>
      <div id="cfg-msg" class="kv"></div>
    </section>

    <!-- EFFECTS -->
    <section class="card tabpane hidden" id="tab-fx">
      <h3>Effects</h3>
      <div class="btn-grid" id="effect-grid"></div>
      <div class="hr"></div>
      <div class="btn-grid" id="wsfx-grid"></div>
      <div class="grid2" style="margin-top:10px">
        <div><label>Speed</label><input id="pat-speed" type="range" min="1" max="200" value="80" oninput="thSpeed(this.value)"></div>
        <div><label>Mirror</label>
          <select id="pat-mirror" onchange="setMirror()">
            <option value="0">Off</option><option value="1">On</option>
          </select>
        </div>
      </div>
      <div class="grid2">
        <div><label>Output</label><select id="pat-output"><option value="all">All</option><option>1</option><option>2</option><option>3</option><option>4</option></select></div>
        <div style="display:flex;align-items:end;gap:10px">
          <button class="sm" onclick="startPat()">Start</button>
          <button class="ghost sm" onclick="stopPat()">Stop</button>
        </div>
      </div>
      <div class="hr"></div>
      <div class="grid2">
        <div><label>Hue</label><input id="hue" class="slider" type="range" min="0" max="359" value="240" oninput="thHue(this.value)"></div>
        <div><label>Color</label><input id="colorpick" type="color" value="#0000ff" oninput="setColorPicker(this.value)"></div>
      </div>
      <div class="badge2">Tap a button to start instantly.  modes also start on click.</div>
    </section>

    <!-- TEMPLATES -->
    <section class="card tabpane hidden" id="tab-tpl">
      <h3>Templates</h3>
      <div class="grid3">
        <div><label>Name</label><input id="tpl-name" placeholder="My Effect"></div>
        <div><label>Button Color</label><input id="tpl-color" type="color" value="#1f6feb"></div>
        <div style="display:flex;align-items:end"><button onclick="saveTpl()">Save Template</button></div>
      </div>
      <div class="hr"></div>
      <div class="btn-grid" id="tpls"></div>
    </section>

    <!-- INFO -->
    
    <!-- INFO -->
    <section class="card tabpane hidden" id="tab-info">
      <h3>📡 Art-Net</h3>
      <div class="home-metrics">
        <div class="metric"><div class="label">Protocol</div><div class="value" id="info-proto">Auto</div></div>
        <div class="metric"><div class="label">DMX</div><div class="value" id="anet-st">idle</div></div>
        <div class="metric"><div class="label">Source IP</div><div class="value" id="anet-ip">-</div></div>
        <div class="metric"><div class="label">STA SSID</div><div class="value" id="info-ssid">-</div></div>
        <div class="metric"><div class="label">RSSI</div><div class="value"><span id="info-rssi">-</span> dBm</div></div>
      </div>
      <div class="hr"></div>
      <div style="display:flex;gap:10px;flex-wrap:wrap"><button class="ghost sm" onclick="reboot()">Reboot</button></div>
    </section>

  </main>
</div>

<!-- EDIT MODAL -->
<div id="tpl-modal" class="modal hidden" onclick="if(event.target===this) closeTplEdit()">
  <div class="card" style="width:min(560px,92vw)">
    <h3>Edit Template</h3>
    <div class="grid3">
      <div><label>Name</label><input id="tplm-name"></div>
      <div><label>Button Color</label><input id="tplm-color" type="color" value="#1f6feb"></div>
      <div><label>Output (opt.)</label><input id="tplm-out" placeholder="all / 1..4"></div>
    </div>
    <div class="hr"></div>
    <div style="display:flex;gap:10px;flex-wrap:wrap">
      <button class="sm" onclick="event.stopPropagation(); saveTplEdit()">Save</button>
      <button class="ghost sm" onclick="event.stopPropagation(); delTplEdit()">Delete</button>
      <button class="ghost sm" onclick="event.stopPropagation(); closeTplEdit()">Close</button>
    </div>
  </div>
</div>

<script>
const EFFECTS=[["solid","Solid"],["rainbow","Rainbow"],["colorwipe","Color Wipe"],["blink","Blink"],["fade","Fade"],["theater","Theater"],["comet","Comet"],["twinkle","Twinkle"],["wave","Wave"],["wsfx",""]];
let wsfxModes=[];
function loadWsfxModes(){fetch("/wsfx/modes").then(r=>r.json()).then(list=>{wsfxModes=Array.isArray(list)?list:[]; buildEffects(); }).catch(_=>{ wsfxModes=[]; buildEffects(); });}
let activeEffect="solid", hue=240, color="#0000ff";
function $(id){return document.getElementById(id);}
function toggleSide(){ $("side").classList.toggle("open"); }
document.querySelectorAll('.tab').forEach(t=>{t.addEventListener('click',()=>{
  document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));t.classList.add('active');
  const tab=t.dataset.tab;document.querySelectorAll('.tabpane').forEach(p=>p.classList.add('hidden'));$("tab-"+tab).classList.remove('hidden');
  // mobile: close sidebar after selection
  if(window.matchMedia("(max-width: 980px)").matches){ $("side").classList.remove("open"); }
});});

function hsvToRgb(h,s,v){let c=v*s,x=c*(1-Math.abs((h/60)%2-1)),m=v-c,r=0,g=0,b=0;if(h<60){r=c;g=x;b=0;}else if(h<120){r=x;g=c;b=0;}else if(h<180){r=0;g=c;b=x;}else if(h<240){r=0;g=x;b=c;}else if(h<300){r=x;g=0;b=c;}else{r=c;g=0;b=x;}return [Math.round((r+m)*255),Math.round((g+m)*255),Math.round((b+m)*255)];}
function rgbToHex(r,g,b){return "#"+[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('');}
function hueToHex(h){const [r,g,b]=hsvToRgb(parseInt(h)||0,1,1);return rgbToHex(r,g,b);}

function buildEffects(){
  const g=$("effect-grid"); if(!g) return; g.innerHTML="";
  const wsfx = (wsfxModes||[]).map((m,i)=>({id:`wsfx:${i}`, label:(m&&m.name)?m.name:(`WSFX ${i+1}`)}));
  const built = (EFFECTS||[]).map(([id,label])=>({id,label}));
  const all = [...built, ...wsfx];
  all.forEach(({id,label})=>{
    const b=document.createElement('button');
    b.className='tpl-btn';
    b.textContent=label;
    if(id===activeEffect) b.classList.add('active');
    b.onclick=()=>{
      activeEffect=id;
      document.querySelectorAll('#effect-grid .tpl-btn').forEach(x=>x.classList.remove('active'));
      b.classList.add('active');
      if(id.startsWith('wsfx:')){
        const idx=parseInt(id.split(':')[1]||'0')||0;
        fetch('/wsfx/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:idx})});
      }else{
        startPat();
      }
    };
    g.appendChild(b);
  });
}

let tHue,tSpeed,tGbr;
function setHue(v){hue=parseInt(v)||0; color=hueToHex(hue); sendColor();}
function thHue(v){ if(tHue) cancelAnimationFrame(tHue); tHue=requestAnimationFrame(()=>setHue(v)); }
function setColorPicker(val){ color=val; sendColor(); }
function thSpeed(v){ if(tSpeed) cancelAnimationFrame(tSpeed); tSpeed=requestAnimationFrame(()=>fetch('/pattern',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'speed',value:parseInt(v)})})); }
function thGbr(v){ if(tGbr) cancelAnimationFrame(tGbr); tGbr=requestAnimationFrame(()=>fetch('/brightness',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({value:parseInt(v)})})); }

function startPat(){
  const body={action:'start',output:$("pat-output").value,pattern:activeEffect,color:color,speed:parseInt($("pat-speed").value),mirror:parseInt($("pat-mirror").value)};
  fetch('/pattern',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
}
function stopPat(){ fetch('/pattern',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'stop',output:$("pat-output").value})}); }
function setMirror(){ fetch('/pattern',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'mirror',value:parseInt($("pat-mirror").value)})}); }
function sendColor(){ fetch('/pattern',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'color',output:$("pat-output").value,color:color})}); }

function outputsUI(s){
  const orders=["RGB","GRB","BGR","GBR","RBG","BRG"];
  const models=["WS28xx RGB","SK6812 RGBW","APA102 SPI","LPD8806 SPI"];
  const tb=$("output-table"); tb.innerHTML="";
  for(let i=0;i<s.outputs.length;i++){
    const o=s.outputs[i];
    const tr=document.createElement('tr');
    tr.innerHTML=`
      <td class="col-center"><b>${i+1}</b></td>
      <td class="col-center"><input type="checkbox" ${o.en?'checked':''} data-i="${i}" data-k="en"></td>
      <td><select data-i="${i}" data-k="model">${models.map((m,mi)=>`<option value="${mi}" ${o.model==mi?'selected':''}>${m}</option>`).join('')}</select></td>
      <td><input type="number" value="${o.dp}" data-i="${i}" data-k="dp"></td>
      <td><input type="number" value="${o.cp}" data-i="${i}" data-k="cp"></td>
      <td><select data-i="${i}" data-k="order">${orders.map((nm,oi)=>`<option value="${oi}" ${o.order==oi?'selected':''}>${nm}</option>`).join('')}</select></td>
      <td><input type="number" min="0" max="4095" value="${o.universe}" data-i="${i}" data-k="uni"></td>
      <td><input type="number" value="${o.leds}" data-i="${i}" data-k="leds"></td>`;
    tb.appendChild(tr);
  }
}
function collectOutputs(){
  const fields=document.querySelectorAll('#output-table input,#output-table select');
  const arr=[{},{},{},{}]; fields.forEach(f=>{const i=parseInt(f.dataset.i); const k=f.dataset.k; if(isNaN(i)) return; if(f.type==="checkbox") arr[i][k]=f.checked; else arr[i][k]=parseInt(f.value);});
  return arr.map((o,i)=>({en:!!o.en,model:o.model??0,dp:o.dp??0,cp:o.cp??0,order:o.order??1,universe:(isNaN(o.uni)?i:o.uni),leds:o.leds??60}));
}
function saveOutputs(){ const body={outputs:collectOutputs()}; fetch('/saveOutputs',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).then(()=>{$("cfg-msg").textContent="Saved. Rebooting…"; setTimeout(()=>location.reload(),8000);}); }

function scanWiFi(){
  fetch('/scan').then(r=>r.json()).then(list=>{
    const s=$("wifi-list"); s.innerHTML='';
    if(!list || !list.length){ const o=document.createElement('option'); o.value=''; o.text='(No networks found)'; s.add(o); return; }
    list.forEach(n=>{ const o=document.createElement('option'); o.value=n.ssid; o.text=`${n.ssid} (${n.rssi} dBm)`; s.add(o); });
  });
}
function saveWiFi(){
  const b=new URLSearchParams();
  b.set('hostname',$("hostname").value); b.set('aport',$("aport").value);
  b.set('proto',$("proto").value);
  b.set('ssid',$("wifi-list").value); b.set('pass',$("wifi-pass").value);
  b.set('dhcp',$("dhcp").checked?'1':'0');
  b.set('ip',$("ipaddr").value); b.set('gw',$("gw").value); b.set('mask',$("mask").value); b.set('dns1',$("dns1").value); b.set('dns2',$("dns2").value);
  b.set('apssid',$("ap-ssid").value); b.set('appass',$("ap-pass").value);
  fetch('/saveNet',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b.toString()}).then(()=>{$("wifi-msg").textContent="Rebooting in 10s…"; setTimeout(()=>location.reload(),10000);});
}

let _tplEditIdx=-1, _tplCache=[];
function openTplEdit(idx){
  _tplEditIdx=idx;
  const it=_tplCache[idx]; if(!it) return;
  $("tplm-name").value=it.name||""; $("tplm-color").value=it.btn||"#1f6feb"; $("tplm-out").value=(it.output||"");
  $("tpl-modal").classList.remove("hidden");
}
function closeTplEdit(){ $("tpl-modal").classList.add("hidden"); _tplEditIdx=-1; }
function saveTplEdit(){
  if(_tplEditIdx<0) return;
  const body={slot:_tplEditIdx,name:$("tplm-name").value,btn:$("tplm-color").value,output:$("tplm-out").value};
  fetch('/templates/update',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).then(()=>{closeTplEdit(); loadTemplates('tpls'); loadTemplates('home-templates');});
}
function delTplEdit(){
  if(_tplEditIdx<0) return;
  fetch('/templates/delete/'+_tplEditIdx,{method:'POST'}).then(()=>{closeTplEdit(); loadTemplates('tpls'); loadTemplates('home-templates');});
}

function loadTemplates(gridId){
  fetch('/templates').then(r=>r.json()).then(t=>{
    _tplCache=t;
    const g=$(gridId); g.innerHTML="";
    t.forEach((it,idx)=>{ if(!it||!it.name) return;
      const wrap=document.createElement('div'); wrap.className='tpl-wrap';
      const b=document.createElement('button'); b.className='tpl-btn'; b.textContent=it.name;
      b.style.background=it.btn||'#1f6feb'; b.onclick=()=>{ fetch('/templates/load/'+idx,{method:'POST'}); };
      const dots=document.createElement('button'); dots.className='tpl-dots'; dots.textContent='⋯'; dots.title='Edit template';
      dots.onclick=(e)=>{ e.stopPropagation(); openTplEdit(idx); };
      wrap.appendChild(b); wrap.appendChild(dots); g.appendChild(wrap);
    });
  });
}
function saveTpl(){
  const payload={name:$("tpl-name").value, button:$("tpl-color").value, effect:activeEffect, color:color, speed:parseInt($("pat-speed").value), mirror:parseInt($("pat-mirror").value), output:$("pat-output").value};
  fetch('/templates/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}).then(()=>{loadTemplates('tpls'); loadTemplates('home-templates');});
}

//  MODES as buttons
function loadWsfxModes(){
  fetch('/wsfx').then(r=>r.json()).then(js=>{
    const g=$("effect-grid"); g.innerHTML="";
    (js.modes||[]).forEach(m=>{
      const b=document.createElement('button'); b.className='tpl-btn'; b.textContent=m.name||('Mode '+m.id);
      b.onclick=()=>{ activeEffect='wsfx'; document.querySelectorAll('#effect-grid .tpl-btn').forEach(x=>x.style.outline=''); startWsfx(m.id); };
      g.appendChild(b);
    });
  });
}
function startWsfx(mode){
  const body={action:'start',output:$("pat-output").value,pattern:'wsfx',color:color,speed:parseInt($("pat-speed").value),mirror:parseInt($("pat-mirror").value),mode:mode};
  fetch('/pattern',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
}

function updateInfo(s){
  if ($("m-ip"))   $("m-ip").textContent   = (s && s.ip) ? s.ip : "-";
  var ssid = (s && s.net && s.net.ssid) ? s.net.ssid : "-";
  var rssi = (s && s.net && (typeof s.net.rssi !== "undefined")) ? (s.net.rssi + " dBm") : "-";
  if ($("m-ssid")) $("m-ssid").textContent = ssid;
  if ($("m-rssi")) $("m-rssi").textContent = rssi;
  var cpu = (s && s.cpu && (typeof s.cpu.loadPct !== "undefined")) ? Math.round(s.cpu.loadPct) + "%" : "-";
  if ($("m-cpu"))  $("m-cpu").textContent  = cpu;
}

function reload(){ loadState(); }

function loadState(){
  fetch('/state').then(r=>r.json()).then(s=>{ updateInfo(s); updateInfoArtNet(s); outputsUI(s); buildEffects(); loadWsfxModes();  updateInfo(s); updateInfoArtNet(s); outputsUI(s); buildEffects(); if(typeof loadWsfxModes==='function') loadWsfxModes();  updateInfo(s); updateInfoArtNet(s);
    $("ip").textContent="IP: "+(s.ip||'-'); $("ssid").textContent="SSID: "+(s.net.ssid||'-'); $("rssi").textContent="RSSI: "+(s.net.rssi||'-')+" dBm";
    $("hostname").value=s.net.hostname||'pixidriver'; $("aport").value=s.artnetPort||6454;
    $("proto").value = (""+ ( (s.proto!==undefined)? s.proto : 0 ));
    $("dhcp").checked=s.net.dhcp; $("ipaddr").value=s.net.ip; $("gw").value=s.net.gw; $("mask").value=s.net.mask; $("dns1").value=s.net.dns1; $("dns2").value=s.net.dns2;
    $("ap-ssid").value=s.net.apSsid||'PIXIDRIVER'; $("ap-pass").value=s.net.apPass||'smartspot';
    $("gbr").value = Math.round(((s.globalBrightness||220) * 100) / 255);
  if ($("gbr-pct")) $("gbr-pct").textContent = $("gbr").value + "%";
    outputsUI(s); buildEffects(); loadTemplates('tpls'); loadTemplates('home-templates'); loadWsfxModes(); updateInfo(s); updateInfoArtNet(s);
  });
}
function factoryReset(){ fetch('/factory',{method:'POST'}); }
function reboot(){ fetch('/restart',{method:'POST'}); }

loadState();

function thGbrPct(v){ var p=Math.max(0,Math.min(100,parseInt(v)||0)); var v255=Math.round(p*255/100); thGbr(v255);
  if ($("gbr-pct")) $("gbr-pct").textContent = (parseInt(v)||0) + "%";}

function updateInfoArtNet(s){
  var protoMap = {0:"Auto (Art-Net & sACN)",1:"Art-Net only",2:"sACN only"};
  if (document.getElementById("info-proto")) document.getElementById("info-proto").textContent = (s && typeof s.proto!=="undefined") ? (protoMap[s.proto]||("Mode "+s.proto)) : "-";
  var active = (s && s.artnet && s.artnet.active) ? true : false;
  if (document.getElementById("anet-st")) {
    var el = document.getElementById("anet-st");
    el.textContent = active ? "active" : "idle";
    el.className = "value " + (active ? "ok" : "warn");
  }
  if (document.getElementById("anet-ip")) document.getElementById("anet-ip").textContent = (s && s.artnet && s.artnet.src) ? s.artnet.src : "-";
  var ssid = (s && s.net && s.net.ssid) ? s.net.ssid : "-";
  var rssiNum = (s && s.net && (typeof s.net.rssi !== "undefined")) ? s.net.rssi : null;
  if (document.getElementById("info-ssid")) document.getElementById("info-ssid").textContent = ssid;
  if (document.getElementById("info-rssi")) document.getElementById("info-rssi").textContent = (rssiNum!==null) ? rssiNum : "-";
}
</script>
<footer>
  <div style="text-align:center;padding:8px 0;">
    <a href="https://github.com/smartspot-led/PIXIDRIVER-V1.0" target="_blank" rel="noopener" style="text-decoration:none;color:inherit">
      Powered by <strong>SMART SPOT</strong>
    </a>
  </div>
</footer>

</body></html>)HTML";

// ===== API helpers =====
bool parseIp(const String& s, IPAddress& out){ return out.fromString(s); }

// forward decls
void handleTplLoadPath(); void handleTplDeletePath(); void handleTplLoadQuery(); void handleTplDeleteQuery(); void handleTplUpdate(); void handleWsfx();

void sendIndex(){ server.send_P(200,"text/html", INDEX_HTML); }

// Wi-Fi scan
void handleScan(){
  WiFi.scanDelete();
  int n = WiFi.scanNetworks(false, true);
  DynamicJsonDocument doc(4096); JsonArray arr = doc.to<JsonArray>();
  for(int i=0;i<n;i++){ JsonObject o=arr.createNestedObject(); o["ssid"]=WiFi.SSID(i); o["rssi"]=WiFi.RSSI(i); }
  String out; serializeJson(arr,out); server.send(200,"application/json", out);
  WiFi.scanDelete();
}

void handleSaveNet(){
  cfg.net.hostname = server.arg("hostname").length()? server.arg("hostname") : "pixidriver";
  cfg.artnetPort = clampu16_any(server.arg("aport").toInt(), 65535);
  int psel = server.arg("proto").toInt(); if(psel<0||psel>2) psel=0; cfg.proto=(InputProto)psel;
  cfg.net.ssid = server.arg("ssid"); cfg.net.pass = server.arg("pass");
  cfg.net.dhcp = (server.arg("dhcp")=="1");
  IPAddress ip=cfg.net.ip,gw=cfg.net.gw,mask=cfg.net.mask,d1=cfg.net.dns1,d2=cfg.net.dns2;
  parseIp(server.arg("ip"),ip); parseIp(server.arg("gw"),gw); parseIp(server.arg("mask"),mask); parseIp(server.arg("dns1"),d1); parseIp(server.arg("dns2"),d2);
  cfg.net.ip=ip; cfg.net.gw=gw; cfg.net.mask=mask; cfg.net.dns1=d1; cfg.net.dns2=d2;
  String aps=server.arg("apssid"); String app=server.arg("appass"); if(aps.length()>=1) cfg.net.apSsid=aps; if(app.length()>=8) cfg.net.apPass=app;
  saveConfig(); server.send(200,"text/plain","ok"); Serial.println("[CFG] Network saved. Rebooting..."); delay(250); ESP.restart();
}

// brightness
void handleBrightness(){
  DynamicJsonDocument d(128); if(deserializeJson(d, server.arg("plain"))){ server.send(400,"text/plain","bad json"); return; }
  int v=d["value"]|cfg.globalBrightness; cfg.globalBrightness=clampu8(v);
  for(int i=0;i<MAX_PORTS;i++) if(strips[i]) strips[i]->setBrightness(cfg.globalBrightness);
  for(int i=0;i<MAX_PORTS;i++) if(fx[i]) fx[i]->setBrightness(cfg.globalBrightness);
  g_sync.pendingShow=true;
  prefs.begin(CFG_NS,false); prefs.putUChar("gbr",cfg.globalBrightness); prefs.end();
  server.send(200,"text/plain","ok");
}

// WS2812FX modes list
void handleWsfx(){
  WS2812FX* first=nullptr;
  for(int i=0;i<MAX_PORTS;i++){ if(fx[i]){ first=fx[i]; break; } }
  DynamicJsonDocument d(4096);
  JsonArray arr = d.createNestedArray("modes");
  if(first){
    uint8_t cnt = first->getModeCount();
    for(uint8_t i=0;i<cnt;i++){
      JsonObject o = arr.createNestedObject();
      o["id"]=i;
      o["name"]=first->getModeName(i);
    }
  }
  String out; serializeJson(d,out); server.send(200,"application/json", out);
}

// pattern ops
void handlePattern(){
  DynamicJsonDocument doc(512); if(deserializeJson(doc, server.arg("plain"))){ server.send(400,"text/plain","bad json"); return; }
  String action=doc["action"]|"start"; String oSel=doc["output"]|"all";
  int pStart=0,pEnd=MAX_PORTS-1; if(oSel!="all"){ int one=oSel.toInt(); if(one>=1&&one<=MAX_PORTS){ pStart=one-1; pEnd=one-1; } }

  if(action=="stop"){
    for(int p=pStart;p<=pEnd;p++){ pat[p].pat=PAT_NONE; if(fx[p]) fx[p]->stop(); }
    server.send(200,"text/plain","stopped"); return;
  
    g_sync.pendingShow=true;}
  if(action=="color"){
    String cs = doc["color"] | String("#000000");
    uint32_t col=parseHexColor(cs);
    for(int p=pStart;p<=pEnd;p++){ pat[p].color=col; if(fx[p]) fx[p]->setColor(col); }
    server.send(200,"text/plain","colored"); return;
  
    g_sync.pendingShow=true;}
  if(action=="speed"){
    int spd=doc["value"]|80;
    for(int p=0;p<MAX_PORTS;p++){ pat[p].speed=clampu8(spd); if(fx[p]) fx[p]->setSpeed( (int)((205 - clampu8(spd))*10) ); }
    server.send(200,"text/plain","speed"); return;
  
    g_sync.pendingShow=true;}
  if(action=="mirror"){ bool m=(int)doc["value"]==1; for(int p=0;p<MAX_PORTS;p++) pat[p].mirror=m; server.send(200,"text/plain","mirror"); return; 
    g_sync.pendingShow=true;}

  String pName=doc["pattern"]|"solid"; bool isWsfx = (pName=="wsfx");
  String cs = doc["color"] | String("#000000");
  uint32_t col=parseHexColor(cs); int spd=doc["speed"]|80; bool mir=(int)doc["mirror"]==1;

  if(isWsfx){
    int mode = doc["mode"] | 0;
    for(int p=pStart;p<=pEnd;p++){
      pat[p].pat=PAT_WSFX;
      if(fx[p]){
        fx[p]->setMode((uint8_t)mode);
        fx[p]->setColor(col);
        fx[p]->setSpeed( (int)((205 - clampu8(spd))*10) );
        fx[p]->start();
      }
    }
    server.send(200,"text/plain","wsfx"); return;
  }

  PatName pn=PAT_SOLID;
  if(pName=="rainbow") pn=PAT_RAINBOW; else if(pName=="colorwipe") pn=PAT_COLORWIPE; else if(pName=="blink") pn=PAT_BLINK; else if(pName=="fade") pn=PAT_FADE;
  else if(pName=="theater") pn=PAT_THEATER; else if(pName=="comet") pn=PAT_COMET; else if(pName=="twinkle") pn=PAT_TWINKLE; else if(pName=="wave") pn=PAT_WAVE;

  for(int p=pStart;p<=pEnd;p++){
    if(fx[p]) fx[p]->stop();
    pat[p].pat=pn; pat[p].color=col; pat[p].speed=clampu8(spd); pat[p].mirror=mir; pat[p].tLast=0; pat[p].step=0; pat[p].on=true;
  }
  server.send(200,"text/plain","started");
}

// save outputs
void handleSaveOutputs(){
  DynamicJsonDocument doc(4096); if(deserializeJson(doc, server.arg("plain"))){ server.send(400,"text/plain","bad json"); return; }
  JsonArray outs=doc["outputs"].as<JsonArray>();
  for(size_t i=0;i<outs.size()&&i<MAX_PORTS;i++){
    JsonObject o=outs[i];
    if(o.containsKey("en")) cfg.ports[i].enabled=o["en"];
    cfg.ports[i].dataPin=clampu8((int)o["dp"]);
    cfg.ports[i].clkPin=clampu8((int)o["cp"]);
    cfg.ports[i].model=(LedModel)clampu8((int)o["model"]);
    cfg.ports[i].order=(ColorOrder)clampu8((int)o["order"]);
    cfg.ports[i].startUniverse=clampu16_any((int)o["universe"],4095); // allow 0
    cfg.ports[i].ledCount=clampu16_any((int)o["leds"],20000);
  }
  saveConfig(); server.send(200,"text/plain","ok"); Serial.println("[CFG] Outputs saved. Rebooting..."); delay(250); ESP.restart();
}

// state
void handleState(){
  DynamicJsonDocument doc(4096);
  doc["ip"]=WiFi.isConnected()?WiFi.localIP().toString():WiFi.softAPIP().toString();
  doc["artnetPort"]=cfg.artnetPort; doc["proto"]=(uint8_t)cfg.proto;
  JsonObject net=doc.createNestedObject("net");
  net["hostname"]=cfg.net.hostname; net["dhcp"]=cfg.net.dhcp;
  net["ip"]=cfg.net.ip.toString(); net["gw"]=cfg.net.gw.toString(); net["mask"]=cfg.net.mask.toString();
  net["dns1"]=cfg.net.dns1.toString(); net["dns2"]=cfg.net.dns2.toString();
  net["ssid"]=WiFi.SSID(); net["rssi"]=WiFi.RSSI(); net["apSsid"]=cfg.net.apSsid; net["apPass"]=cfg.net.apPass;
  doc["globalBrightness"]=cfg.globalBrightness;
  JsonObject an=doc.createNestedObject("artnet"); an["active"]=dmxActive && (millis()-lastDmxMs<DMX_HOLD_MS); an["src"]=lastDMXSrc.toString();
  JsonObject cpu=doc.createNestedObject("cpu"); cpu["avgLoopMs"]=avgLoopMs; cpu["loadPct"]=loadPct;
  JsonArray outs=doc.createNestedArray("outputs");
  for(int i=0;i<MAX_PORTS;i++){ JsonObject o=outs.createNestedObject(); o["en"]=cfg.ports[i].enabled; o["dp"]=cfg.ports[i].dataPin; o["cp"]=cfg.ports[i].clkPin; o["model"]=cfg.ports[i].model; o["order"]=cfg.ports[i].order; o["universe"]=cfg.ports[i].startUniverse; o["leds"]=cfg.ports[i].ledCount; }
  String out; serializeJson(doc,out); server.send(200,"application/json", out);
}

// templates
void handleTplList(){
  DynamicJsonDocument doc(4096); JsonArray arr=doc.to<JsonArray>();
  for(uint8_t i=0;i<MAX_TEMPLATES;i++){
    String name,json,btn; if(loadTemplateSlot(i,name,json,btn)){ JsonObject o=arr.createNestedObject(); o["slot"]=i; o["name"]=name; o["btn"]=btn; }
    else arr.add(nullptr);
  }
  String out; serializeJson(arr,out); server.send(200,"application/json", out);
}
void handleTplSave(){
  DynamicJsonDocument d(512); if(deserializeJson(d, server.arg("plain"))){ server.send(400,"text/plain","bad json"); return; }
  String name=d["name"]|"Effect"; String effect=d["effect"]|"solid"; String color=d["color"]|"#000000";
  int speed=d["speed"]|80; int mirror=d["mirror"]|0; String output=d["output"]|"all"; String btn=d["button"]|"#1f6feb";
  DynamicJsonDocument p(256); p["effect"]=effect; p["color"]=color; p["speed"]=speed; p["mirror"]=mirror; p["output"]=output;
  String payload; serializeJson(p,payload);
  for(uint8_t i=0;i<MAX_TEMPLATES;i++){ String n,j,b; if(!loadTemplateSlot(i,n,j,b)){ saveTemplateSlot(i,name,payload,btn); server.send(200,"text/plain","ok"); return; } }
  saveTemplateSlot(0,name,payload,btn); server.send(200,"text/plain","ok");
}
static bool parseTrailingSlot(const String& uri, uint8_t& slot){ int idx=uri.lastIndexOf('/'); if(idx<0||idx+1>=(int)uri.length()) return false; int v=uri.substring(idx+1).toInt(); if(v<0||v>255) return false; slot=(uint8_t)v; return true; }
void applyTemplatePayload(const String& json){
  DynamicJsonDocument p(256); if(deserializeJson(p,json)) return;
  String eff=p["effect"]|"solid"; String col=p["color"]|"#000000"; int spd=p["speed"]|80; bool mir=(int)p["mirror"]==1; String outSel=p["output"]|"all";
  int pStart=0,pEnd=MAX_PORTS-1; if(outSel!="all"){ int one=outSel.toInt(); if(one>=1&&one<=MAX_PORTS){ pStart=one-1; pEnd=one-1; } }
  if(eff=="wsfx"){
    uint32_t colorV=parseHexColor(col);
    for(int i=pStart;i<=pEnd;i++){ pat[i].pat=PAT_WSFX; if(fx[i]){ fx[i]->setMode(0); fx[i]->setColor(colorV); fx[i]->start(); } }
    return;
  }
  PatName pn=PAT_SOLID;
  if(eff=="rainbow")pn=PAT_RAINBOW; else if(eff=="colorwipe")pn=PAT_COLORWIPE; else if(eff=="blink")pn=PAT_BLINK; else if(eff=="fade")pn=PAT_FADE;
  else if(eff=="theater")pn=PAT_THEATER; else if(eff=="comet")pn=PAT_COMET; else if(eff=="twinkle")pn=PAT_TWINKLE; else if(eff=="wave")pn=PAT_WAVE;
  uint32_t colorV=parseHexColor(col);
  for(int i=pStart;i<=pEnd;i++){ if(fx[i]) fx[i]->stop(); pat[i].pat=pn; pat[i].color=colorV; pat[i].speed=clampu8(spd); pat[i].mirror=mir; pat[i].tLast=0; pat[i].step=0; pat[i].on=true; }
}
void handleTplLoadPath(){
  uint8_t slot; if(!parseTrailingSlot(server.uri(),slot)){ server.send(400,"text/plain","bad slot"); return; }
  String name,json,btn; if(!loadTemplateSlot(slot,name,json,btn)){ server.send(404,"text/plain","no slot"); return; }
  applyTemplatePayload(json); server.send(200,"text/plain","ok");
}
void handleTplDeletePath(){
  uint8_t slot; if(!parseTrailingSlot(server.uri(),slot)){ server.send(400,"text/plain","bad slot"); return; }
  if(deleteTemplateSlot(slot)) server.send(200,"text/plain","ok"); else server.send(404,"text/plain","no slot");
}
void handleTplLoadQuery(){
  uint8_t slot=(uint8_t)server.arg("slot").toInt(); String name,json,btn; if(!loadTemplateSlot(slot,name,json,btn)){ server.send(404,"text/plain","no slot"); return; }
  applyTemplatePayload(json); server.send(200,"text/plain","ok");
}
void handleTplDeleteQuery(){ uint8_t slot=(uint8_t)server.arg("slot").toInt(); if(deleteTemplateSlot(slot)) server.send(200,"text/plain","ok"); else server.send(404,"text/plain","no slot"); }
void handleTplUpdate(){
  DynamicJsonDocument d(512); if(deserializeJson(d, server.arg("plain"))){ server.send(400,"text/plain","bad json"); return; }
  uint8_t slot = (uint8_t)(int)(d["slot"] | 255); if(slot>=MAX_TEMPLATES){ server.send(400,"text/plain","bad slot"); return; }
  String name,json,btn; if(!loadTemplateSlot(slot,name,json,btn)){ server.send(404,"text/plain","no slot"); return; }
  String newName = d["name"].isNull() ? name : d["name"].as<String>();
  String newBtn  = d["btn"].isNull()  ? btn  : d["btn"].as<String>();
  String newOut  = d["output"].isNull()? ""  : d["output"].as<String>();
  if(newOut.length()){
    DynamicJsonDocument p(256);
    if(!deserializeJson(p, json)){ p["output"]=newOut; String payload; serializeJson(p,payload); json=payload; }
  }
  saveTemplateSlot(slot, newName, json, newBtn);
  server.send(200,"text/plain","ok");
}

// web
void startWeb(){
  server.on("/", HTTP_GET, sendIndex);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/saveNet", HTTP_POST, handleSaveNet);
  server.on("/saveOutputs", HTTP_POST, handleSaveOutputs);
  server.on("/pattern", HTTP_POST, handlePattern);
  server.on("/brightness", HTTP_POST, handleBrightness);
  server.on("/state", HTTP_GET, handleState);
  server.on("/templates", HTTP_GET, handleTplList);
  server.on("/templates/save", HTTP_POST, handleTplSave);
  server.on("/templates/load", HTTP_POST, handleTplLoadQuery);
  server.on("/templates/delete", HTTP_POST, handleTplDeleteQuery);
  server.on("/templates/update", HTTP_POST, handleTplUpdate);
  server.on("/wsfx", HTTP_GET, handleWsfx);
  server.onNotFound([](){
    String u=server.uri();
    if(u.startsWith("/templates/load/")){ handleTplLoadPath(); return; }
    if(u.startsWith("/templates/delete/")){ handleTplDeletePath(); return; }
    server.send(404,"text/plain","not found");
  });
  server.on("/factory", HTTP_POST, [](){ prefs.begin(CFG_NS,false); prefs.clear(); prefs.end(); tplstore.begin(TPL_NS,false); tplstore.clear(); tplstore.end(); server.send(200,"text/plain","factory"); delay(300); ESP.restart(); });
  server.on("/restart", HTTP_POST, [](){ server.send(200,"text/plain","reboot"); delay(100); ESP.restart(); });
  server.begin();
}

// ===== Setup/Loop =====
void setup(){
  Serial.begin(115200); delay(100);
  Serial.println("\n[Boot] PIXIDRIVER Pro — WSFX Buttons + Universe0 + Mobile UX");
  loadConfig(); startAPAlways(); connectSTA(); startWeb(); initStrips(); dmxRecomputeExpectedMasks();
  if(!UdpArtnet.begin(cfg.artnetPort)) Serial.println("[UDP] Art-Net bind failed"); else Serial.printf("[UDP] Art-Net listening on %u\n", cfg.artnetPort);
  if(!UdpSACN.begin(SACN_PORT)) Serial.println("[UDP] sACN bind failed"); else Serial.printf("[UDP] sACN listening on %u\n", SACN_PORT);
}
void loop(){
  // ---- CPU load calc ----
  { static uint32_t lastMs=millis(); uint32_t nowMs=millis(); float dt=(nowMs-lastMs);
    lastMs=nowMs; if(dt<1) dt=1;
    avgLoopMs = (0.95f*avgLoopMs) + (0.05f*dt);
    loadPct   = (avgLoopMs/10.0f)*100.0f; if(loadPct>100) loadPct=100;
  }
  // -----------------------

  server.handleClient(); ArduinoOTA.handle();
  for(int i=0;i<MAX_PORTS;i++) if(fx[i]) { if(!dmxActive) fx[i]->service(); }
  for(int p=0;p<MAX_PORTS;p++) applyPatternOne(p);

  int ps=UdpArtnet.parsePacket();
  if(ps>0){ int len=UdpArtnet.read(udpArtnetBuf,sizeof(udpArtnetBuf)); if(len>0){ handleArtnetPacket(udpArtnetBuf,(size_t)len,UdpArtnet.remoteIP()); lastDMXSrc=UdpArtnet.remoteIP(); } }
  int qs=UdpSACN.parsePacket();
  if(qs>0){ int len=UdpSACN.read(udpSACNBuf,sizeof(udpSACNBuf)); if(len>0){ uint16_t u; const uint8_t* d; uint16_t dl; if(parseE131(udpSACNBuf,(size_t)len,u,d,dl)){ for(int p=0;p<MAX_PORTS;p++) applyDMXToPort(p,d,dl,u); for(int p=0;p<MAX_PORTS;p++) if(cfg.ports[p].enabled&&strips[p]) strips[p]->show(); dmxActive=true; lastDmxMs=millis(); lastDMXSrc=UdpSACN.remoteIP(); } } }
  dmxTryShow();
  if(dmxActive && (millis()-lastDmxMs>DMX_HOLD_MS)) dmxActive=false;
  // Toggle WS2812FX custom show depending on DMX activity
  static bool __wasDMX = false;
  if(!__wasDMX && dmxActive && !g_wsfxMuted){
    for(int i=0;i<MAX_PORTS;i++) if(fx[i]) fx[i]->setCustomShow(wsfx_noop_show);
    g_wsfxMuted = true;
  }
  if(__wasDMX && !dmxActive && g_wsfxMuted){
    for(int i=0;i<MAX_PORTS;i++) if(fx[i]) fx[i]->setCustomShow(NULL);
    g_wsfxMuted = false;
  }
  __wasDMX = dmxActive;

}
