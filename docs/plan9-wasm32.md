# Plan2001, Plan9-wasm32 ja Monolith (päätös 16.10.2026)

Tämä päätös korvaa 7.10.2026 päätöksen, jonka mukaan Plan2001:n oma
wasm32-kone pysyi tässä repossa. Plan2001:ssä ei ole enää wasm32-koodia.

## Jako

| Projekti | Mikä | Repo |
|---|---|---|
| **Plan2001** | natiivi 64-bittinen käyttöjärjestelmä: AMD64, ARM64 ja RISC-V64, UEFI. Palvelin (CPU, auth, fs) ja sen työkalut | tämä |
| **Plan9-wasm32** | Plan 9 virtuaalikoneessa selaimessa, 9front-yhteensopiva: WASM32-ydin, 3c/3l, Web Worker -prosessit, WebAssembly-muisti, OPFS, virtuaalinen verkkokortti ja 9frontin TCP/IP | [pereuna/plan9-wasm32](https://github.com/pereuna/plan9-wasm32) |
| **Monolith** | Plan2001:n entinen wasm32-kone ja selainpalvelut sekä drawterm (9pterm). Siirretty historioineen tästä reposta | `~/Src/monolith` (ei vielä GitHubissa) |

Monolithiin siirtyivät:
- wasm32-ydin ja sen testit (`plan2001/sys/src/9/wasm32`), 3a/3c/3l,
  wasm32:n kirjastot ja arkkitehtuuritiedostot;
- selainpalvelut: webterm, aux/wsrcpu, signupd, passkeyd, weblimitd,
  auth/passkey, aux/seckeys, websession, `/rc/bin/apps`, `lib/app/*`,
  rc-httpd:n `plan2001-app` ja WebSocketit välittävä select-handler;
- `monolith/` (drawterm, 9pterm, rakennus- ja testityökalut),
  `tools/native-wasm32`, `tools/test-native`, `tools/cloud/accounts*`,
  `vm-cpu`:n selainosa (Monolithin `tools/vm-web`);
- dokumentit: `architecture.md` (vaiheet C ja D), `wasm32.md`,
  `boot-abi-wasm32.md`, `webauthn.md`, `webrtc.md`, `app-origins.md`,
  `9pterm.md`.

Viimeinen Plan2001:n commit, jossa ne olivat mukana: `f399452`.

## Miksi

- **Eri suoritusympäristöt.** Natiivi ydin perustuu prosessorin
  keskeytyksiin, MMU:hun ja fyysisiin laitteisiin. Selaimessa ei ole
  MMU:ta: wasm32:n `mmuswitch` ja muut ovat tyhjiä, prosessit ovat Web
  Workereita omine WebAssembly-muisteineen, järjestelmäkutsut kopioivat
  muistia, ja selain ajoittaa. Yhteinen ydinkoodi vaatisi, että jokainen
  muutos tarkastetaan molemmissa.
- **Kaksi eriytyvää wasm32-ydintä.** Plan2001:n kopio ja Plan9-wasm32
  erosivat jo (16.10.: 92 tiedostoa samoja, 45 eri sisältö, 34 vain
  Plan2001:ssä, enimmäkseen testejä). Verkko on toteutettu eri tavoin
  (Plan2001: WebSocket- ja WebRTC-kuljetin `devwsnet.c`; Plan9-wasm32:
  virtuaalinen Ethernet).
- **Selkeys.** Plan2001 on natiivi käyttöjärjestelmä. Terminaali on
  verkkokone, joka puhuu 9P:tä.

## Järjestelmien yhteys

```
             Plan2001: 64-bittinen ydin
         AMD64        ARM64        RISC-V64
                        |
                       9P
                        |
     Plan9-wasm32 (selaimessa), Plan2001, 9front, Linux + 9P-asiakas
```

Plan2001:n ei tarvitse tietää, millainen sen terminaali on. Yhteiseksi
riittävät verkkoprotokollat (9P, dp9ik, rcpu, TLS), ei binääri-ABI eikä
ytimen sisäinen rajapinta.

## Mitä Plan2001:ssä on edelleen

- **9pterm** on ylläpidon työkalu (pilvi, CPU-VM). Se on Monolithin:
  `tools/9pterm` käyttää sitä (`MONOLITH`, oletus `~/Src/monolith`, tai
  `NINEPTERM`) ja kääntää sen tarvittaessa.
- **Staattinen https-sivusto** (`plan2001/rc/bin/rc-httpd`:
  select-handler ja `plan2001-static`): pilvi tarjoaa sillä toisen
  projektin valmiin sivun. Esimerkkejä ovat Plan9-wasm32:n live-image
  (`tools/cloud/plan9-wasm32`) ja Monolithin sivu hiekkalaatikossa
  (`tools/cloud/sandbox`).
- **Laskentapooli** (crsrv, rcc, `docs/cpu-server-design.md`) on
  palvelimen. Selaimet CR:inä ovat Monolithin tai Plan9-wasm32:n asia.
- **BootInfo** (`plan2001/sys/include/bootinfo.h`, `port/bootinfo.c`,
  `bootargs.c`): `BootArchWasm32` jää ABI:in. Monolithin wasm32-ydin
  käyttää näitä tiedostoja Plan2001:n checkoutista (`PLAN2001`).
- **`subset/9front`**: Monolith kääntää 9frontin kirjastot ja ohjelmat
  sieltä. Siksi `tools/subset/extra`:n wasm32-koneen tarvitsemat rivit
  (rcpu, rc-httpd, clock.c) jäävät toistaiseksi.

## Seuraavaksi

1. Monolithin julkaisu omana repona (GitHub, nimi päätetään; vanha
   `pereuna/monolith` on arkistoitu).
2. Monolithin omat ominaisuudet, joita Plan9-wasm32:ssa ei ole
   (devwsnet ja WebRTC, devwebauthn, sovellusten originit), siirretään
   Plan9-wasm32:een tai sen virtuaalisen verkkokortin päälle. Sen jälkeen
   Monolithin wasm32-ydin voidaan poistaa.
3. Plan2001 tarjoaa selaimelle Plan9-wasm32:n valmiin imagen eikä
   tarvitse wasm32-kääntäjää, ydintä eikä niiden testejä.

## Historia

- 5.10.2026: wasm32-kone tuotiin omaksi projektikseen Plan9-wasm32
  (commitista `59ee476`).
- 7.10.2026: Plan2001 ei ole 9front-yhteensopiva. Sen wasm32-kone jäi
  tähän repoon Plan2001:n omana koodina, ja puu yhdistettiin
  `plan2001/`:een (vaihe 3).
- 16.10.2026: wasm32 ja selainpalvelut siirrettiin Monolithiin (tämä
  dokumentti).
