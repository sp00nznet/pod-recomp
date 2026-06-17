# The disc: `POD2_0.BIN` / `.CUE`

- **Format:** BIN/CUE, **Mode1/2352**. Track 1 is the ISO9660 data track; **track 2 is
  redbook CD-audio** — POD's music is literally audio tracks on the CD.
- **Volume label:** `POD2_0`. The PVD system-identifier field reads
  `APPLE COMPUTER, INC., TYPE: 0002` — this is a **hybrid Mac/PC pressing** (POD shipped
  on Mac too), premastered with Apple tooling.
- **Contents:** 951 files, ~450 MB, extracted to `game_files/` by `tools/extract_disc.py`.
- **Multi-language:** a European pressing — readmes and string tables in English, French,
  German, Italian, and Spanish.

## Layout (selected)

```
/                        renderer EXEs (PODMMX, PODX3DFX, …), MOSAICO.EXE (autorun), readmes
/DATA/
  LOADER.EXE             in-game loader
  BINARY/
    CIRCUITS/            track data
    VOITURES/            cars            (Fr. "voitures")
    RESSON/              sound resources (Fr. "ressources son")
    POLICES/             fonts           (Fr. "polices")
    MIMG/  DICOS/  SCRIPTS/  VIDEO/      images, string dictionaries, scripts, cutscenes
  RECORDS/               *.BRT best-race times, one per track
                         (Alderon, Beltane, Burrow, Canyon, City, Cocoon, Downtown,
                          Galleria, HQ, …)
/OSD/                    Ubi Soft Game Service online client (CLIENT.EXE, POD2_0.DLL, …)
/DIRECTX/                bundled DirectX redist (DDRAW, DSOUND, DINPUT, D3D, DPLAY)
/3DFX/  /POWERVR/  /MAXI64/   per-vendor 3D driver installers
/INSTDATA/               installer art + STRINGS.{ENG,FRA,GER,ITA,ESP}
```

The French directory and resource names (`VOITURES`, `POLICES`, `RESSON`) are a tell:
POD was made in France by Ubi Soft.

## Re-extracting

```
py -3.11 tools/extract_disc.py POD2_0.BIN game_files
```
