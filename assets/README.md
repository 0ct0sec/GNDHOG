# GNDHOG ZERO mascot

`gndhog-zero-source.png` is the original mascot supplied by 0ct0 for this
project, retained byte-for-byte. The artwork is not redrawn or generated.

`gndhog-zero_100.png` is the APPLaunch icon. `src/mascot_data.h` embeds a
112x112 one-bit version for the About screen without a runtime PNG dependency.
Both preserve the whole badge using center-sampled nearest-neighbour pixels
and a fixed black/white threshold. Regenerate on Windows with:

```powershell
powershell -NoProfile -File tools/pack-mascot.ps1
```
