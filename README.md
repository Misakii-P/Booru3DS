# Booru3DS

A booru image board browser for the **Nintendo 3DS**, written in C with
citro2d/citro3d. Browse Safebooru or Konachan, view images on the top
screen, and save full-size originals to your SD card — or install them
straight into the Nintendo 3DS Camera app.

![3ds](https://img.shields.io/badge/platform-Nintendo%203DS-red)

## Features

- **Two image boards** — switch between `safebooru.org` and `konachan.net`
  with `SELECT` (both SFW).
- **Thumbnail grid** — a 4x3 grid of live thumbnails on the bottom screen
  with a selection highlight; D-Pad / L / R / touch to navigate.
- **Full-size viewer** — the selected post renders large on the top screen,
  first from the thumbnail cache, then sharpened when the full preview
  arrives.
- **Async networking** — downloads are pumped a little every frame, so the
  UI never freezes. Thumbnails, the big view and saves all download in
  parallel.
- **Save to SD** (`A` -> `A`) — streams the original file to
  `sdmc:/3ds/booru/` with no size limit (nothing large is ever held in RAM).
- **Install to 3DS Camera** (`A` -> `X`) — converts any downloaded image
  into a photo the Nintendo 3DS Camera app displays natively, complete
  with EXIF timestamps.
- **Background music** — streaming WAV looper with ~750 ms of buffered
  audio so image decoding never causes audio stuttering.
- **Hardware-minded** — RGB565 thumbnails, Morton-tiled GPU textures
  verified byte-exact against tex3ds, static scratch buffers everywhere
  (no per-image heap churn), libcurl over mbedTLS for real-hardware HTTPS.

## Controls

| Button | Action |
| ------ | ------ |
| A | Open save prompt (in grid) / confirm "save to booru folder" |
| X | Search (also: tap search bar) / confirm "install to camera" |
| B | Back to title screen / cancel prompt |
| SELECT | Switch board (safebooru.org <-> konachan.net) |
| L / R | Previous / next page |
| D-Pad | Move selection |
| START | Exit |

## Building

Requires [devkitARM](https://devkitpro.org/) with libctru, citro2d,
citro3d, and turbojpeg from portlibs:

```sh
pacman -S 3ds-libctru 3ds-citro2d 3ds-citro3d 3ds-libjpeg-turbo
make
```

`stb_image.h` is vendored in `source/`; everything else is standard
devkitPro toolchain. Output is `booru3ds.3dsx` — run it with your favourite
homebrew launcher.

## Camera install caveats

The Nintendo 3DS Camera app was never meant to accept foreign photos, so a
few quirks apply. Booru3DS works around them the same way SCR2JPG does:

1. **Images are resized to fit 640x480.** The camera's decoder refuses to
   index anything larger — files above that size simply never appear, with
   no error anywhere. This is a hard limit.
2. **Only baseline JPEGs are indexed.** Progressive JPEGs and PNGs are
   transparently decoded and re-encoded before writing.
3. **Each DCIM folder holds 100 photos** (`100NIN03`, then `101NIN03`, ...).
   Booru3DS picks the first folder with a free slot and the next free
   `HNI_####` number automatically.
4. **New photos do not appear immediately.** The camera caches its library
   in an internal management file. After installing, open the camera and go
   to *Settings -> Data Management -> SD Card to System*, confirm with OK,
   answer **No** to the copy dialog, then back out. Your installed images
   will now be listed (dated correctly, thanks to injected EXIF).

## Why konachan.net exists in the app

Safebooru.org sits behind Cloudflare, which rejects the TLS handshake of
the 3DS system SSL module outright (HTTP 403). Booru3DS therefore uses
libcurl + mbedTLS for HTTPS, whose software TLS fingerprint passes — but if
you ever see `fail(...)` errors on safebooru, press `SELECT` to switch to
konachan.net, which accepts the console without any tricks. In emulators
(Azahar), both sites work because requests originate from your PC's network
stack.

## Credits

- [SCR2JPG](https://github.com/Adrix12team/SCR2JPG) — reference for the
  DCIM conventions that make the camera accept imported photos.
- [stb_image](https://github.com/nothings/stb) — PNG/BMP decoding.
- devkitPro, libctru, citro2d/citro3d, libjpeg-turbo, cURL/mbedTLS.

## License

MIT
