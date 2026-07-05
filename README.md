# Spinnerino
Un giro nel passato

## 📘 Descrizione

Mini arcade dedicato ai titoli con controllo a manopola con grandi classici come Arkanoid. Un concentrato di retro-gaming e DIY in un formato unico.
Un progetto di Marco Prunca basato su ESP32-P4. 

<img width="3840" height="2160" alt="IMG_9652_(00-00-08-12)" src="https://github.com/user-attachments/assets/c1a3b713-ff37-46b9-b017-30f5a540b1e8" />
<img width="3840" height="2160" alt="IMG_9650_(00-00-03-03)" src="https://github.com/user-attachments/assets/d3b824f6-8af3-44bd-a042-fffb83bed8f1" />
<img width="3840" height="2160" alt="IMG_9646_(00-00-02-06)" src="https://github.com/user-attachments/assets/9b8e4b81-fbad-4911-bf16-bc04afeb70a8" />
<img width="3840" height="2160" alt="IMG_9642_(00-00-02-23)" src="https://github.com/user-attachments/assets/322a2244-ee6e-4c6f-b591-427e38b8ce93" />
<img width="3840" height="2160" alt="IMG_9642_(00-00-00-00)" src="https://github.com/user-attachments/assets/57e43023-a464-44e6-bbb7-ddca840d6f96" />
<img width="3840" height="2160" alt="IMG_9641_(00-00-02-20)" src="https://github.com/user-attachments/assets/32f2aee2-9f78-437c-a5f8-d7a7a1a731ba" />
<img width="3840" height="2160" alt="IMG_9641_(00-00-02-15)" src="https://github.com/user-attachments/assets/024744f2-f7e9-44a6-8999-6974f8a754a4" />

---
    COMANDI:
 
    Gioca:            -Ruota l'encoder/paddle/spinner. 
                      -Premi manopola (COIN) encoder per inserire credito. 
                      -Premi pulsante FIRE per avviare il gioco.
 
    REGOLA VOLUME:    Tieni premuto FIRE per ~3 secondi
                      -> compare la barra volume verde. Poi RUOTA l'encoder:
                      orario (destra) = alza, antiorario (sinistra) = abbassa.
                      RILASCIA FIRE per confermare e salvare (NVS). Durante la
                      regolazione il paddle resta fermo.
 
    ESCI DAL GIOCO:   tieni premuto (COIN) per ~3 secondi -> torna al menu giochi.
 
    IMPOSTAZIONI:     nel menu giochi, dopo l'ultimo gioco c'e' la voce
                      IMPOSTAZIONI per regolare la sensibilita' dell'encoder
                      (spinner) per ogni gioco.
 
---
 
 * Dove trovare i file da inserire nella cartella ROM per la conversione:
 * [Galaga (Namco Rev. B ROM)](https://www.google.com/search?q=galaga.zip+arcade+rom)
 * [Galaxian](https://www.google.com/search?q=glaxian.zip+arcade+rom)
 * [SpaceInvaders](https://www.google.com/search?q=invaders.zip+arcade+rom)
 * [Arkanoid](https://www.google.com/search?q=arkangc.zip+arcade+rom)
 * [Arkanoid2](https://www.google.com/search?q=arknoid2.zip+arcade+rom)
 * [Gigas Bootleg](https://www.google.com/search?q=gigasb.zip+arcade+rom)
 * [Gigas2 Bootleg](https://www.google.com/search?q=gigasm2b.zip+arcade+rom)
 * [Super Breakout](https://www.google.com/search?q=sbrkout.zip+arcade+rom)
 * [Moto Race USA](https://www.google.com/search?q=motorace.zip+arcade+rom)
 * [Phoenix](https://www.google.com/search?q=phoenix.zip+arcade+rom)
 * [Goindol](https://www.google.com/search?q=goindol.zip+arcade+rom)
 * [Gyruss](https://www.google.com/search?q=gyruss.zip+arcade+rom)
 * [Road Fighters](https://www.google.com/search?q=roadf2.zip+arcade+rom)
 * [Bomb Bee](https://www.google.com/search?q=bombbee.zip+arcade+rom)
 * [Pang / Buster Bros](https://www.google.com/search?q=pang.zip+arcade+rom)   
 
 Una volta recuperati tutti i file ZIP inseriteli nella cartella SPINNERINO\ROMS
 
 * Per convertire le ROM:
 - WINDOWS: Lanciare il file convert_all.bat nella cartella \ROM_CONVERTER (serve avere installato python)
 - MACOS: Lanciare il file .\convert.all.sh nella cartella \ROM_CONVERTER (nel caso rendere prima eseguibile con il comendo chmod +x conv_all.sh)

---
## 🎛️ Schema pratico di assemblaggio

<img width="2236" height="1580" alt="Screenshot 2026-07-05 alle 15 23 18" src="https://github.com/user-attachments/assets/c66df527-61c2-4831-bd88-03d565c39a80" />

---
## 🎛️ Materiali

* Display con encoder 2.4": https://s.click.aliexpress.com/e/_c45R7U3r
* ESP32 P4 PICO: https://s.click.aliexpress.com/e/_c3XGFXBT
* BMS FM5324: https://s.click.aliexpress.com/e/_c3Ug7Bln
* BMS IP5306: https://s.click.aliexpress.com/e/_c35Euvxf
* BMS CD42: https://s.click.aliexpress.com/e/_c4L3E7op

* Altoparlante 4230 4ohm 3W: https://s.click.aliexpress.com/e/_c3zBnIV3
* Batteria: https://amzn.to/4omnK15
* CAVO USBC 2P: https://s.click.aliexpress.com/e/_c40L1aSl
* Interruttore a bilanciere 10x15 ALI: https://s.click.aliexpress.com/e/_c3iHtujx

---
# 🎛️ Programmazione ESP32P4

Arduino IDE settings (esp32 core >= 3.2.0):
*   Board:                 ESP32P4 Dev Module
*   Flash Size:            32MB
*   PSRAM:                 Enabled
*   Partition Scheme:      32M Flash (13Mb App/6.75Mb SPIFFS)
*   USB Mode:              Hardware CDC and JTAG
*   USB CDC On Boot:       Disabled
*   Upload Speed:          921600

---
# 📝 Revisioni

V1.0 - 21/05/2026
*  Prima Versione

---
## 🧾 Licenza

Questo progetto è distribuito con licenza
**Creative Commons – Attribuzione – Non Commerciale 4.0 Internazionale (CC BY-NC 4.0)**.

Puoi condividerlo e modificarlo liberamente, **citando l’autore**
(Davide Gatti / [survivalhacking](https://github.com/survivalhacking)) e **senza scopi commerciali**.

🔗 [https://creativecommons.org/licenses/by-nc/4.0/](https://creativecommons.org/licenses/by-nc/4.0/)

