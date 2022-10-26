# Digitální záznamové zařízení

Cílem projektu bylo sestavit záznamník za použití SoC ESP32.
Zařízení dále používá mikrofon SPH0645 a zesilovač MAX98357A, které komunikují po sběrnici I2S.
Dále je připojena SD katra, na které jsou uchovány záznamy ve formátu WAV.
Uživatel se zařízením interaguje pomocí tří tlačítek a OLED displaye SSD1306.



## Zapojení

Vzhledem k tomu, že ESP32 umožňuje připojení téměř jakékoli periferie k jakémukoli pinu, je toto zapojení poměrně arbitrární. 
V tomto projektu byla použit vývojový kit Wemos D1 R32.
Všechny periferie byly napájeny z 3.3V vycházející z této desky, deska samotná byla napájena přes USB.

### SPH0645

Mikrofon komunikuje po sběrnici I2S. Jeho piny jsou zapojeny následovně:

| SPH0645 | ESP32 |
|---------|-------|
| BCLK    | 17    |
| DOUT    | 16    |
| LRCL    | 25    |
| SEL     | 3.3V  |

### MAX98357A

Zesilováč komunikuje po sběrnici I2S. 
Teoreticky by zřejmě mohla být použita stejná sběrnice, na které komunikuje mikrofon.
Pro tento případ je ale použita druhá, protože ESP32 může obsluhovat až dvě najednou.
Také lze individuálně zapínat a vypínat sběrnice pro tyto periferie.
Jeho piny jsou zapojeny následovně:

| MAX98357A | ESP32 |
|-----------|-------|
| BCLK      | 2     |
| DIN       | 4     |
| LRC       | 27    |

### SD karta

SD karta komunikuje přes rozhraní SPI. 
K mikrokontroléru je připojena adaptérem bez LDO regulátoru a bez level shifteru.

| SD adaptér | ESP32 |
|------------|-------|
| MISO       | 19    |
| MOSI       | 23    |
| CLK        | 18    |
| CS         | 5     |

### SSD1306

OLED dislay komunikuje přes rozhraní I2C.
Nebylo potřeba použití pullup rezistorů, zřejmě byly zapnuté softwarově v ESP32.

| SSD1306 | ESP32 |
|---------|-------|
| SDA     | 21    |
| SCL     | 22    |

### Tlačítka a LED

K mikrokontroléru jsou připojena 3 tlačítka (UP, DOWN, OK) na piny 13, 12 a 14.
Všechna tlačítka jsou typu NO (normally open) a z druhé strany připojena k 3.3V.

LED není pro projekt nutná, její jediný účel je signalizace nahrávání.
Anoda LED je připojena k pinu 26 přes rezistor.

## Způsob vývoje

Prvním krokem byla volba a instalace vývojového prostředí.
Pro tento projekt bylo zvoleno ESP-IDF, které je vyvíjené a podporované výrobcem.
ESP-IDF je open source pod licencí Apache 2.0.

Dále bylo potřeba oživit všechny periferie.
Toto bylo nejjednodušší nahráním příkladu z ESP-IDF.

Poté co byla ověřena funkčnost a kompatibilita všech periferií zvlášť, bylo možné kód integrovat do jednoho projektu.
Nejprve bylo zprovozněno nahrávání a převod na požadovaný formát.
Pak následovalo přehrávání z tohoto formátu.
Dále byla integrována SD karta a implementováno ukládání WAV souborů.

## Implementační detaily

### Úlohy FreeRTOS




## Použité zdroje

- [ESP-IDF Examples](https://github.com/espressif/esp-idf/tree/master/examples)

