/*
 * ============================================================
 *  LED Strip Slave Device  —  ESP32 + NeoPixel (RGBW)
 *  Non-blocking effects via state machine (no delay() in effects).
 *  Any new command instantly interrupts the running effect.
 *
 *  IMPORTANT: Change MY_DEVICE_ID for each physical device!
 *  Available IDs: blue | red | yellow | orange
 * ============================================================
 */

#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>

// ─── Hardware config ───────────────────────────────────────────
#define PIXEL_PIN    5
#define PIXEL_COUNT  3700
#define BRIGHTNESS   200

const char* MY_DEVICE_ID = "red";  // change for each device: red | yellow | blue | orange

Adafruit_NeoPixel strip(PIXEL_COUNT, PIXEL_PIN, NEO_GRBW + NEO_KHZ800);

// ─── Color ────────────────────────────────────────────────────
struct RGBWColor { uint8_t r, g, b, w; };

const RGBWColor COLOR_BLACK   = {0,   0,   0,   0};
const RGBWColor COLOR_WHITE   = {0,   0,   0, 255};
const RGBWColor COLOR_WARM    = {0,   0,   0, 255};
const RGBWColor COLOR_RED     = {255,   0,   0, 0};
const RGBWColor COLOR_GREEN   = {0,   255,   0, 0};
const RGBWColor COLOR_BLUE    = {0,     0, 255, 0};
const RGBWColor COLOR_YELLOW  = {255, 200,   0, 0};
const RGBWColor COLOR_CYAN    = {0,   255, 255, 0};
const RGBWColor COLOR_MAGENTA = {255,   0, 255, 0};
const RGBWColor COLOR_ORANGE  = {255,  80,   0, 0};
const RGBWColor COLOR_PURPLE  = {128,   0, 128, 0};

RGBWColor parseColor(const String& t) {
    if (t == "white")   return COLOR_WHITE;
    if (t == "red")     return COLOR_RED;
    if (t == "green")   return COLOR_GREEN;
    if (t == "blue")    return COLOR_BLUE;
    if (t == "yellow")  return COLOR_YELLOW;
    if (t == "cyan")    return COLOR_CYAN;
    if (t == "magenta") return COLOR_MAGENTA;
    if (t == "orange")  return COLOR_ORANGE;
    if (t == "purple")  return COLOR_PURPLE;
    if (t == "warm")    return COLOR_WARM;
    if (t == "off")     return COLOR_BLACK;
    int v[4]={0,0,0,0}, idx=0, pos=0;
    while (idx < 4 && pos < (int)t.length()) {
        int c = t.indexOf(',', pos);
        v[idx++] = ((c==-1) ? t.substring(pos) : t.substring(pos,c)).toInt();
        if (c==-1) break; pos=c+1;
    }
    return {(uint8_t)v[0],(uint8_t)v[1],(uint8_t)v[2],(uint8_t)v[3]};
}

// ─── Strip helpers ────────────────────────────────────────────
uint8_t globalBright = BRIGHTNESS;

inline uint16_t clampPx(int32_t v) {
    if (v < 0) return 0;
    if (v >= PIXEL_COUNT) return PIXEL_COUNT - 1;
    return (uint16_t)v;
}

void fillRange(uint16_t s, uint16_t e, const RGBWColor& c) {
    s=clampPx(s); e=clampPx(e);
    if(s>e){uint16_t t=s;s=e;e=t;}
    for(uint16_t i=s;i<=e;i++) strip.setPixelColor(i,c.r,c.g,c.b,c.w);
}

void setRange(uint16_t s,uint16_t e,const RGBWColor& c){fillRange(s,e,c);strip.show();}
void clearRange(uint16_t s,uint16_t e){fillRange(s,e,COLOR_BLACK);strip.show();}
void fillAll(const RGBWColor& c){fillRange(0,PIXEL_COUNT-1,c);}
void setAll(const RGBWColor& c){fillAll(c);strip.show();}

// ─── Effect state machine ─────────────────────────────────────
enum EffectType {
    EFF_NONE=0,
    EFF_RANDOM,
    EFF_BLINK, EFF_STROBE,
    EFF_BREATH,
    EFF_WIPE,
    EFF_CHASE,
    EFF_SPARKLE,
    // range-scoped:
    EFF_RBLINK, EFF_RSTROBE,
    EFF_RBREATH,
    EFF_RWIPE,
    EFF_RCHASE,
    EFF_RSPARKLE,
};

struct EffectState {
    EffectType    type        = EFF_NONE;
    RGBWColor     color       = COLOR_BLACK;
    uint16_t      rangeStart  = 0;
    uint16_t      rangeEnd    = PIXEL_COUNT - 1;
    uint32_t      intervalMs  = 100;
    uint16_t      param1      = 0;   // tail / steps / count
    int32_t       step        = 0;
    bool          phaseUp     = true;
    bool          phaseOn     = true;
    uint16_t      repeatCount = 0;   // 0 = loop forever
    uint16_t      repeatDone  = 0;
    unsigned long lastTick    = 0;
} eff;

void stopEffect() { eff.type = EFF_NONE; }

// ─── Pending command from ESP-NOW ─────────────────────────────
volatile bool pendingCmd = false;
char          pendingBuf[250];

// ─── Window simulation state ──────────────────────────────────
// Каждое "окно" = один пиксель, независимо управляется
#define MAX_WINDOWS  200

// Состояния окна
enum WinState : uint8_t {
    WIN_OFF = 0,   // выключено, ждём включения
    WIN_FADEIN,    // плавно включается
    WIN_ON,        // горит, ждём выключения
    WIN_FADEOUT,   // плавно гаснет
};

struct Window {
    uint16_t  pixel;        // индекс пикселя на ленте
    WinState  state;        // текущее состояние
    uint32_t  changeAt;     // millis(), когда переходить в следующее состояние
    uint32_t  fadeStepAt;   // millis(), когда делать следующий шаг fade
    uint16_t  fadeSteps;    // всего шагов fade
    uint16_t  fadeCur;      // текущий шаг fade (0..fadeSteps)
    RGBWColor color;        // целевой цвет окна
};

static Window windows[MAX_WINDOWS];
static bool   windowsInited = false;

// 10 минут = полный цикл «суток»
#define DAY_CYCLE_MS  (10UL * 60 * 1000)

// Вероятность того, что окно горит (0..255), зависит от фазы дня
static uint8_t calcOccupancyChance(uint32_t phase) {
    uint8_t pct = (uint8_t)((phase * 100UL) / DAY_CYCLE_MS);
    if      (pct < 20) return 200;   // утро  — высокая активность
    else if (pct < 60) return 140;   // день  — средняя активность
    else if (pct < 90) return 245;   // вечер — почти всё горит
    else               return 120;   // ночь  — умеренная активность
}

// Фиксированный цвет окна (0, 0, 0, 255)
static RGBWColor randomWindowColor() {
    return {0, 0, 0, 255};
}

// Время горения: 5 сек — 8 минут
static uint32_t randomOnDuration() {
    return (uint32_t)random(5000, 480000);
}

// Время до следующего включения: зависит от активности
static uint32_t randomOffDuration(uint8_t occupancy) {
    uint32_t base = (uint32_t)map(occupancy, 0, 255, 20000, 500);
    return base + (uint32_t)random(0, base);
}

// Применить цвет окна с коэффициентом яркости t (0.0..1.0)
static inline void applyWindowFade(const Window& w, float t) {
    strip.setPixelColor(w.pixel,
        (uint8_t)(w.color.r * t),
        (uint8_t)(w.color.g * t),
        (uint8_t)(w.color.b * t),
        (uint8_t)(w.color.w * t));
}

void initWindows() {
    uint32_t now      = millis();
    uint32_t phase    = now % DAY_CYCLE_MS;
    uint8_t  occupancy = calcOccupancyChance(phase);

    for (uint16_t i = 0; i < MAX_WINDOWS; i++) {
        windows[i].pixel  = (uint16_t)((uint32_t)i * PIXEL_COUNT / MAX_WINDOWS);
        windows[i].color  = randomWindowColor();

        if ((uint8_t)random(255) < occupancy) {
            // Сразу на полную яркость — без fade-in при старте
            strip.setPixelColor(windows[i].pixel,
                windows[i].color.r, windows[i].color.g,
                windows[i].color.b, windows[i].color.w);
            windows[i].state      = WIN_ON;
            windows[i].changeAt   = now + randomOnDuration();
            windows[i].fadeSteps  = 0;
            windows[i].fadeCur    = 0;
            windows[i].fadeStepAt = 0;
        } else {
            // Выключено — включится чуть позже с плавным fade-in
            windows[i].state      = WIN_OFF;
            windows[i].changeAt   = now + (uint32_t)random(0, 4000);
            windows[i].fadeStepAt = 0;
            windows[i].fadeSteps  = 0;
            windows[i].fadeCur    = 0;
        }
    }
    strip.show();  // мгновенно показываем начальное состояние
    windowsInited = true;
}

void tickRandom() {
    if (!windowsInited) initWindows();

    uint32_t now       = millis();
    uint32_t phase     = now % DAY_CYCLE_MS;
    uint8_t  occupancy = calcOccupancyChance(phase);

    bool needShow = false;

    for (uint16_t i = 0; i < MAX_WINDOWS; i++) {
        Window& w = windows[i];

        // ── Плавное включение ──
        if (w.state == WIN_FADEIN) {
            if (now >= w.fadeStepAt) {
                w.fadeCur++;
                float t = (float)w.fadeCur / (float)w.fadeSteps;
                applyWindowFade(w, t);
                needShow = true;
                if (w.fadeCur >= w.fadeSteps) {
                    // Достигли полной яркости — переходим в ON
                    strip.setPixelColor(w.pixel, w.color.r, w.color.g, w.color.b, w.color.w);
                    w.state    = WIN_ON;
                    w.changeAt = now + randomOnDuration();
                } else {
                    w.fadeStepAt = now + (uint32_t)random(12, 35); // неравномерный шаг
                }
            }
            continue;
        }

        // ── Плавное выключение ──
        if (w.state == WIN_FADEOUT) {
            if (now >= w.fadeStepAt) {
                if (w.fadeCur > 0) w.fadeCur--;
                float t = (float)w.fadeCur / (float)w.fadeSteps;
                applyWindowFade(w, t);
                needShow = true;
                if (w.fadeCur == 0) {
                    // Полностью погасло
                    strip.setPixelColor(w.pixel, 0, 0, 0, 0);
                    w.state    = WIN_OFF;
                    w.changeAt = now + randomOffDuration(occupancy);
                } else {
                    w.fadeStepAt = now + (uint32_t)random(15, 40);
                }
            }
            continue;
        }

        // ── Плановая смена состояния ──
        if (now < w.changeAt) continue;

        if (w.state == WIN_ON) {
            // Начинаем плавное выключение
            // Длительность fade-out: 500..2500 мс, случайное число шагов
            w.fadeSteps  = (uint16_t)random(20, 80);
            w.fadeCur    = w.fadeSteps;  // начинаем с полной яркости
            w.fadeStepAt = now;
            w.state      = WIN_FADEOUT;
        } else {
            // WIN_OFF — решаем, включать ли окно
            if ((uint8_t)random(255) < occupancy) {
                // Начинаем плавное включение
                w.color      = randomWindowColor();
                w.fadeSteps  = (uint16_t)random(15, 60);
                w.fadeCur    = 0;
                w.fadeStepAt = now;
                w.state      = WIN_FADEIN;
            } else {
                // Не время — попробуем чуть позже
                w.changeAt = now + (uint32_t)random(500, 3000);
            }
        }
    }

    if (needShow) strip.show();
}

// ─── Effect tick functions ────────────────────────────────────

// Shared blink/strobe logic
void tickBlinkBase(uint16_t rs, uint16_t re) {
    if (eff.phaseOn) {
        fillRange(rs,re,eff.color); strip.show();
        eff.phaseOn = false;
    } else {
        fillRange(rs,re,COLOR_BLACK); strip.show();
        eff.phaseOn = true;
        eff.repeatDone++;
        if (eff.repeatCount>0 && eff.repeatDone>=eff.repeatCount) stopEffect();
    }
}

void tickBlink()   { tickBlinkBase(0,PIXEL_COUNT-1); }
void tickStrobe()  { tickBlinkBase(0,PIXEL_COUNT-1); }
void tickRBlink()  { tickBlinkBase(eff.rangeStart,eff.rangeEnd); }
void tickRStrobe() { tickBlinkBase(eff.rangeStart,eff.rangeEnd); }

// Shared breath logic
void tickBreathBase(uint16_t rs, uint16_t re) {
    float t = (float)eff.step / max((int)eff.param1,1);
    RGBWColor c = {
        (uint8_t)(eff.color.r*t),(uint8_t)(eff.color.g*t),
        (uint8_t)(eff.color.b*t),(uint8_t)(eff.color.w*t)
    };
    fillRange(rs,re,c); strip.show();

    if (eff.phaseUp) {
        eff.step++;
        if (eff.step>(int32_t)eff.param1){eff.phaseUp=false;eff.step=eff.param1;}
    } else {
        eff.step--;
        if (eff.step<0){
            eff.phaseUp=true; eff.step=0;
            eff.repeatDone++;
            if(eff.repeatCount>0 && eff.repeatDone>=eff.repeatCount) stopEffect();
        }
    }
}

void tickBreath()  { tickBreathBase(0,PIXEL_COUNT-1); }
void tickRBreath() { tickBreathBase(eff.rangeStart,eff.rangeEnd); }

void tickWipeBase(uint16_t rs, uint16_t re) {
    uint16_t i = rs + (uint16_t)eff.step;
    if(i>re){stopEffect();return;}
    strip.setPixelColor(i,eff.color.r,eff.color.g,eff.color.b,eff.color.w);
    strip.show(); eff.step++;
}

void tickWipe()  { tickWipeBase(0,PIXEL_COUNT-1); }
void tickRWipe() { tickWipeBase(eff.rangeStart,eff.rangeEnd); }

void tickChaseBase(uint16_t rs, uint16_t re) {
    uint16_t len = re-rs+1;
    uint16_t total = len+eff.param1;
    uint16_t i = (uint16_t)eff.step;
    if(i>=total){
        eff.repeatDone++;
        if(eff.repeatCount>0 && eff.repeatDone>=eff.repeatCount){stopEffect();return;}
        eff.step=0; return;
    }
    uint16_t head = rs+i;
    if(head<=re) strip.setPixelColor(head,eff.color.r,eff.color.g,eff.color.b,eff.color.w);
    int32_t prev = (int32_t)rs+i-eff.param1;
    if(prev>=(int32_t)rs && prev<=(int32_t)re) strip.setPixelColor((uint16_t)prev,0,0,0,0);
    strip.show(); eff.step++;
}

void tickChase()  { tickChaseBase(0,PIXEL_COUNT-1); }
void tickRChase() { tickChaseBase(eff.rangeStart,eff.rangeEnd); }

void tickSparkleBase(uint16_t rs, uint16_t re) {
    if(eff.repeatCount>0 && eff.repeatDone>=eff.repeatCount){stopEffect();return;}
    uint16_t len=re-rs+1;
    fillRange(rs,re,COLOR_BLACK);
    for(uint16_t i=0;i<eff.param1;i++)
        strip.setPixelColor(rs+random(len),eff.color.r,eff.color.g,eff.color.b,eff.color.w);
    strip.show(); eff.repeatDone++;
}

void tickSparkle()  { tickSparkleBase(0,PIXEL_COUNT-1); }
void tickRSparkle() { tickSparkleBase(eff.rangeStart,eff.rangeEnd); }

// ─── Effect dispatcher ────────────────────────────────────────
void runEffectTick() {
    if(eff.type==EFF_NONE) return;
    unsigned long now=millis();
    if(now-eff.lastTick < eff.intervalMs) return;
    eff.lastTick=now;

    switch(eff.type){
        case EFF_RANDOM:   tickRandom();   break;
        case EFF_BLINK:    tickBlink();    break;
        case EFF_STROBE:   tickStrobe();   break;
        case EFF_BREATH:   tickBreath();   break;
        case EFF_WIPE:     tickWipe();     break;
        case EFF_CHASE:    tickChase();    break;
        case EFF_SPARKLE:  tickSparkle();  break;
        case EFF_RBLINK:   tickRBlink();   break;
        case EFF_RSTROBE:  tickRStrobe();  break;
        case EFF_RBREATH:  tickRBreath();  break;
        case EFF_RWIPE:    tickRWipe();    break;
        case EFF_RCHASE:   tickRChase();   break;
        case EFF_RSPARKLE: tickRSparkle(); break;
        default: break;
    }
}

// ─── reffect parser ───────────────────────────────────────────
void startReffect(const String& args) {
    int s1=args.indexOf(' '); if(s1==-1)return;
    int s2=args.indexOf(' ',s1+1); if(s2==-1)return;
    int s3=args.indexOf(' ',s2+1);

    uint16_t rs=clampPx(args.substring(0,s1).toInt());
    uint16_t re=clampPx(args.substring(s1+1,s2).toInt());
    String effect=(s3==-1)?args.substring(s2+1):args.substring(s2+1,s3);
    String params=(s3==-1)?String(""):args.substring(s3+1);
    effect.trim(); params.trim();

    Serial.printf("[REFFECT] %d-%d '%s' '%s'\n",rs,re,effect.c_str(),params.c_str());

    stopEffect();
    if(effect=="fill"){fillRange(rs,re,parseColor(params));strip.show();return;}
    if(effect=="off") {clearRange(rs,re);return;}

    eff.rangeStart=rs; eff.rangeEnd=re;
    eff.step=0; eff.phaseUp=true; eff.phaseOn=true; eff.repeatDone=0;
    eff.lastTick=millis()-9999;

    if(effect=="blink"){
        int p1=params.indexOf(' ');if(p1==-1)return;
        int p2=params.indexOf(' ',p1+1);if(p2==-1)return;
        eff.color=parseColor(params.substring(0,p1));
        eff.intervalMs=params.substring(p1+1,p2).toInt();
        eff.repeatCount=params.substring(p2+1).toInt();
        eff.type=EFF_RBLINK; return;
    }
    if(effect=="strobe"){
        int p1=params.indexOf(' ');if(p1==-1)return;
        int p2=params.indexOf(' ',p1+1);if(p2==-1)return;
        int p3=params.indexOf(' ',p2+1);if(p3==-1)return;
        eff.color=parseColor(params.substring(0,p1));
        eff.intervalMs=params.substring(p1+1,p2).toInt();
        eff.repeatCount=params.substring(p3+1).toInt();
        eff.type=EFF_RSTROBE; return;
    }
    if(effect=="breath"){
        int p1=params.indexOf(' ');if(p1==-1)return;
        int p2=params.indexOf(' ',p1+1);if(p2==-1)return;
        eff.color=parseColor(params.substring(0,p1));
        eff.param1=params.substring(p1+1,p2).toInt();
        eff.intervalMs=params.substring(p2+1).toInt();
        eff.repeatCount=0; eff.type=EFF_RBREATH; return;
    }
    if(effect=="wipe"){
        int p1=params.lastIndexOf(' ');if(p1==-1)return;
        eff.color=parseColor(params.substring(0,p1));
        eff.intervalMs=params.substring(p1+1).toInt();
        eff.type=EFF_RWIPE; return;
    }
    if(effect=="chase"){
        int p1=params.indexOf(' ');if(p1==-1)return;
        int p2=params.indexOf(' ',p1+1);if(p2==-1)return;
        eff.color=parseColor(params.substring(0,p1));
        eff.param1=params.substring(p1+1,p2).toInt();
        eff.intervalMs=params.substring(p2+1).toInt();
        eff.repeatCount=0; eff.type=EFF_RCHASE; return;
    }
    if(effect=="sparkle"){
        int p1=params.indexOf(' ');if(p1==-1)return;
        int p2=params.indexOf(' ',p1+1);if(p2==-1)return;
        int p3=params.indexOf(' ',p2+1);if(p3==-1)return;
        eff.color=parseColor(params.substring(0,p1));
        eff.param1=params.substring(p1+1,p2).toInt();
        eff.intervalMs=params.substring(p2+1,p3).toInt();
        eff.repeatCount=params.substring(p3+1).toInt();
        eff.type=EFF_RSPARKLE; return;
    }
    Serial.printf("[REFFECT] Unknown: '%s'\n",effect.c_str());
}

// ─── Main command handler ─────────────────────────────────────
void handleCommand(const String& rawCmd) {
    String cmd=rawCmd; cmd.trim();
    if(cmd.length()==0)return;

    if(cmd=="random"){
        stopEffect();
        windowsInited = false;  // сбрасываем, чтобы переинициализировать окна
        eff.type=EFF_RANDOM; eff.intervalMs=10;
        eff.lastTick=millis()-11;
        return;
    }

    stopEffect();  // new command = instant stop of current effect

    if(cmd=="off"){setAll(COLOR_BLACK);return;}
    if(cmd=="on"||cmd.startsWith("on ")){
        String a=(cmd.length()>3)?cmd.substring(3):"white";a.trim();
        setAll(parseColor(a));return;
    }
    if(cmd.startsWith("brightness ")){
        globalBright=(uint8_t)constrain(cmd.substring(11).toInt(),0,255);
        strip.setBrightness(globalBright);strip.show();return;
    }
    if(cmd.startsWith("pixel ")){
        String a=cmd.substring(6);a.trim();
        int sp=a.indexOf(' ');if(sp==-1)return;
        RGBWColor c=parseColor(a.substring(sp+1));
        strip.setPixelColor(clampPx(a.substring(0,sp).toInt()),c.r,c.g,c.b,c.w);
        strip.show();return;
    }
    if(cmd.startsWith("fill ")){fillAll(parseColor(cmd.substring(5)));return;}
    if(cmd=="show"){strip.show();return;}
    if(cmd.startsWith("reffect ")){startReffect(cmd.substring(8));return;}

    if(cmd.startsWith("range ")){
        String arg=cmd.substring(6);arg.trim();
        int pos=0;
        while(pos<(int)arg.length()){
            int comma=arg.indexOf(',',pos);
            String token=(comma==-1)?arg.substring(pos):arg.substring(pos,comma);
            pos=(comma==-1)?arg.length():comma+1;
            token.trim();if(token.length()==0)continue;
            bool doClear=false;
            if(token.startsWith("off:")){doClear=true;token=token.substring(4);token.trim();}
            else if(token.startsWith("off ")){doClear=true;token=token.substring(4);token.trim();}
            int sp1=token.indexOf(' ');if(sp1==-1)continue;
            int sp2=token.indexOf(' ',sp1+1);
            uint16_t s=token.substring(0,sp1).toInt();
            uint16_t e;RGBWColor c=COLOR_WHITE;
            if(sp2==-1){e=token.substring(sp1+1).toInt();}
            else{e=token.substring(sp1+1,sp2).toInt();c=parseColor(token.substring(sp2+1));}
            fillRange(s,e,doClear?COLOR_BLACK:c);  // fill without show — batch all ranges first
        }
        strip.show();  // single show for all ranges at once
        return;
    }

    if(cmd.startsWith("sections ")){
        uint8_t n=(uint8_t)cmd.substring(9).toInt();if(n==0)return;
        uint16_t len=PIXEL_COUNT/n;
        for(uint8_t s=0;s<n;s++){
            uint16_t start=s*len;
            uint16_t end=(s==n-1)?PIXEL_COUNT-1:start+len-1;
            fillRange(start,end,(s%2==0)?COLOR_WHITE:COLOR_BLACK);
        }
        strip.show();return;
    }

    // Non-blocking effects — just configure state and return immediately
    if(cmd.startsWith("blink ")){
        String a=cmd.substring(6);a.trim();
        int s1=a.indexOf(' ');if(s1==-1)return;
        int s2=a.indexOf(' ',s1+1);if(s2==-1)return;
        eff.color=parseColor(a.substring(0,s1));
        eff.intervalMs=a.substring(s1+1,s2).toInt();
        eff.repeatCount=a.substring(s2+1).toInt();
        eff.repeatDone=0;eff.phaseOn=true;
        eff.lastTick=millis()-eff.intervalMs;
        eff.type=EFF_BLINK;return;
    }
    if(cmd.startsWith("breath ")){
        String a=cmd.substring(7);a.trim();
        int s1=a.indexOf(' ');if(s1==-1)return;
        int s2=a.indexOf(' ',s1+1);if(s2==-1)return;
        eff.color=parseColor(a.substring(0,s1));
        eff.param1=a.substring(s1+1,s2).toInt();
        eff.intervalMs=a.substring(s2+1).toInt();
        eff.repeatCount=0;eff.step=0;eff.phaseUp=true;
        eff.lastTick=millis()-eff.intervalMs;
        eff.type=EFF_BREATH;return;
    }
    if(cmd.startsWith("wipe ")){
        String a=cmd.substring(5);a.trim();
        int sp=a.lastIndexOf(' ');if(sp==-1)return;
        eff.color=parseColor(a.substring(0,sp));
        eff.intervalMs=a.substring(sp+1).toInt();
        eff.step=0;eff.lastTick=millis()-eff.intervalMs;
        eff.type=EFF_WIPE;return;
    }
    if(cmd.startsWith("chase ")){
        String a=cmd.substring(6);a.trim();
        int s1=a.indexOf(' ');if(s1==-1)return;
        int s2=a.indexOf(' ',s1+1);if(s2==-1)return;
        eff.color=parseColor(a.substring(0,s1));
        eff.param1=a.substring(s1+1,s2).toInt();
        eff.intervalMs=a.substring(s2+1).toInt();
        eff.repeatCount=0;eff.step=0;
        eff.lastTick=millis()-eff.intervalMs;
        eff.type=EFF_CHASE;return;
    }
    if(cmd.startsWith("sparkle ")){
        String a=cmd.substring(8);a.trim();
        int s1=a.indexOf(' ');if(s1==-1)return;
        int s2=a.indexOf(' ',s1+1);if(s2==-1)return;
        int s3=a.indexOf(' ',s2+1);if(s3==-1)return;
        eff.color=parseColor(a.substring(0,s1));
        eff.param1=a.substring(s1+1,s2).toInt();
        eff.intervalMs=a.substring(s2+1,s3).toInt();
        eff.repeatCount=a.substring(s3+1).toInt();
        eff.repeatDone=0;eff.step=0;
        eff.lastTick=millis()-eff.intervalMs;
        eff.type=EFF_SPARKLE;return;
    }
    if(cmd.startsWith("strobe ")){
        String a=cmd.substring(7);a.trim();
        int s1=a.indexOf(' ');if(s1==-1)return;
        int s2=a.indexOf(' ',s1+1);if(s2==-1)return;
        int s3=a.indexOf(' ',s2+1);if(s3==-1)return;
        eff.color=parseColor(a.substring(0,s1));
        eff.intervalMs=a.substring(s1+1,s2).toInt();
        eff.repeatCount=a.substring(s3+1).toInt();
        eff.repeatDone=0;eff.phaseOn=true;
        eff.lastTick=millis()-eff.intervalMs;
        eff.type=EFF_STROBE;return;
    }

    if(cmd=="status"){
        uint8_t mac[6];WiFi.macAddress(mac);
        Serial.println("=== STATUS ===");
        Serial.printf("Device ID  : %s\n",MY_DEVICE_ID);
        Serial.printf("Pixel count: %d\n",PIXEL_COUNT);
        Serial.printf("Brightness : %d\n",globalBright);
        Serial.printf("Effect     : %d\n",(int)eff.type);
        Serial.printf("MAC        : %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        Serial.println("==============");
        return;
    }

    Serial.printf("[ERROR] Unknown command: %s\n",cmd.c_str());
}

// ─── ESP-NOW receive callback ─────────────────────────────────
// Runs in a FreeRTOS task — only copy data, don't process here.
void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if(pendingCmd) return;
    int copyLen=min(len,249);
    memcpy(pendingBuf,data,copyLen);
    pendingBuf[copyLen]='\0';
    pendingCmd=true;
}

// ─── Setup ────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== SLAVE DEVICE INIT ===");
    Serial.printf("Device ID: %s\n",MY_DEVICE_ID);

    WiFi.mode(WIFI_STA);
    uint8_t mac[6];WiFi.macAddress(mac);
    Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    WiFi.disconnect();

    if(esp_now_init()!=ESP_OK) Serial.println("[ERROR] ESP-NOW init failed!");
    else                       Serial.println("[OK] ESP-NOW ready");
    esp_now_register_recv_cb(onDataRecv);

    strip.begin();
    strip.setBrightness(BRIGHTNESS);
    strip.clear();
    strip.show();

    randomSeed(esp_random());

    // Start with window simulation
    windowsInited = false;
    eff.type=EFF_RANDOM; eff.intervalMs=10;
    eff.lastTick=millis()-11;

    Serial.println("=== READY ===\n");
}

// ─── Loop ─────────────────────────────────────────────────────
void loop() {
    // 1. Process pending ESP-NOW command
    if(pendingCmd){
        String msg=String(pendingBuf);
        msg.trim();
        pendingCmd=false;  // clear BEFORE processing so next cmd can arrive
        Serial.printf("[ESP-NOW] %s\n",msg.c_str());

        int colon=msg.indexOf(':');
        if(colon==-1){Serial.println("[WARNING] No ':' — ignoring");}
        else{
            String target=msg.substring(0,colon);target.trim();
            String command=msg.substring(colon+1);command.trim();
            if(target.equalsIgnoreCase(MY_DEVICE_ID)||target.equalsIgnoreCase("all")){
                Serial.printf("[EXECUTE] %s\n",command.c_str());
                handleCommand(command);
            }
        }
    }

    // 2. Process USB Serial command
    if(Serial.available()){
        String cmd=Serial.readStringUntil('\n');
        cmd.trim();
        if(cmd.length()){Serial.printf("[SERIAL] %s\n",cmd.c_str());handleCommand(cmd);}
    }

    // 3. Tick the active effect (non-blocking, uses millis())
    runEffectTick();
}
