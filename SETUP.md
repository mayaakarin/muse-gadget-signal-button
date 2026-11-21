# Memo (how to use)

Japanese version: [SETUP.ja.md](SETUP.ja.md).

Add physical buttons to the gadget.
This is especially useful for the Disp machine, which can only display.
The gadget side only sends which button was pressed. The receiving Muse can
tell which button it was, so if you prepare actions (skills) for each button,
it works as a remote control.

## Enabling it

Set these in the board's sdkconfig:

```
CONFIG_HOMEHUB_SIGNAL_BUTTON
CONFIG_HOMEHUB_SIGNAL_GPIO
```

Example:

```
CONFIG_HOMEHUB_SIGNAL_BUTTON=y
CONFIG_HOMEHUB_SIGNAL_GPIO=7
```

Up to 30 buttons can be configured, but the button config is loaded when the
gadget boots, so one entry in sdkconfig is enough.

## About the setup (teaching Muse)

#important: teach Muse the rules beforehand.

The rules (in a sense, Muse's setup) are kept separately from this memo, in
[muse_setup.md](muse_setup.md). Hand it to your own Muse as-is, and have
it rewrite the assignments and actions to your own. The Disp-machine action
example (a 4-band button image, an execution screen on press, back after
5 seconds) is in there too.

The assignment you write can be in the short form:

```
BTN1=18
BTN2=23
BTN3=24
```

Turning it into the CSV form for the CFGREQ reply is Muse's job.
