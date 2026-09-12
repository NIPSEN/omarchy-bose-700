# BMAP on the Bose Noise Cancelling Headphones 700

The NC700 speaks Bose's **BMAP** (Bose Management and Control Protocol) over a
plain RFCOMM link. Everything below was verified on firmware **1.8.2**
(`1.8.2-11524+e0f7590`). The protocol knowledge comes from the reverse
engineering in [aaronsb/bosectl](https://github.com/aaronsb/bosectl).

## Transport

- **RFCOMM channel 8**, fixed — no SDP lookup needed.
- Each message is a framed packet addressed to a two-byte `[block.function]`
  address, with GET, SET and SETGET operations. SETGET sets a value and replies
  with the value now in effect.

## Verified addresses

| Feature | Address | Payload notes |
|---|---|---|
| Firmware version | `[0.5]` | GET; string such as `1.8.2-11524+e0f7590` |
| Device name | `[1.2]` | GET; the user-chosen Bluetooth name |
| Voice prompts | `[1.3]` | GET/SET; on/off plus the prompt language |
| Noise cancelling (CNC) | `[1.5]` | GET/SETGET; wire payload `{raw, 1}` with the axis **inverted** — raw 0 = max ANC, raw 10 = full passthrough; the daemon exposes ANC strength (0 = transparency, 10 = max ANC), matching the Bose Music app |
| Equalizer | `[1.7]` | one 4-byte group per band: `[min, max, current, band]`; bass/mid/treble, −10…+10 |
| Button configuration | `[1.9]` | GET |
| Multipoint | `[1.10]` | GET/SET; on/off |
| Sidetone | `[1.11]` | GET/SET; off / low / medium / high |
| Battery level | `[2.2]` | GET; percent |
| Active source | `[5.1]` | GET |

## Known dead ends

- **Block 31 is not supported** on the NC700 — queries to it go unanswered.
  Don't wait on it.

## Caveats

- Addresses and payload shapes are from a single headset on firmware 1.8.2;
  other firmware revisions may differ.
- Prefer SETGET over SET where available: the reply doubles as confirmation,
  which is what the daemon uses to keep `status.json` truthful.
