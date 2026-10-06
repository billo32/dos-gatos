# Protocol

The clock and the Dos GatOS app speak newline-delimited JSON (one object per line, `t` is its type)
over USB serial (CH340, 460800 baud) or over the local network.

```
[TC001: scheduler] --req{url,path|find,re}--> USB / Wi‑Fi link --> [Dos GatOS app] --HTTPS--> API
          |        <--resp{status, body | text | frames}--
          +-- no app for 10 s --> Wi‑Fi --HTTPS (certificates not verified)--> API
```

## Messages

**Clock → app**

| message | when |
|---|---|
| `hello{fw,apps,font,wifi,rst,heap,scr,id,cfg,fs,max,line}` | after boot and on `hello?`; `id` is the chip id, `cfg` the stored config revision, `fs{used,total}` LittleFS use, `max` how many switched-on apps are shown (16), `line` the longest line accepted (12288 bytes) |
| `req{id,url,path?,find?,keep?,re?,headers?,ipath?}` | an app is due |
| `pong` | answer to `ping` |
| `btn{b}` | a button was pressed |
| `log{m}` | a line for the app's log |
| `wifi{ssid,state,ip?,rssi?,why?}` | Wi‑Fi changed; `why` says why it can't join (0.8.2) |
| `scr{name}` | which screen is showing |
| `cfg{rev,cfg?}` | answer to `cfg?` |
| `pomo{phase,left,paused,rounds}` | Pomodoro state |

**App → clock**

| message | what it does |
|---|---|
| `hello?`, `ping` (every 3 s) | |
| `time{epoch,tz,tzp}` | time; `tzp` is the POSIX zone rule (`CET-1CEST,M3.5.0,M10.5.0/3`) for daylight saving on its own |
| `resp{id,status,body,ikey?}` | a request's result (version-1 sources) |
| `resp{id,status,text,color?,icon?}` | a finished result from version-2 rules (0.9.1); empty `text` skips the app |
| `resp{id,status,frames:[{text,color,icon}…],fdur}` | several screens in turn (0.9.2) |
| `apps[…]` | the app list |
| `icon{id,n,d,px}` | an icon: `n` frames of 8×8 RGB565, `d` delays in ms, `px` n×256 hex |
| `notify{text,color,dur,icon?,font?}` | show a notification |
| `bright{v}` | brightness |
| `wifi{ssid,pass}` | Wi‑Fi settings (USB only) |
| `settings{font?,clock?{on,dur,pos,h24,wday},pomo?{…},scroll?,restart?}` | clock settings |
| `cfg{rev,cfg}`, `cfg?` | store / fetch the app's config on the clock |
| `pomo{cmd: start|stop|toggle}` | Pomodoro |
| `busy{on,icon?}` | ON CALL while repeated, and 15 s after |

**Config on the clock** (0.6.0): `cfg{rev,cfg}` is the app's whole configuration (sources, clock
face, brightness, time zone) with a revision. The clock doesn't read it: it keeps it in LittleFS
(`/cfg.json`), confirms with the log line `cfg saved: rev N` and returns it on `cfg?`. That's how a
clock's apps follow it to another computer. Flashing the full image from 0x0 keeps it; `erase_flash`
doesn't.

## The Wi‑Fi link (0.9.0)

A clock on Wi‑Fi announces itself over mDNS as `_dosgatos._tcp` (TXT `id`, `fw`) and takes one
connection on TCP port 7766 with the same protocol. On connect it sends `{"t":"hi","id","fw"}`.
USB wins: a host on the cable closes the network one, and while it's active new network
connections get `{"t":"bye","why":…}`. A network host that's quiet for 15 s is dropped.

There's no authentication in this first version, so the clock refuses `wifi` over the network.

## Time

The time is kept in the DS1307 RTC (0x68) in UTC. After a reboot it is used only if the RTC drifted
less than 60 s at the last check against the app; otherwise the clock shows `--:--` until it syncs
(from the app, or NTP on Wi‑Fi). The app re-syncs on connect and then hourly.
