# Replies spoken on the device (experimental)

With `CONFIG_MUSE_LOCAL_TTS`, a gadget speaks Muse's replies itself, with a
small text-to-speech model ([SteerTTS Nano](https://huggingface.co/WhissleAI/steer-tts-nano))
that runs on the ESP32-S3. No server makes the speech. It is off by default,
English only, and a personal tech demo: it was built and tested on the
**Waveshare ESP32-S3-Touch-AMOLED-1.75C** only.

The model and its runtime are **not** in this repository. They are licensed
separately by Whissle (CC BY-NC 4.0 plus Whissle's terms: non-commercial use
only), and you download them yourself. Never commit them here; `.gitignore`
keeps `*.bin` out.

## What you need

- An ESP32-S3 board with the full UI, PSRAM and 16 MB of flash or more.
- ESP-IDF v6.0.3, as for any build (`../AGENTS.md`).
- A Hugging Face account.
- The model takes the flash space of the WebAssembly apps' partitions
  (`apps`, `files`): a build has speech or apps, not both.

## 1. Get the licence and the model

1. Sign in at [huggingface.co](https://huggingface.co) and open the model's
   page, [WhissleAI/steer-tts-nano](https://huggingface.co/WhissleAI/steer-tts-nano).
2. The files are locked until you fill in the short form on that page: your
   name and whether your use is personal/hobby or commercial. Read the licence
   on the model card first. A personal or hobby answer unlocks the downloads
   right away; commercial use needs a licence from Whissle.
3. Download two files:

   | File | Which revision | Size |
   |---|---|---|
   | `nano.bin` (the weights) | **`2516acf`** (v1.0.0), not the latest | 2,969,552 bytes |
   | `front.bin` (text front end data) | the latest | about 1.8 MB |

   The latest `nano.bin` is a newer format (v2) that the public C runtime
   can't read yet: the gadget then logs `no speech model` and replies stay
   text. Check the size of the one you downloaded. Its SHA-256 is
   `21ef6ac046cf859890e869ae1c36fa2ce69066469c7ee52e4d95b0ef4199862f`.

   With the Hugging Face CLI (`pip install huggingface_hub`), after the form:

   ```sh
   hf auth login        # offers a browser login, so no token has to be pasted
   hf download WhissleAI/steer-tts-nano nano.bin --revision 2516acf --local-dir models/v1
   hf download WhissleAI/steer-tts-nano front.bin --local-dir models/front
   ```

   A phone's browser may cut the download short; compare the size.

## 2. Get the runtime

Clone Whissle's runtime next to this repository, not inside it:

```sh
git clone https://github.com/WhissleAI/steer-tts.git
```

The build uses its `esp32/components/steertts_nano` component. It is under
the same non-commercial licence as the weights.

## 3. Build

Set `MUSE_STEERTTS_DIR` to the runtime's components directory and add the
overlay `devices/sdkconfig.muse-local-tts` after the board's own. For the
1.75C, from `esp32/`:

```sh
export MUSE_STEERTTS_DIR=/path/to/steer-tts/esp32/components
idf.py -B build-muse-175c-tts -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-175c-tts/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c;devices/sdkconfig.muse-local-tts" build
```

The overlay turns the option on and selects `partitions_muse_tts.csv`, which
has a 6 MB `steertts` partition at `0x830000`. Set your SDK token in that
build directory's `sdkconfig` as for any build.

## 4. Flash

Flash the firmware as usual, with the same `-B`, target and defaults
arguments and `-p PORT flash`. This changes the partition table: whatever the
`apps` and `files` partitions held is gone. Pairing and Wi-Fi are kept.

Then flash the model, once. It survives later firmware flashes:

```sh
python -m esptool --chip esp32s3 -p PORT -b 460800 write-flash \
  0x830000 models/v1/nano.bin \
  0xC30000 models/front/front.bin
```

`nano.bin` goes at the partition's start and `front.bin` 4 MB in.

## 5. Check it

- The boot log has `on-device speech ready`. If it says `no speech model` or
  `no front end data`, check the two offsets and the `nano.bin` revision.
- With a serial console open, the key `t` speaks a test sentence with no
  network.
- Settings > Sound > **Device voice** turns it on and off (on by default).
  Off: replies are shown as text.

## How it behaves

- The reply is split into sentences. Each one is synthesized, buffered and
  then played, so speech starts a moment after the text and there may be a
  short gap between long sentences. On the 1.75C a sentence takes a little
  less time to make than to say.
- The voice plays at the speaker's top level whatever the Volume setting,
  because the model's output is quiet. Volume at 0 and the Speaker switch
  still mute it.
- Typed turns from the console aren't spoken, as before.
- It takes about 1 MB of PSRAM while the gadget runs.
