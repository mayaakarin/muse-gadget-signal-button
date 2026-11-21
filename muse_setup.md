# Gadget agreement rules

Japanese version: [muse_setup.ja.md](muse_setup.ja.md).

This is a file to hand to your own Muse.
Rewrite the button assignments and actions to your own, then give it to
your Muse.

These are the agreement rules for my gadget. Please remember them.

## Premises

- This applies to devices that have `device.signal`.
- If there are several gadgets, keep an assignment per device. When a
  `CFGREQ` or a `[SIG]` arrives, check which device it came from and use
  that device's assignment.

## Button assignment

```
BTN1=GPIO18
BTN2=GPIO23
BTN3=GPIO24
```

## Replying to CFGREQ

When `[SIG] code=CFGREQ seq=N nonce=XXXXXXXX` arrives in the chat as the
gadget boots, turn this assignment into one CSV line like
`01:18,02:23,03:24` and reply with a single `[CFG]` message that copies the
nonce. Do not add any extra text.

## When a button is pressed

When a message like `[SIG] code=BTN1 press=short seq=N` arrives, perform
the corresponding action. If needed, decide an action for each BTN.

- When BTN1 is pressed, do XXX
- When BTN2 is pressed, send today's weather as an image
- When BTN3 is pressed, return a confirmation image

## Action example (Disp machine)

With a gadget that has a display, this shape works too (a real example).

The button image is a legend: the screen split into four vertical bands,
each showing a button's colour and name.

- BTN1: red, with the text "Command 1"
- BTN2: yellow, with the text "Command 2"
- BTN3: blue, with the text "Command 3"
- BTN4: white, with the text "Weather forecast"

When a button is pressed, show the matching execution screen.

- BTN1–BTN3: the execution screen for "Command N"
- BTN4 (white) only: the weather forecast image

In all cases, return to the original button image after 5 seconds.
