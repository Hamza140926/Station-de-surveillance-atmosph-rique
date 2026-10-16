# Atmospheric monitoring station - Uno master / slave

An atmospheric monitoring station using two Arduino Uno boards: a master
reads the DS1621 and BMP280 sensors, while a slave displays measurements,
controls visual and audible alerts, and records data to an SD card.

## Proteus ISIS project and circuit

The Proteus project is included as [Project.pdsprj](./Project.pdsprj).
Open it in a compatible version of Proteus to inspect the circuit and
configure the simulation.

![Atmospheric monitoring station circuit in Proteus ISIS](./image.png)

### Simulation setup

1. Open [Project.pdsprj](./Project.pdsprj) in Proteus.
2. You may need to download and install an **Arduino library for Proteus**
   if the Arduino Uno models are not available in your installation.
   Any other missing component models must also be installed. These Proteus
   simulation libraries are separate from the Arduino IDE sketch libraries
   listed below.
3. Compile [MASTER/MASTER.ino](./MASTER/MASTER.ino) and
   [SLAVE/SLAVE.ino](./SLAVE/SLAVE.ino) for Arduino Uno in the Arduino IDE,
   export the compiled binaries, and set each simulated board's program
   file to the corresponding compiled firmware. Update any existing
   machine-specific firmware paths in the project.
4. For **SD card functionality in the simulation**, use a disk-image tool
   such as **WinImage** to create or prepare a writable **FAT16/FAT32**
   SD card image in a format supported by the Proteus SD model. Select that
   image in the simulated card's properties; the project file alone does
   not provide a prepared SD card image. Do not use exFAT.
5. Run the simulation. Stop it before opening the card image in WinImage
   to inspect or extract the generated `LOGnnnnn.CSV` files; do not modify
   the same image from both programs at once.

WinImage (or an equivalent image utility) is for the simulated SD card.
On real hardware, use a physical FAT16/FAT32 card instead. Simulation
does not replace the hardware acceptance checks at the end of this README.

## What the four tests establish

- TEST1: LCD address `0x27`, 16 columns / 2 rows, red LED D6, green LED D5,
  button D4 to GND with `INPUT_PULLUP`. Its button toggles LEDs, not reset.
- TEST2 and TEST4: UART commands between boards on D0/D1.
  They use SoftwareSerial on the hardware UART pins. The final sketches use
  hardware `Serial` instead, without USB debug output contaminating the link.
- TEST3: DS1621 at `0x48`, BMP280 at `0x76` or `0x77`, sensor acquisition,
  9600-baud measurement frames and a 3 ms Wire timeout. That test uses D10/D11;
  the final sketches use D0/D1 as requested. It only detects the BMP280 at boot
  and does not check each later transaction or retry after disconnection.

The original tests are unchanged.

## Upload and wiring

Upload `MASTER/MASTER.ino` to one **Arduino Uno**, and `SLAVE/SLAVE.ino` to
the other. Use an up-to-date **Arduino AVR Boards** package with Wire timeout
support. The slave needs the same **LiquidCrystal_I2C** library as TEST1,
plus the Arduino **Servo** library and **SdFat by Bill Greiman (version 2.x)**.
Install SdFat from the Arduino IDE Library Manager. The sketch uses SdFat32
to check actual write/sync results; it does not use the older SD.h wrapper.
The master uses Wire directly for checked BMP280 transactions and Bosch
compensation, so it does not need Adafruit_BMP280.

| Connection | Wiring |
| --- | --- |
| UART | Master D1 (TX) -> slave D0 (RX) |
| UART return/reset acknowledgement | Slave D1 (TX) -> master D0 (RX) |
| Common reference | Master GND -> slave GND |
| Master sensor I2C | DS1621 and BMP280 SDA -> A4, SCL -> A5 |
| Slave LCD I2C | LCD SDA -> A4, SCL -> A5; address `0x27` |
| Slave red LED | D6 -> 330 ohm series resistor -> LED -> GND |
| Slave green LED | D5 -> 330 ohm series resistor -> LED -> GND |
| Master reset button | Master D2 -> button -> GND (`INPUT_PULLUP`) |
| Slave passive buzzer | Slave D2 -> low-current passive piezo +, piezo - -> GND |
| Slave flag servo signal | Slave D3 -> servo signal wire |
| Slave blue SD LED | D8 -> 330 ohm series resistor -> blue LED -> GND |
| Slave SD module CS | D10 |
| Slave SD module MOSI | D11 |
| Slave SD module MISO | D12 |
| Slave SD module SCK | D13 |

There is no longer a button on slave D4. D2 is used separately on each
board: master D2 is the button, slave D2 is the buzzer. Do not join them.
The servo is now on slave D3, not D8: D8 is only for the blue LED.

Use a suitable regulated supply for the servo, with its ground connected
to slave GND. Do not power the servo from a GPIO pin; avoid loading the Uno
5 V rail with servo starting/stall current. Do not join unrelated supply
positive outputs. Keep the servo's 0/180-degree travel clear and adjust the
position constants if the mechanism hits a stop.

Connect the SD module's GND to slave GND. Power it according to the exact
module's markings/specification: a bare SD card is a 3.3 V device. Verify
that the module regulates its supply and level-shifts the Uno's 5 V SPI
signals; do not assume all modules marked "MH" do so.

The buzzer code uses `tone()` for a **passive** piezo. A buzzer needing more
current than an Uno GPIO supports needs a suitable driver, not direct GPIO
power. On Uno, Servo uses Timer1 and tone uses Timer2; the LEDs on D5/D6
are controlled with digitalWrite, not PWM.

Keep the master sensor I2C bus separate from the slave LCD I2C bus.
Each bare LED needs its own series resistor. Do not connect an LED directly
between an Uno output and GND: excessive current can damage the LED or
output pin and cause unreliable brightness. Alert and reset animation use
the same full-HIGH ON level; there is no PWM brightness reduction.
Preserve the working sensor power, address-pin wiring and pull-ups from the
tests. A bare BMP280 is a 3.3 V device: do not apply 5 V power or I2C levels
unless the breakout explicitly supports them with regulation/level shifting.

**Disconnect the D0/D1 inter-board wires while uploading, then reconnect.**
Do not use Serial Monitor to send commands or display debug text while the
boards communicate. Uno USB-serial interfaces also share D0/D1; avoid an
active USB serial host driving those pins during operation. Power the boards
appropriately without tying unrelated power supplies together.

## LCD and LEDs

A 16x2 LCD cannot fit
all measurements and messages on one screen.
Instead, it cycles every 1.8 seconds through three pages:

| First row (always) | Second row |
| --- | --- |
| `DS:25.0 C` | `BMPR:1013.2 hPa` |
| `DS:25.0 C` | `NORMALE TEMP` or `ALERT TEMP` |
| `DS:25.0 C` | `PRESSION NORMALE` or `ALERT PRESS` |

Displayed values have one decimal place; transmitted values retain six
decimal places so display rounding does not decide alarms.
Only the DS1621 temperature **>= 30.0 C**
triggers `ALERT TEMP`. Pressure **< 950.0 or > 1050.0 hPa** triggers
`ALERT PRESS`; the endpoints themselves are normal. These pressure limits
are a demonstration range, not a safety threshold or weather forecast.
Thresholds are constants near the top of the slave sketch.

Green means DS temperature is available and below 30 C, any available
pressure is within the normal range, and the communication link is healthy.
Red means a temperature/pressure alert, missing DS temperature, bad frame
or lost link. BMP temperature is not displayed and never controls an alarm.
The master still reads it internally because BMP pressure calculation needs
temperature correction; its existing field remains in the checked data frame.
A missing BMP280 is a displayed warning, not by itself a red LED alarm:
with the BMP280 removed, DS temperature alone controls the temperature alert
and LEDs (green below 30 C, red at or above 30 C). Its absent pressure does
not trigger a pressure alarm. `NORMALE TEMP` / `ALERT TEMP` remain on the
temperature status page, and `CHECK BMP280` appears on the other status page.
If the DS1621 fails, BMP temperature is not used as a replacement:
the LCD shows `DS:NaN C` / `CHECK DS1621`, with a red flag/LED.
Missing data and communication faults are visual warnings, not buzzer alarms.

## Buzzer and flag servo

- Boot: rising three-note tune; flag centered at 90 degrees for at least
  900 ms, and stays centered until data arrives (up to the 3.5-second link
  timeout). Both LEDs are off while waiting at boot.
- Normal: green LED, green flag at 0 degrees, no sound.
- Temperature/pressure alert: red LED, red flag at 180 degrees, continuous
  1600 Hz tone while fresh measurements remain outside the limits.
  Missing DS data, bad frames and lost communication give red LED/flag but
  no continuous sound. In particular, serial errors caused by an SD retry
  must not produce a periodic beep. A missing BMP alone is still only a
  warning when DS temperature is normal.
- Reset: flag returns to 90 degrees and stays there while the LEDs alternate;
  a falling three-note tune replaces any alarm tone, then stops.

Sound patterns use elapsed time rather than delay(), so the LCD, serial
receiver and servo continue working while notes play.

## SD recording and blue LED

The Uno has only 2 KB of RAM, shared by global variables, library buffers,
temporary variables and function calls. The slave keeps fixed LCD messages,
log headings and format strings in program memory using F()/PSTR(), and
uses bounded small text buffers. SD still needs a sector buffer (512 bytes).
These memory changes have not been compiled or tested; check the Arduino
IDE's reported global RAM usage and remaining bytes after compilation.
That report does not include the maximum temporary memory used at runtime,
so free reported RAM is not itself proof of stability.

- At every slave boot (including after reset), check the card and writable
  FAT16/FAT32 filesystem. Without a usable card, show `SD ABSENTE / HS` once
  for two seconds, then return to the normal LCD pages. A card/file writing
  problem instead shows `ERREUR ECRIT SD`. Red/green LEDs, flag and sound
  still depend on sensor/link alarms, not on missing storage.
- Blue blinks approximately every 300 ms while recording is unavailable.
  Retry initialization every five seconds, so insertion is detected without
  another reset. Blue stays on only after initialization and saving a
  session header succeed.
- While a card is ready, check its identity once a second and append the
  latest checked temperature/pressure sample when received (normally 1 Hz).
  Check the write, sync and close results; on failure, stop recording,
  blink blue and show one short warning. Resume automatically after a
  usable card is detected again.
- Each successful card mount creates a new **LOG00001.CSV**, **LOG00002.CSV**,
  etc., choosing the first unused number up to 99999. No existing file is
  overwritten, including old MESURES.TXT recordings. Entries append within
  that session; a reset or remount starts a new file with one header. Example:

  ```text
  sep=;
  Temps_s;Temperature_DS_C;Pression_BMP_hPa;Alerte_temperature;Alerte_pression
  12,345;25,500000;1013,250000;NORMAL;NORMAL
  13,345;31,000000;947,929260;ALERTE;ALERTE
  14,345;29,000000;NaN;NORMAL;INDISPONIBLE
  ```

`Temps_s` is the slave's receive time in seconds since restart, with three
decimal places (wraps after about 49.7 days), not a calendar date. Measurements
retain six decimals, and alert columns use the original unrounded values:
DS >= 30 C; pressure < 950 or > 1050 hPa. Missing values are `NaN` with
`INDISPONIBLE` status, not a normal or measured-alert state.
Invalid serial frames and stale display values are not logged.

The CSV targets French Excel: semicolon-separated columns, decimal commas,
and an Excel `sep=;` line. Extract the CSV from the stopped Proteus image
before opening it in Excel. If importing manually, select delimiter `;`,
French numeric locale and skip the `sep=;` line. For clearer presentation,
format numeric columns to one or two decimals in Excel; the stored precision
is retained. Select time and either measurement column to insert a chart.
These CSV changes have not been compiled or tested, as requested.

Without a card, measurements continue but are **not stored or replayed**
later. The Uno cannot write into the PC project directory without a
separate PC bridge; that feature was explicitly deferred.

There is no card-detect switch connected. An initialization failure cannot
distinguish an absent card from bad wiring/power, and a filesystem failure
can mean an unsupported format. An empty module cannot be verified as
working until a real supported card is available. Use FAT16/FAT32, not
exFAT. No automatic formatting is performed.

SD initialization and writes are synchronous library operations: they may
briefly pause the loop, including the blue blink and animation updates,
especially with a missing/bad card. Boot initializes storage before the
serial receiver and tune start. Runtime retries are spaced out and are
not performed during reset or the boot melody. Long card stalls can lose
serial samples; the Uno does not buffer a recording backlog, and when
multiple samples arrive together only the latest is queued for logging.

Sync/close per entry reduces risk but does not make live card removal or
power loss safe. Prefer powering down before removing a card; unexpected
removal can lose the current entry or damage the filesystem.

## Faulty BMP280

The master checks acknowledgements, byte counts, timeouts, chip ID,
configuration, repeated calibration/data reads, invalid raw values and
physical measurement ranges (-40..85 C and 300..1100 hPa).
On a failed BMP280 read, **both BMP fields become `NaN` immediately in that
sample**. No previous BMP value is presented as current. The master keeps
sampling the DS1621 and transmitting, and retries BMP280 discovery at both
addresses every three seconds. Following recovery, BMP readings resume
after a new conversion.

During a BMP fault the LCD still shows DS temperature on its first row,
shows `BMPR:NaN hPa` on its pressure page, and shows `CHECK BMP280` on
the sensor/pressure status page. A valid DS temperature >= 30 C still shows `ALERT TEMP`
on the temperature status page. DS1621 failures similarly show `NaN` and
`CHECK DS1621` and are retried independently.
A failed DS read gets bounded bus recovery (up to nine SCL pulses and a
STOP, followed by reinitializing the Uno I2C controller) and one immediate
retry if the bus is released. Recovery does not reset or change either
sensor's configuration. If the read remains unavailable, the next sample still attempts a
read instead of immediately restarting conversion. Successful reads clear
the fault. Only failures persisting for three seconds trigger DS
reinitialization. Unavailable readings remain `NaN`, not cached values
presented as live data.

**Software cannot guarantee detection of every plausible but incorrect
measurement caused by a loose connection. Repair the solder joints.**
Also, a BMP280 physically holding SDA/SCL low blocks the DS1621 on the
same bus: timeouts keep the application responsive, but both sensors can
then show `NaN`. Continued DS acquisition under that electrical fault
requires repairing/disconnecting the BMP280 or hardware bus isolation.

The link carries one frame per second:
`D,<DS temperature>,<BMP temperature>,<pressure>*<4-digit CRC16>\r\n`.
CRC is CRC-16/CCITT-FALSE over the text before `*`, initial value `0xFFFF`,
polynomial `0x1021`. `NaN` is an explicit missing reading.
Invalid/oversized frames are discarded without overwriting good data;
`ERREUR TRAME` appears on the status page until a valid frame arrives.
After 3.5 seconds without a valid frame, the LCD shows `DS:NaN C` and
`LIAISON PERDUE` rather than stale measurements.

## Button reset

Use the external button on **master D2**, not the Uno's built-in RESET button.
After 30 ms debounce, the master sends `R`. The slave replies `A`, centers
the flag, plays the falling reset tune, displays `RESET SYSTEME`, draws a
progress bar and alternates LEDs with increasing speed for 1.8 seconds.
It then displays `REDEMARRAGE...` for 800 ms and resets via its watchdog.
The master waits three seconds after receiving `A` before resetting itself,
giving the slave time to finish.

If no reply arrives, the master retries `R` every 300 ms for up to 1.5 seconds
and resets itself after 4.5 seconds total. Repeated `R` requests do not restart
the slave animation. The master cannot guarantee a slave reset if the link
is broken. Standard Uno Optiboot supports watchdog resets.
A button held down across reboot requests another reset; release it after
pressing.

## Hardware acceptance checks

The previous version of both sketches compiled for `arduino:avr:uno` with
Arduino AVR Boards 1.8.8 and LiquidCrystal I2C 1.1.2.
Previous host simulations passed Bosch compensation,
DS1621 initialization timing/negative temperatures, sensor failures and
recovery, LCD pages, alarm boundaries, frame validation, link timeout,
button debounce, animation and the reset handshake/fallback.
The latest SD recording, master-button, buzzer, servo and DS-only temperature changes
have not been compiled or tested,
as requested; compilation is being handled in the Arduino IDE.

1. Boot: rising tune and centered flag, then DS temperature and BMP pressure
   appear; normal readings give green LED/flag and silence.
2. DS temperature reaches exactly 30 C: red LED/flag, `ALERT TEMP`, steady tone.
   BMP temperature changes alone must not trigger a temperature alarm.
3. Pressure outside 950..1050 hPa: red LED/flag, `ALERT PRESS`, steady tone.
4. Unplug/interrupt only the BMP280: DS continues when the bus is free,
   both BMP values show `NaN`, `CHECK BMP280`, no frozen application.
   DS below 30 C gives green and `NORMALE TEMP`; DS at or above 30 C
   gives red and `ALERT TEMP`. The missing BMP alone does not force red.
5. Restore BMP280: automatic rediscovery and fresh readings resume.
6. Disconnect UART: within 3.5 s, `LIAISON PERDUE`, red, no stale data.
7. Press master D2: centered flag, falling tune, LCD reset message,
   accelerating alternating LEDs, both boards restart.
8. Disconnect the slave and press master D2: master still restarts after
   its bounded wait. No slave restart can be promised over a disconnected link.
9. Boot/reset with no SD: one short LCD warning, blinking blue, sensor
   displays and red/green alarms still work.
10. Insert a writable FAT16/FAT32 card: after a retry, blue stays on and
    the new LOGnnnnn.CSV contains elapsed seconds, DS temperatures, BMP
    pressures and separate alert columns.
    Verify actual file contents on a PC after powering down and removing it.

These checks must be performed on the actual boards; host-side tests cannot
reproduce loose solder joints, bus wiring or the physical LCD.
