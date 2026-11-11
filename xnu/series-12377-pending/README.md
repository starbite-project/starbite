# xnu-12377 patches held back from the series

Patches here are applied to test kernels only (the port tree build/xnu-12377 has them as commits); tools/build-xnu-12377.sh
applies only xnu/series-12377/*.patch, so daily kernels built from the series do not get them. Move one into
xnu/series-12377/ once its phone test has passed.

| Patch | Why held | Moves when |
|---|---|---|
| 0428-starbite-m8a-display.patch | Replaces SweetFramebuffer with StarbiteDisplay on every STARLTE boot (a 3 s wait then CPU copy without the board ADT; sweetfbdump/sweetfbcrop/a5fb_* gone) | M8a passed on the phone, and daily command lines have sbdisp=copy until M8b |
