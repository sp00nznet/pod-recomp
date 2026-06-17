# POD — a short history

> *"On the planet Io, there is only one ship left. There is only room for one."*

## The game

**POD** (in Germany: **POD: Planet of Death**) is a futuristic combat-racer released
by **Ubi Soft** in 1997 — an early, scrappy, gorgeous showcase of what a PC could do
when you fed it the right instructions.

The setup is bleak in the best late-90s way. In the distant future, the colony planet
**Io** is mining its own crust when the dig breaks open something it shouldn't: a fungus,
deep in the planet, that spreads across the surface and through the population. Most of
the colonists flee offworld. A handful are left behind — and there is exactly **one ship**
remaining, with room for **one**. So the survivors do the only reasonable thing: they
soup up their cars and **race for the last seat off a dying world.** The winner of the
final tournament escapes. Everyone else stays on Io with the fungus.

That premise — racing not for a trophy but for *survival* — gave POD a mean, desperate
edge that most of its candy-colored contemporaries didn't have.

## The MMX game

POD's real claim to fame is silicon. A pre-retail build (**POD 1.0 / "POD: OEM"**) was
**bundled with MMX systems in 1996**, making it one of the very first games to lean on
Intel's **MMX** instruction set. It shipped on Pentium / Pentium II MMX machines (and
some AMD K6 boxes), and Intel waved it around as proof that MMX was worth caring about.
For a lot of people, POD was the first thing that *ran* on their shiny new MMX PC.

This matters for us: the headline executable on this disc, **`PODMMX.EXE`**, is the
**MMX software rasterizer** — POD drawing the whole world with hand-tuned integer SIMD,
no 3D card required. That's the build this project recompiles. (See [BINARIES.md](BINARIES.md).)

## One game, nine renderers

3D acceleration in 1997 was the Wild West — there was no single Direct3D HAL you could
count on. So Ubi Soft did what studios did back then: **they shipped a separate, hand-tuned
executable for every graphics chip on the market.** This disc carries builds for the 3dfx
Voodoo (Glide), PowerVR, S3 ViRGE, ATI Rage, generic Direct3D, and the MMX software path —
a perfect fossil record of the pre-D3D-standardization era. (Full matrix in
[BINARIES.md](BINARIES.md).)

## Versions, and which one this is

- **POD / POD 2.0 (retail), March 1997** — the demo dropped March 5, the full game
  March 28. Retail added multiplayer, 3dfx support, redone physics, retouched car
  textures, and 4 extra tracks over the OEM build.
- **POD: Back to Hell / Extended Time (late 1997)** — an expansion: 8 more cars (plus
  bikes and some cartoonish vehicles), 3 more tracks, and a bonus disc of renderer patches
  (Voodoo, Voodoo Rush, PowerVR, S3 ViRGE, ATI3D, D3D, software).
- **POD Gold (1998)** — retail **+** expansion in one box: 32 tracks, 16 cars, the online
  **Ubi Soft Game Service** patch baked in.

This disc is **POD Gold, "MMX and Pentium Version 2.8"**, dated **September 29, 1997**
(per the on-disc `README.TXT`), volume label `POD2_0`. A European multi-language pressing —
the disc carries readmes in English, French (`LISEZMOI`), German (`LIESMICH`), Italian
(`LEGGIMI`), and Spanish (`LEAME`).

## Online before online was easy

POD Gold shipped with Ubi Soft's **Game Service** — matchmaking and ladders for up to
**8 players over the internet**, plus downloadable tracks and cars and uploadable replays,
years before that was table stakes. The `GS.DLL` on the disc is a fingerprint of it.

## Sources

- [POD (video game) — Wikipedia](https://en.wikipedia.org/wiki/POD_(video_game))
- [POD: Planet of Death Wiki (Fandom)](https://planetofdeath.fandom.com/wiki/POD)
- [POD: Extended Time / Back to Hell — Fandom](https://planetofdeath.fandom.com/wiki/POD:_Extended_Time)
- [POD: Back to Hell — MobyGames](https://www.mobygames.com/game/19771/pod-back-to-hell/)
- [POD: Planet of Death — PCGamingWiki](https://www.pcgamingwiki.com/wiki/POD:_Planet_of_Death)
- On-disc `README.TXT` / `FAQS.TXT` (Ubi Soft, 1997)
