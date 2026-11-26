# LibreTap protocol descriptions (version 1)

A protocol description says what is known about an instrument's messages, so LibreTap can say each message in a
sentence (the timeline's *Meaning* column, `LibreTapCLI decode`) and show how much of a recording is explained.
It is one JSON file, written by hand or by an assistant, shared like any text file. Example:
[`app/examples/cora/cora.protocol.json`](../app/examples/cora/cora.protocol.json).

```json
{
  "format": "libretap-protocol",
  "version": 1,
  "name": "DEMO-METER",
  "notes": "Where it comes from, what is verified and what is guessed.",
  "messages": [
    {"name": "set integration time", "dir": "to-instrument",
     "text": "^SET INT (\\d+)\\r$",
     "fields": [{"name": "time", "group": 1, "type": "int", "scale": 0.001, "unit": "s"}],
     "say": "Set the integration time to {time}"},
    {"name": "reading", "dir": "from-instrument", "text": "^M (-?[0-9.]+) V\\r\\n$",
     "fields": [{"name": "v", "group": 1, "type": "float", "unit": "V"}], "say": "Reading {v}"},
    {"name": "exposure", "device": "ba8c:1688", "usb": {"bmRequestType": "0x40", "bRequest": "0xAE"},
     "fields": [{"name": "exposure", "offset": 0, "length": 4, "type": "u32le", "add": 1, "scale": 16e-6, "unit": "s"}],
     "say": "Spectrometer: exposure {exposure}"},
    {"name": "status report", "device": "04d8:00df", "ep": 1, "dir": "from-instrument", "hex": "80 …",
     "fields": [{"name": "gpio", "offset": 10, "type": "u8", "format": "bits"}], "say": "GPIO {gpio}"}
  ]
}
```

## Matching

Messages are tried in order; the first that fits says the sentence.

| key | meaning |
|---|---|
| `dir` | `to-instrument` (PC/host/program wrote; tap line `a`), `from-instrument`, or `any` (default) |
| `device` | USB events: the device's `vid:pid` |
| `source` | the session source kind (`serial-tap`, `api-serial`, `usb-pico`, `usbpcap`, `ethernet`) |
| `ep` | USB endpoint number |
| `text` | a regular expression on the message as Latin-1 text (anchor it with `^…$`) |
| `hex` | bytes from the start, `??` = any byte; end with `…` or `*` to allow more bytes; `"*"` alone = any payload |
| `usb` | a control transfer's setup fields: `bmRequestType`, `bRequest`, `wValue`, `wIndex`, `wLength` (numbers or "0x..") |

## Fields

| key | meaning |
|---|---|
| `name` | used as `{name}` in `say` |
| `group` | with `text`: the regular expression's group number |
| `offset`, `length`, `type` | with bytes: `u8`, `i8`, `u16le`, `u16be`, `i16le`, `i16be`, `u32le`, `u32be`, `i32le`, `i32be`, `ascii`, `hex` |
| `from` | USB control transfers: `wValue`, `wIndex`, `wLength`, `bRequest` |
| `type` (text) | `int`, `float`, `hex`, `text` |
| `scale`, `add`, `unit` | value = raw × scale + add, shown with the unit |
| `values` | names for raw values: `{"9": "laser ON", "H": "high"}` |
| `format` | `hex` or `bits` instead of a number |

## Coverage

A message matched by `text` or `hex` counts as explained entirely; a USB control transfer counts its setup bytes and
its byte fields; everything else is unexplained. `LibreTapCLI decode REC --protocol FILE` prints the percentage and
the kinds of message not explained yet (`--unexplained` lists only those messages).
