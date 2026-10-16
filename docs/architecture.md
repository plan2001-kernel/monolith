# Plan2001:n arkkitehtuuri (16.10.2026)

Plan2001 on moderni, natiivi 64-bittinen käyttöjärjestelmä 9frontin
pohjalta. Se ei ole 9front-yhteensopiva, ja legacy jää pois.

| Arkkitehtuuri | Alusta | Tila |
|---|---|---|
| AMD64 | UEFI | käytössä (pc64, `docs/boot-abi-amd64.md`) |
| ARM64 | UEFI | suunniteltu, QEMU-kokeilu (`docs/boot-abi-arm64.md`) |
| RISC-V64 | UEFI | tuleva (`tools/targets/riscv64`) |

Kerneli, prosessinhallinta, tiedostojärjestelmä, verkkopino ja palvelut
pyritään pitämään yhteisinä. Arkkitehtuurikohtaisia ovat prosessorin
käynnistys, MMU, kontekstinvaihto, poikkeukset ja keskeytykset sekä
alustan ajurit. Boot-rajapinta on kaikille sama BootInfo
(`docs/boot-abi.md`).

## Profiilit

- **Palvelin:** CPU-, auth- ja tiedostopalvelin. Siihen kuuluvat
  tunnukset, istunnot, laskentapooli (`docs/cpu-server-design.md`) ja
  WireGuard (`docs/wireguard.md`). Pilvessä: `docs/cloud.md`.
- **Konsoli:** sarjakonsoli ja natiivi konsoli (rivieditori, `kbdfs`,
  `devcons`, `vga.c`).

## Terminaalit ovat verkkokoneita

Plan2001 ei sisällä selaimen virtuaalikonetta. Terminaali on mikä
tahansa kone, joka puhuu Plan2001:n kanssa 9P:tä, dp9ik:ta ja rcpu:ta:
toinen Plan2001-kone, 9front, Linux 9P-asiakkaalla (9pterm) tai
selaimessa ajettava [Plan9-wasm32](https://github.com/pereuna/plan9-wasm32).
Plan2001:n entinen wasm32-kone ja selainpalvelut ovat Monolithissa
(`docs/plan9-wasm32.md`).

Aiempi kerrosmalli (arkkitehtuuri, alusta, rooli, ohjelmat) ja vaiheet
C–D ovat Monolithin `docs/architecture.md`:ssä ja tämän repon
historiassa (commit `f399452`).
