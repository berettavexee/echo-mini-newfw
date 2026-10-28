# Nouveau firmware ECHO MINI : étude de faisabilité (étape 1)

Document de travail local, en français, comme `re/REPORT.md`. Rien de ce dossier n'est publié. Le
SDK Rockchip (`re/sdk_ref/`) et le TRM (`re/doc_rknanod/`) ont servi de référence de lecture : on n'en
recopie rien, on ne cite que des noms et des adresses observés dans le firmware.

## 0. Objectif et architecture visée
- **Cœur A (système)** : un firmware neuf et léger, sous BSD. Il comprend le navigateur trié, une
  interface proche de RECHO, la gestion de l'énergie avec ajustement dynamique des fréquences, le mode
  disque USB et la mise à jour.
- **Cœur B (HiFi)** : un module par famille de formats, chargé à la demande comme dans le firmware
  d'origine, avec deux sources :
  - nos modules recompilés depuis des sources ouvertes, avec une structure de retour d'information
    (erreurs, cycles, fréquence) ;
  - les modules d'origine, quand il n'existe pas d'équivalent libre et rapide (MP3 accéléré). Ils
    sont extraits à l'installation du `HIFIEC39.IMG` de l'utilisateur, jamais redistribués.
- **Sans égaliseur** (demande de l'utilisateur, 2026-10-17). Le PCM du cœur B part vers l'I2S sans
  passer par les conversions 16 → 24 → 32 bits du firmware d'origine (§4).

## 1. Protocole hôte des codecs : validé en émulation
Outil : `tools/emu_host.py FW.IMG MODULE FICHIER SORTIE.raw [--seconds S] [--trace]`.
- Le module démarre **depuis son vecteur de reset**, avec son runtime, et ne communique avec le cœur
  A que par la boîte aux lettres.
- Le faux cœur A sert le fichier et lit le PCM.
- Différence avec `tools/emu74.py` : ce dernier appelle directement la fonction du décodeur.

### 1.1 Boîte aux lettres `0x40110000` (instance 0, pas de 0x50)
Le TRM ne la décrit pas ; la carte qui suit a été reconstituée à partir du pilote du module 74
(`0x102b4`–`0x10490`).

| Offset | Sens | Rôle |
|---|---|---|
| `+0x00` | A → B | autorisation des interruptions, 1 bit par canal |
| `+0x04` | A → B | état : écrire 1 pour effacer |
| `+0x08 + 8·ch` / `+0x0c + 8·ch` | A → B | commande / donnée du canal `ch` (0 à 3) |
| `+0x28` | B → A | autorisation des interruptions |
| `+0x2c` | B → A | état |
| `+0x30 + 8·ch` / `+0x34 + 8·ch` | B → A | commande / donnée. **C'est l'écriture de la donnée qui envoie le message** (le runtime écrit la commande, puis la donnée). |

Interruptions du cœur B (numéros d'exception, table en RAM à VTOR = `0x01022000`, remplie par
`0xf48(n, handler)`) :
- `0x1a` : canal 1, contrôle ;
- `0x1b` : canal 2, fichiers.

### 1.2 Messages
**A → B, canal 1 (contrôle)**

| Commande | Effet |
|---|---|
| 1 | OPEN |
| 4 | DECODE |
| `0xb` (donnée = ms) | SEEK |
| `0xd` | CLOSE |
| 7, 9, `0xcb`, `0xcd` | internes, non utilisées ici |

**A → B, canal 2 (fichiers)**
- `0x65` : descripteur de fichier ; la donnée pointe vers une structure **dans la RAM du cœur A**,
  que le cœur B lit lui-même. Format : `u8 handle, u8 h2, u8 h3, pad | u32 taille | u32 pos | u32
  pos2 | u32 pos3 | u32 type de codec`. Les deux handles supplémentaires ne servent qu'à l'AAC et à
  l'ALAC (types 2 et 7), qui lisent le MP4 en trois endroits.
- Réponses : `0x6a` (seek, donnée 0 = OK), `0x6c` (lecture, donnée = octets lus), `0x74`
  (fermeture).

**B → A, canal 0 (modules 74, 76, 77) ou canal 1 (modules 75, 78) : contrôle**

| Message | Sens |
|---|---|
| 1 | prêt (après le reset) |
| 2 | échec de l'ouverture |
| 3 | ouvert ; donnée = &info |
| 5 | tampon décodé ; donnée = &info |
| 6 | fin ou erreur |
| `0xc` | seek fait |
| `0xe` | fermé |

**B → A, canal 2 : fichiers**
- `0x66` : descripteur pris en compte.
- `0x69` : seek `{off, whence, h}`.
- `0x6b` : lecture `{buf, taille, h}` ; le cœur A écrit directement dans la RAM HiFi.
- `0x73` : fermeture (donnée = handle).
- **Point à respecter** : le cœur A doit **réécrire la position résultante** dans le bloc de requête
  (en `+0` pour un seek, en `+4` pour une lecture). Le runtime la recopie dans sa table de
  positions. Sans cela, la lecture repart du mauvais endroit après le calcul de la durée en fin de
  fichier (constaté : 25 440 trames de décalage avant la correction).

**Structure info** (`&info`) : `time, length (ms), bitrate, bps, samplerate, channels, _, pcm,
frames` (voir `re/opus/codec_abi.h`).
- **Le message 3 porte déjà un tampon.** L'Opus, le Vorbis et l'AAC y mettent le premier bloc
  décodé ; le WAV y met 4 096 trames de zéros.
- Le firmware d'origine les joue de la même façon, et ses 3 tampons muets au démarrage les masquent.

### 1.3 Résultats (image HIFIEC58, 4 à 5 s par fichier)
| Module | Format | Résultat |
|---|---|---|
| 74 | Opus (notre libopus) | **identique au bit près** à `emu74.py` (déjà validé) ; 72,5 dB face au décodage flottant du PC |
| 77 | Vorbis (notre Tremor) | **identique au bit près** à `emu77.py` ; 83,6 dB |
| 76 | WAV (d'origine) | **identique au fichier source**, après les 4 096 trames du tampon d'ouverture |
| 75 | AAC-LC (d'origine, Helix) | 90,4 dB face à ffmpeg, erreur max 1 LSB ; trois handles, fermeture comprise |
| 78 | FLAC (d'origine, LPC du HIFIACC) | **identique au bit près** au fichier source sur 8 s, grâce au modèle du HIFIACC et du DMAC2 (§1.5) |
| 79 | ALAC (d'origine) | pas encore : prédicteur adaptatif à modéliser (§1.5) |
| 73 | MP3 (d'origine) | attente sans fin des accélérateurs IMDCT36 / SYNTH, non émulés. Même remarque. |

**Conclusion** : le protocole est assez compris pour qu'un nouveau cœur A pilote les modules
d'origine et les nôtres. La partie qui précède le reset (horloge, reset, bloc de paramètres) est décrite au §1.4 ; il
reste à la confirmer sur le matériel.

Bloc de paramètres : `+0x20` = fréquence du cœur, lue par la temporisation du runtime (`0xb6c`) ; à
zéro, il n'y a pas d'attente. C'est la valeur de 47 MHz, périmée, relevée par la sonde HIFIEC64.

### 1.4 Démarrage du cœur B par le cœur A (firmware d'origine, `0x0306e478(module)`)
1. `0x03052c24(2, 1)` : horloge `hclk_cal_core` activée (`CRU_CLKGATE_CON0`, bit 2 ; écrire
   `0x10000 << bit` active l'horloge, `0x10001 << bit` la coupe, d'après le masque d'écriture).
2. `0x03052c94(0x32, 1)` : cœur B mis en reset (`CRU_SOFTRST_CON3` en `0x401800cc`, bit 2,
   `cal_core_srst`, qui vaut 1 à la mise sous tension).
3. `0x0306913c(module, 7)` : module chargé dans HIGHRAM0 et HIGHRAM1. Le cœur B voit HIGHRAM0 à
   l'adresse 0, donc sa table de vecteurs est au début du code du module. Puis 5 ms d'attente
   (`0x03065cd0(5)`).
4. Copie de 0x90 octets de `0x03006630` vers `0x01020000` : c'est la structure `chip_freq` du SDK
   (pll, armclk_src, armclk, armclk2_src, armclk2, stclk/hclk/fclk du cœur A, puis du cœur B…),
   remplie par `0x0305222c`.
   - **Les modules d'origine (73 à 82) n'en lisent que `+0x20` (stclk_cal) et `+0x24`
     (hclk_cal)**, en Hz : temporisations (`0xb6c`, `0xb90`) et période du SysTick (`0x852`).
   - Le firmware d'origine ne recopie le bloc qu'au démarrage du cœur B, d'où les 47 MHz périmés
     vus par la sonde HIFIEC64. Notre cœur A pourra réécrire ces deux mots à chaque changement
     d'horloge.
5. `[0x03004138] = 0`, reset relâché (`0x03052c94(0x32, 0)`). Attente de `[0x03004138] ≠ 0` :
   au plus 200 000 tours de 1 ms, avec rafraîchissement du chien de garde.
   - Cette valeur est posée par l'ISR A de la boîte aux lettres (`0x0306e4da`) sur le message 1
     (« prêt ») ; le message 3 pose `[0x0300413c]`.
6. Arrêt (`0x0306e534`) : resets 0x20, 0x21, 0x22, 0x32 et 0x1e, puis coupure d'une série
   d'horloges.

**Recoupement avec RECHO 0.10.0** (émulé) :
- **Mise sous tension** (`0x03064c90`) :
  - `PMU_PWRDN_CON` (`PMU+0x0c`) = 0, ce qui **alimente le domaine PD_HIGH** (cœur B, HIFIACC,
    HIRAM et HDRAM, TRM chap. 5). Ce domaine doit être alimenté avant toute écriture en HIGHRAM ;
  - horloges : `CRU+0xa4`, `+0x90` = `0x170f0000`, `+0x8c` = `0x1ff0000`, `+0x80`, `+0x98` et
    `+0x9c` ; `CLKSEL0` = `0xf7000000` (diviseur du cœur B à 1, source PLL) ;
  - resets du bloc HiFi (`CRU+0xc4`, `+0xc8`, `+0xcc`), puis effacement des états de la boîte
    aux lettres.
- **Démarrage** (`0x03064d9c`) :
  1. `CRU+0xcc` = `0x40004`, c'est-à-dire `SOFTRST_CON3` bit 2 = 1 : **le même reset du cœur B
     que dans l'origine** (id `0x32`) ;
  2. 5 µs d'attente, puis états A2B et B2A de la boîte aux lettres effacés ;
  3. `CRU+0xcc` = `0x40000` : reset relâché.
- RECHO utilise son propre bloc partagé en `0x01020000`–`0x01020fff` (`0x01020104`, `0x01020050`,
  anneau d'entrée à `0x01024000`…) et non le protocole d'origine du §1.2. Les deux sont possibles
  avec le même démarrage.
- **RECHO 0.10.0 fait tourner un égaliseur sur le cœur B** (« B's equaliser self-test … the C
  sections run », « dsp %u us for %u frames, %u sections … gain %d/2^28 »). Si l'utilisateur veut un
  jour un égaliseur, c'est là qu'il faudrait le placer.

**Horloge du cœur B** (TRM, `CRU_CLKSEL0` en `0x40180014`, avec masque d'écriture) :
- bit 15 : source, PLL ou PHY USB à 480 MHz ;
- bits 14:12 : diviseur, `clk = src / (n + 1)` ;
- bits 10:8 : diviseur de stclk.

Pour l'ajustement dynamique, il suffit de changer ce diviseur, sans toucher à la PLL. RECHO le
fait (« B /%u -> /%u (%u MHz; busy %u%%) »). Il faudra ensuite réécrire `+0x20` et `+0x24` en
HIGHRAM1, puis relancer le SysTick du cœur B, ou laisser nos modules le recalculer.

### 1.5 LPC du HIFIACC (FLAC), lu dans RECHO et recoupé avec l'origine
Le TRM ne donne pas de carte des registres (chap. 26). Elle est reconstituée à partir du module B
de RECHO 0.10.0 (`0x01001618`), qui fait aussi un auto-test comparant le matériel au logiciel
(« hifi: self-test… narrow/wide… exact »).

| Adresse | Rôle déduit |
|---|---|
| `0x01060000` | configuration générale (`0x01080108` à l'initialisation, `0x010015fe`) |
| `+0x04` | bit 15 : mode LPC actif (mis avant le calcul, effacé après) |
| `+0x08` | état : bit 15 = calcul terminé |
| `+0x0c` | `0xa0000000` \| (taille de bloc << 15), puis \| 3 pour lancer |
| `+0x2c` | (ordre << 16) \| (décalage << 8) |
| `+0x40` | FIFO d'entrée : résidus, écrits par le DMAC2, canal 1 |
| `+0xa0` | FIFO de sortie : échantillons, lus par le DMAC2, canal 0 |
| `0x0106b000` | mémoire des coefficients, en mots de 32 bits, **ordre inverse** (H(i) = h(N−1−i), comme dans le TRM) |
| `0x01069000` | une autre mémoire, utilisée par l'ALAC d'origine (module 79) |

- **DMAC2** (`0x01070000`) :
  - canal 1 : SAR = résidus, DAR = `+0x40`, `CTL` = `0x02109125`, ou `0x1a109125` avec une liste
    chaînée pour les gros blocs ;
  - canal 0 : SAR = `+0xa0`, DAR = sortie, `CTL` = `0x00a09425` ou `0x18a09425` ;
  - `CFG` = `0x400` / `CFG_H` = 7 pour le canal 0 ; `0x800` / `0x807` pour le canal 1 ;
  - requêtes matérielles 0 = hifi rx et 1 = hifi tx (TRM, tableau 2-2) ;
  - lancement : `ChEn` = `0x202`, puis `0x101`.
- **Fin** : `RawTfr` du canal 0 (`+0x2c0`, bit 0), puis le bit 15 de `+0x08`. Au-delà de 20 ms,
  RECHO passe en logiciel pour cette classe de sous-trames (« hifi: timeout … software from here »).
- **Recoupement** : le module 78 d'origine utilise les mêmes adresses (`0x01060040`,
  `0x010600a0`, `0x0106b000`) et les mêmes registres du DMAC2. Les modules 79 (ALAC) et 80 (APE)
  utilisent `+0x40`/`+0xa0` et des registres voisins.
- **Modèle dans `tools/emu_host.py` (classe `Accel`)** :
  - FIR exact : y[n] = x[n] pour n < ordre, puis y[n] = x[n] + (Σ coef[k]·y[n−ordre+k]) >> décalage,
    avec coef[k] lu à `0x0106b000 + 4k` ;
  - DMAC2 avec listes chaînées, et l'IRQ `0x38` levée en fin de transfert si le canal est
    démasqué ;
  - registres d'un canal lus au premier transfert réel.
- **Résultat avec le module FLAC d'origine** (2026-10-18), sur un fichier de test de 20 s, par
  blocs de 4 608 : **identique au bit près au fichier source sur 8 s**, en stéréo L/S comme en
  canaux indépendants, chirp et sinus à −3 dBFS compris, après les 4 608 trames muettes du tampon
  d'ouverture.
  - La carte des registres et la sémantique du FIR sont donc **validées** : ordre et décalage dans
    `+0x2c`, coefficients inversés, chauffe puis résidus dans la FIFO, sortie sur place, fin
    signalée par l'IRQ `0x38` du DMAC2.
  - Deux pièges d'émulation corrigés en chemin :
    1. ne jamais arrêter Unicorn dans un rappel MMIO pour lever une interruption, sinon il
       réexécute l'écriture en cours ; l'IRQ reste en attente jusqu'au prochain WFI ;
    2. le module réarme le canal 0 après l'interruption : un canal lit ses registres à son
       premier transfert, pas à son activation.
  - Les fichiers sont servis immédiatement. L'option `EMU_LATENCY=1` les retarde et casse le
    décodage : le runtime d'origine compte sur des données présentes à temps.
- **ALAC (module 79)** : pas encore. Son prédicteur est adaptatif (« coef variable FIR » dans le
  TRM, mise à jour des coefficients à chaque échantillon, mémoire `0x01069000`). Il faudrait un
  second modèle, à écrire d'après l'algorithme public de l'ALAC et à valider de la même façon.
- **APE (module 80)** : non testé, faute d'encodeur sur le PC.

## 2. Mise à jour et démarrage : le point critique
- **Le firmware d'origine écrit lui-même la mise à jour** : l'overlay 8 (`0x0307aa24`–) suit la
  logique du `FwUpdate` du SDK.
  - Il vérifie la signature `RKnanoFW` et compare l'en-tête avec sa copie de fin.
  - Il écrit la **2ᵉ copie** (FW2) par blocs de 16 secteurs (8 Ko), avec relecture et comparaison
    (`0x0307aa24` : écriture `0x030691de`, puis relecture `0x03069196` et `memcmp`). L'en-tête est
    écrit **en dernier**.
  - Au redémarrage, la copie valide la plus récente (date d'en-tête) est recopiée sur l'autre
    (`0x0307aa98`, équivalent de `FwUpgradeRecovery`).
  - Cela corrige le §5 du rapport, qui supposait l'écriture faite par le chargeur.
- **Conséquence** : un firmware réécrit doit embarquer son propre programme de mise à jour, comme
  RECHO (« FW1 and FW2 written and verified »). S'il démarre mais que sa mise à jour ne fonctionne
  pas, seul le mode maskrom permet de récupérer l'appareil.
- **Non connu** : le chargeur de l'IDB (zone de démarrage de l'eMMC, absente du `.IMG`).
  - Comment choisit-il la copie qu'il démarre, FW1 ou FW2 ?
  - Que transmet-il au firmware ? Les chaînes de RECHO parlent de « loader slot/geometry handoff ».
- **Filet de sécurité possible** (à valider) : écrire notre firmware dans une seule copie et laisser
  celle d'origine dans l'autre, sans la « réparer ». Une touche maintenue au démarrage effacerait
  l'en-tête de notre copie, puis redémarrerait pour revenir au firmware d'origine. Cela dépend de
  la règle de choix du chargeur.

### 2.1 Chargeur de démarrage : comparaison entre le firmware d'origine et RECHO 0.10.0
Image RECHO analysée : `HIFIEC90.IMG` **0.10.0**, build `6c14e3f8`, daté du 2026-10-15
(SHA-256 `47d5dc0b…`). Elle a remplacé la 0.9.2 du premier examen. Elle ne gère toujours pas l'Ogg ;
son module du cœur B grossit (`0xbe74` octets).

**Géométrie de l'eMMC** : identique dans les deux firmwares, qui lisent l'IDB de la même façon
(origine `0x0305f5fc`, RECHO `0x03052640`).
- 5 exemplaires de l'IDB, lus à partir du secteur 64 (32 Kio) tous les 1 024 secteurs (512 Kio),
  2 secteurs chacun.
- Secteur 0 : magic `0xFCDC8C3B`. Secteur 1 : `NANO` (`0x4F4E414E`) à `+0x0a`, puis la **taille de
  la zone système en Mio** (u16) à `+0x02`.
- P = Mio × 2 048 secteurs, et R = P/2 = la position de FW2. Le firmware d'origine les range en
  `0x0301e2bc` (P) et `0x0301e2c0` (R) ; l'overlay 8 de mise à jour utilise R.
- FW1 commence au secteur `0x2000` (4 Mio), FW2 à `0x2000 + R`, et le volume FAT utilisateur à
  `0x2000 + P`.
- RECHO s'attend à trouver le volume à cette place, avec `0x66000` pour l'origine (donc
  vraisemblablement P = `0x64000`, soit 200 Mio, à confirmer). Sinon, il le cherche par pas de 1 Mio
  (« volume @%x, NOT at stock's place »).
- Les 3 à 4 Mio sont réservés à l'OTP d'après le SDK (non vérifié dans le firmware).

**Mesuré sur l'appareil** (`ECHOLOG.TXT` de RECHO 0.10.0, copie dans
`RECHO/ECHOLOG_avant_bootdump.TXT`, 2026-10-17) :
- eMMC de 15 269 888 secteurs (7,28 Gio).
- IDB copie 0 : **P = `0x1b000`** secteurs (54 Mio de zone système), **R = `0xd800`** (27 Mio par
  copie : FW1 en `0x2000`, FW2 en `0xf800`).
- Volume utilisateur en **`0x81000`** (258 Mio), et non en `0x2000 + P` = `0x1d000`.
  - Entre les deux, une zone de 200 Mio (`0x64000` secteurs). C'est très probablement la zone
    système de données invisible en USB (base de la médiathèque, §22 du rapport).
  - Le secteur `0x1d000` commence par `SAVE` puis `0x09172026`, la date de HIFIEC64 : informations
    système enregistrées par le firmware d'origine.
  - Taille du volume : `0xe0f000` secteurs.
- `fw1 1 fw2 1` : les deux copies sont valides. « resources @0 (legacy) » : chargeur d'origine, rien
  n'est transmis, et RECHO lit ses ressources dans FW1.
- `PMU_SYS_REG3` vaut `0xffff18df` au démarrage. Ce n'est pas l'étiquette `0x18af` (le test
  masque le bit 4) ; c'est peut-être une valeur laissée par le chargeur, à comprendre.
- CPU à 200 MHz (PLL 200 MHz), dalle d'identifiant `0x66666666`.

**Ce que fait le chargeur de l'IDB** (déduit, puisque son binaire n'est pas dans le `.IMG`) :
- Il **charge lui-même l'entrée 0** de la table des overlays (code, données, BSS), puis saute au
  vecteur de reset. Indices :
  - le reset d'origine `0x0306296e` est une fonction C ordinaire qui ne recopie ni ses données ni
    son BSS ;
  - l'adresse de chargement des données d'origine (`0x030a3c38`) est hors de la SRAM.
- RECHO garde dans l'entrée 0 des tailles **identiques** à celles d'origine (`0x2666c`, `0x1afc`,
  `0x18c50`). Il recopie quand même ses données et met son BSS à zéro (`0x030500e4`), ce qui le rend
  indépendant de ce que fait le chargeur. **À reproduire.**
- **Passage de relais** : RECHO sauve r0 à r3, tels qu'ils sont à l'entrée, en `0x03040818`.
  - Un chargeur « source » (sans doute celui d'atzkey) y place le magic `0x52454348` /
    `0x424E4F31`, l'adresse de la copie démarrée (0 ou R) et P. Contrôle en `0x030528ec`, message
    « source loader ».
  - Avec le chargeur d'origine (« legacy »), rien n'est transmis, et RECHO suppose FW1.
  - Le firmware d'origine, lui, choisit sa copie seul : `FwCheck` dans l'overlay 1, date d'en-tête
    la plus récente. Ses overlays sont lus à `+R` si FW2 l'emporte.
- **Toujours inconnu** : la copie que le chargeur d'origine charge et démarre (FW1, FW2 ou la plus
  récente). RECHO l'évite en écrivant les deux copies à l'identique.

**Deux voies de récupération par logiciel, sans trombone** (firmware d'origine, overlay 9,
`0x0307ceb8`, l'équivalent de `SysReboot(addr, flag)` dans le SDK) :
1. **Mode USB du chargeur** :
   - écrire `0xffff18af` dans `PMU_SYS_REG3` (`0x5002004c`), registre conservé au reset, puis faire
     un reset système (AIRCR `0x05fa0007`) ;
   - le firmware d'origine le fait sur le message `0xbf` (flag 3), dont le déclencheur USB reste à
     trouver ;
   - RECHO le fait quand il trouve `MASKROM.REQ` sur le disque (`0x03050bcc`, fichier consommé, puis
     redémarrage `0x03051d70`) ;
   - au démarrage, RECHO efface une étiquette `0x18af` restée en place (« stale loader tag »).
2. **Mode de mise à jour de la ROM de démarrage** : remap de la ROM en 0, VTOR = 0, puis saut à
   l'adresse lue en `[0x8c]` de la ROM, après avoir écrit la fréquence du CPU en `0x03050000`
   (adresse `0x8c` dans `SysReboot`). Le firmware d'origine sait le faire (overlays 9 et 21).

**Autres fonctions de RECHO utiles pour nous** :
- `BOOTDUMP.REQ` produit `BOOTDUMP.BIN` (« ECHONANO BOOT CAPTURE v1 », apparemment les 4 premiers
  Mio : IDB et chargeur), puis `BOOTDUMP.OK` (`0x03059410`).
- « System backup » (« ECHONANO SYSTEM BACKUP v1 »).
- Démarrage par appui long sur Power (« hold to boot »).
- Repli de la PLL à 96 MHz si le démarrage précédent à 200 MHz n'a pas effacé son marqueur
  (`0x0309ffe0`, « FAST »).

### 2.2 Le chargeur d'origine, lu dans la capture (2026-10-17)
Capture : `BOOTDUMP.BIN` de RECHO, faite par l'utilisateur (secteurs 0 à `0x1fff`, relecture
vérifiée, SHA-256 `1e9b4e21…`), rangée dans `/home/beretta/code/REA/RECHO/`.
- Décodage par `tools/idb.py` ; fichiers extraits dans `newfw/loader/`.
- **Code Rockchip : à ne jamais publier.**

**Structure de l'IDB** (5 copies identiques, secteurs 64 à 116 de chaque bloc de 512 Kio) :
- **Secteur 0** : RC4 avec la clé publique de Rockchip ; magic `0x0ff0aa55`. Mots à `+0x38` :
  `0x14, 0x20, 4, 4, 0xc, 4`, soit l'étage 2 au secteur 20 (32 secteurs) et l'étage 1 au
  secteur 4 (4 secteurs), avec sa copie au secteur 12.
- **Secteur 1** : en clair. Taille système de 54 Mio à `+2`, `NANO` à `+0x0a`, 2016-05-12 à
  `+0x12`, version de chargeur 4.06 à `+0x16`.
- **Étage 1** : secteurs 4 à 7, 2 Kio, chiffré en RC4 secteur par secteur. Pile `0x03002000`,
  reset `0x0300010d`.
- **Étage 2** : secteurs 20 à 51, 16 Kio, RC4. Pile `0x03090000`, reset `0x0300008d`, lien en
  `0x03000000`. Chaîne « start loader 2016-04-19 ».

**Logique de démarrage de l'étage 2** :
- `0x030005f2` : lecture des 16 bits bas de `PMU_SYS_REG3` (`0x5002004c`).
  - `0x18bf` : saut dans la ROM, à l'adresse lue en `[0x8c]` (mode de mise à jour USB de la ROM,
    c'est-à-dire le maskrom).
  - `0x18af` : le registre est effacé et le chargeur passe en **mode USB du chargeur**, sans
    démarrer de firmware.
- **Sinon, test « USBBoot »** (`0x03000524`), indépendant de tout firmware : jusqu'à 500 lectures du
  SARADC, canal 1 (l'échelle des touches), tant que VBUS est présent (bit 1 de GRF `+0xe8`).
  - Il faut que la valeur reste **≤ `0x40`** et que le câble USB soit branché.
  - Sur cet appareil, **seule VOL− donne ≤ `0x40`** : environ 1, contre 124 pour VOL+ et 218 à 304
    pour Play, d'après les journaux de RECHO.
  - Donc **VOL− maintenue avec le câble USB branché au démarrage = mode USB du chargeur**, avec
    `rkdeveloptool`, même si le firmware est détruit, tant que l'IDB est intacte.
  - **Confirmé sur l'appareil le 2026-10-17**, sans aucune écriture. Lecteur éteint, VOL−
    maintenue, puis câble branché : `lsusb` montre `2207:262d`, bcdDevice 1.00, une seule
    interface de classe vendeur `ff/06/05` (Rockusb) avec les points d'accès 0x81 et 0x01, et
    aucun disque.
  - C'est le même identifiant que celui de la procédure communautaire « maskrom » (RST et toutes
    les touches sauf Power). Celle-ci ne fait pas de `db` et écrit directement avec `wl`, ce qui
    suppose que c'est le chargeur qui répond. Elle aboutit donc très probablement à ce même mode,
    puisque VOL− fait partie des touches maintenues.
  - bcdDevice 1.00 a le bit 0 à zéro, d'où le correctif « bcdDevice » de la procédure :
    `rkdeveloptool` prendrait sinon le chargeur pour la ROM.
  - **Sortie de ce mode** : une fois le câble retiré, le chargeur affiche « power down » et boucle
    sans fin (`0x030006d8`). Il ne coupe pas l'alimentation et ne démarre pas le firmware : le
    lecteur semble éteint et ne répond plus. Sur l'appareil, l'utilisateur en est sorti en
    débranchant et en appuyant sur **RST** (2026-10-17).
  - Une procédure de récupération documentée devra le dire, ou utiliser la commande de reset
    Rockusb (`rkdeveloptool rd`) avant de débrancher.
- **Sinon, « MemBoot »** (`0x030007d0`) : `PMU_SYS_REG3` sert de marqueur de tentative.

  | Marqueur trouvé | Action |
  |---|---|
  | autre (cas normal) | écrit `0x18df`, charge **FW1** ; en cas d'échec, si FW2 existe : écrit `0x18cf`, charge FW2 |
  | `0x18df` | FW1, puis FW2 en cas d'échec |
  | `0x18cf` | **FW2 directement** (FW1 si FW2 n'existe pas, en écrivant `0x18df`) |

  Le marqueur reste en place une fois le firmware lancé, d'où le `0xffff18df` relevé par RECHO.
- **Chargement d'une copie** (`0x03002a00`) :
  - en-tête de 512 o avec `RKnanoFW` à `+0x1f8`, comparé octet par octet à sa copie lue à
    `+FwEndOffset` (`[+0x1f4]`) ;
  - puis la table des overlays, à `[+0x6c]` ;
  - copie code et données des **entrées 0, 1 et 2**, et mise à zéro de leurs BSS ;
  - enfin VTOR, SP et PC pris dans la table de vecteurs au début des **données** de l'entrée 0
    (`0x03004000`). Le reset réel du firmware d'origine est donc `0x03056169`, et non le premier
    mot de son code ;
  - échec seulement si une lecture échoue ou si la signature ou la copie de fin ne correspondent
    pas. **Aucun CRC, aucune date, aucune version.**
- **Registres au saut** (`0x03000854`) : r0 = adresse de la table de vecteurs, r1 = SP, r2 = reset.
  Ce n'est pas le magic d'un « source loader », d'où le mode « legacy » de RECHO.

**Ce qui en découle**
- **Le chargeur démarre toujours FW1** quand il est valide. La règle de la date la plus récente
  est propre au firmware d'origine (`FwCheck`, overlay 1), qui choisit ensuite d'où lire ses
  overlays et recopie la copie la plus récente sur l'autre.
- **Récupération sans trombone, indépendante du firmware** : VOL− et USB au démarrage (à vérifier).
  Par logiciel, en plus : `0x18af` (mode USB du chargeur) ou `0x18bf` (ROM) dans `PMU_SYS_REG3`,
  puis reset.
- **Double démarrage possible** : une copie dans FW1 et une autre dans FW2. On démarre FW2 en
  écrivant `0xffff18cf` dans `PMU_SYS_REG3`, puis en faisant un reset ; le reset suivant sans
  marqueur revient sur FW1.
  - **Le firmware d'origine non modifié détruirait ce montage** : son `FwCheck` déclare
    invalide la copie la plus ancienne et la remplace par l'autre.
  - Il faudrait une variante d'origine patchée, sans recopie, ou deux copies de notre firmware.
- Pour notre firmware :
  - l'entrée 0 doit porter la table de vecteurs au début de ses données ;
  - les entrées 1 et 2 sont chargées aussi : les laisser vides ou valides ;
  - réécrire `PMU_SYS_REG3` à une valeur neutre une fois le démarrage réussi est inutile pour le
    chargeur, mais permet de distinguer un redémarrage par chien de garde.

**Conséquences pour notre firmware**
- Entrée 0 valide, mais copie des données et mise à zéro du BSS faites nous-mêmes, comme RECHO.
- **Toute première action au reset**, avant les horloges et l'écran : si la touche de secours est
  maintenue (ou si `MASKROM.REQ` est présent, une fois le disque monté), passer en mode USB du
  chargeur (`PMU_SYS_REG3` = `0x18af`, puis reset).
  - Seul un plantage avant cette lecture de touche obligerait encore à recourir au maskrom.
- Mise à jour : écrire FW2, puis FW1, avec relecture et l'en-tête en dernier, comme le firmware
  d'origine et RECHO.
- **Exigence de l'utilisateur (2026-10-17) : aucune vérification de version ni de date dans notre
  programme de mise à jour.**
  - Comme celui de RECHO, testé sur l'appareil par l'utilisateur, il doit accepter toute image
    `RKnanoFW` valide : signature, copie de l'en-tête en fin d'image, CRC. Rétrogradation et retour
    au firmware d'origine doivent donc rester possibles à tout moment.
  - **Écrire les deux copies** rend la règle « date la plus récente » du firmware d'origine (`FwCheck`)
    et du chargeur sans effet. Si l'écriture est interrompue entre FW2 et FW1, il reste deux copies
    valides, et l'appareil démarre sur l'une d'elles : on peut relancer la mise à jour.
- Lire l'IDB comme les deux autres (5 copies, magic, P), et ne **jamais écrire** dans l'IDB.

**Pour lever l'inconnue sur le chargeur** : obtenir son binaire.
- Soit avec le `BOOTDUMP` de RECHO, si l'utilisateur choisit d'installer RECHO, qui se désinstalle
  avec l'image d'origine.
- Soit avec une image de diagnostic dérivée du firmware d'origine : lecture brute des secteurs 0 à
  `0x2000` par `0x0305e77a(1, secteur, n, tampon)`, puis écriture dans un fichier.
- Le mode USB du chargeur peut aussi se tester sans rien écrire : y entrer, puis
  `rkdeveloptool ld` côté PC.

## 3. Inventaire des pilotes du firmware d'origine
Méthode : `tools/periph_map.py` relève les adresses de périphériques chargées par `ldr` ou
`movw/movt`, plus les tables d'adresses. C'est une borne basse : les pilotes accèdent souvent aux
registres par base + offset.

| Bloc | Fonctions repérées (résident sauf mention) | État |
|---|---|---|
| Démarrage | reset `0x0306296e`, horloges et GPIO `0x03052d70` / `0x03056a94`, vecteurs `0x030561c8` | à analyser |
| Horloges (CRU, PLL) | profils `0x030687b0` / `0x030684ea` (§30), CRU `0x030520b2` | compris |
| eMMC / carte SD | `0x0305eb34`, `0x0305eb70`, `0x0305ed2e`, `0x0305ed62`, `0x0305f0a0`, `0x0305fc10` (EMMC `0x60030000` et SDMMC `0x60020000`, même pilote) | à analyser |
| FAT32 / exFAT | couche FAT résidente (§19 et 20 : `0x0305acd0`…) | partiel ; à réécrire de toute façon |
| Écran 320×170 (LCDC/VOP `0x60070000`) | `0x030520b2`, `0x0306e628` ; overlay 19 | à analyser (séquence d'initialisation de la dalle) |
| DAC | deux circuits sur I2C, `dac60_Left` / `dac66_Right` (adresses 8 bits 0x60 et 0x66, probablement deux CS43131 en double mono), accès `0x0306168c` / `0x030617a8`, volume `0x030619ee` ; I2C `0x03061510` et `0x0305052c` ; table des bases I2C0-2 et GPIO0-2 en `0x0306f248` | partiel |
| I2S | I2S1 TXDR `0x40030024` : `0x03066392`, overlays 11, 14, 33 ; I2S0/1 `0x030505f2` / `0x030507aa` | partiel (§35, 36) |
| Touches / batterie (SARADC `0x400d0000`) | `0x03060710`–`0x030608c8` ; overlay 1 `0x03079dcc`, `0x0307a3c0` | à analyser |
| Rétroéclairage (PWM0/1) | `0x030531c2`, `0x03056980`, `0x03061c40`, `0x03061ca8` | à analyser |
| Chien de garde (WDT) | `0x03061dba`, `0x03061e04`, `0x03061e6c` ; rafraîchissement `0x03061da8` | à analyser |
| USB (OTG `0x60080000`) | résident `0x03065154`, `0x030680f8`, `0x03068304`, `0x030695cc` ; overlays 10 à 12 (stockage de masse) | à analyser ; le plus gros morceau |
| Cœur B | reset et chargement `0x0306e478` ; mailbox `0x0306e628` ; DMA `0x030609ac` | compris (§6, 8, 35) |
| Mise à jour | overlay 8 | compris (§2) |
| Veille, extinction (PMU, GRF) | `0x0306e628`, overlays 9 et 21 | à analyser |

### 3.0 PLL et horloges du cœur A (RECHO émulé, valeurs confirmées par son journal)
RECHO `0x030517d4(fréquence)`, ici 200 MHz (`0x0bebc200`). CRU en `0x40180000`, registres avec
masque d'écriture.

1. `MODE_CON` (`+0x10`) = `0x10000` : passage sur le quartz de 24 MHz.
2. `CLKSEL0` (`+0x14`) = `0xffff0000` (diviseurs à 1) ; `CLKSEL1` (`+0x18`) = `0x03000000`.
3. `PLL_CON1` (`+0x04`) = `0x80008000`, puis `0x40004000` (reset, puis veille de la PLL) ;
   `PLL_CON2` (`+0x08`) = `0x08000000`.
4. `PLL_CON0` (`+0x00`) = `0xffff1064` ; `PLL_CON1` = `0x11ff1046` ; `PLL_CON2` = `0x08000000`.
5. `PLL_CON1` = `0x40000000` (sortie de veille), puis attente du verrouillage (**bit 10** de
   `PLL_CON1`).
6. GRF `+0xc4` (`inter_con0`) = `0x30403040`.
7. `CLKSEL0` = `0xffff1001`, `CLKSEL1` = `0x03000200`, puis `MODE_CON` = `0x10001` (mode PLL).
   **Une courte attente est nécessaire après chaque écriture** (RECHO `0x0305191a`, origine `0x030523e8`) : sans elle, `MODE_CON` reste à 0 et le cœur tourne à 24 / 2 = 12 MHz (vérifié sur l’appareil, images 0.1 à 0.5 ; corrigé en 0.6 : `MODE_CON` relu à `0001`, 200 MHz).

Résultat relevé par RECHO sur l'appareil : `cpu 200000000 Hz, pclk 50000000 Hz, grf inter_con0
00003040, con0 00001064 con1 00009446 con2 08000000 mode 00000001 sel0 00001001 sel1 00000200`.
C'est exactement ce que donne l'émulation, `con1` incluant le bit de verrouillage.

**Précaution reprise de RECHO** : un marqueur `FAST` en `0x0309ffe0` (PMU-SRAM) est posé avant le
passage à 200 MHz et effacé après 5 s de fonctionnement stable. S'il est encore là au démarrage
suivant, RECHO reste à 96 MHz (« the last 200 MHz boot did not clear its marker »). C'est un bon
garde-fou contre une horloge trop haute.

Le firmware d'origine passe par sa table de profils (§30 du rapport), avec un calcul de PLL en
`0x030684ea`. Les deux méthodes conviennent ; la séquence de RECHO est la plus simple à reprendre
sous forme de valeurs.

### 3.1 Écran (firmware d'origine, overlay 4 à `0x03079208`, avec RECHO pour comparaison)
Outil : `tools/emu_lcd.py FW.IMG FONCTION…`. Il exécute une fonction de l'overlay 4 et journalise les
commandes et données envoyées au VOP, les appels GPIO et les temporisations.

**Bus** : VOP (`0x60070000`, TRM chap. 15) en mode MCU i8080 8 bits.
- Commandes écrites dans `VOP_MCU_CMD` (`+0x28`), données dans `VOP_MCU_DATA` (`+0x2c`).
- Accesseurs du firmware d'origine en `0x0306ea54`–`0x0306ec48` ; commande `0x0306ec38`,
  donnée `0x0306ec26`.
- Le firmware d'origine envoie les pixels **octet par octet par le CPU**, sans DMA.
  - Effacement de l'écran (`0x0307931a`) : 320 × 340 écritures.
  - Copie d'un rectangle (`0x0307934e`) : un demi-mot par écriture.
- **RECHO 0.10.0 pilote la dalle en GPIO pur**, en écrivant directement dans le registre de
  données de GPIO0 : par exemple 1 953 écritures pour un envoi (`0x0306af18`), 258 pour une
  réinitialisation (`0x0306ae04`).
  - Le chemin VOP et DMA existe dans son code (« lcd: VOP timing %04x », `VOP_MCU_TIMING` =
    `0x1021` à 200 MHz en `0x0306a21c`), mais il est désactivé dans cette version (« GPIO (no VOP
    path in this build) »).
  - Des marqueurs conservés au reset le font revenir au GPIO quand une tentative VOP ou DMA
    n'aboutit pas : le VOP avec DMA n'est donc pas encore fiable chez lui.
- Pour notre firmware : partir du VOP piloté par le CPU, comme le firmware d'origine, puis
  essayer la FIFO du VOP alimentée par le DMAC1 (requête 14), d'après le TRM chap. 15. RECHO montre
  qu'il faut prévoir un repli.

**Broches**

| Broche | Rôle |
|---|---|
| GPIO0_16 à GPIO0_23 | données D0 à D7 (iomux 1 = VOP) |
| GPIO0_12 | RD (lecture), basculée pour lire l'identifiant |
| GPIO0_24, GPIO0_25 | D/C et CS probablement (iomux 1 = VOP) |
| GPIO0_15 | probablement WR (iomux 1 = VOP), à confirmer |
| **GPIO2_15** | **reset de la dalle** : haut, 50 ms, bas, 150 ms, haut, 120 ms |
| GPIO2_9 | lue comme entrée avant la mise sous tension : sélection de variante |

**Mise sous tension** (`0x03079bfa`) :
1. Reset de la dalle.
2. Lecture de l'identifiant en GPIO (`0x030793ee` : 4 octets lus en basculant RD), rangé en
   `0x0301d124`, puis broches rendues au VOP.
3. Second reset.
4. Choix de la séquence :
   - si `(id & 0xff0000) == 0xe80000` : **variante B** (`0x030799be`) ;
   - sinon **variante A** (`0x03079752`). Si GPIO2_9 était bas, l'identifiant est remplacé par
     `0x66666666`, la valeur que RECHO affiche (« panel 66666666 ») ; les `ffffffff` lus ensuite
     par RECHO sont donc une lecture ratée ou une autre branche.
5. Effacement de l'écran.

**Séquences**, contrôleur de type ST7789 :
- **Variante A** :
  - `11`, puis 120 ms ;
  - `36` = `60` (paysage), `3a` = `05` (RGB565) ;
  - `a4 44 44`, `b2 0c 0c 00 33 33`, `b7 35`, `bb 1a`, `c0 2c`, `c2 01`, `c3 0b`, `c4 20`,
    `c6 0f`, `d0 a4 a1`, `d6 a1` ;
  - gamma : `e0 f0 06 0b 07 07 24 2e 32 46 37 13 13 2d 33` et
    `e1 f0 02 06 09 08 05 29 44 42 38 14 14 2a 30` ;
  - `2a 0000 00ef`, `2b 0023 00cc`, puis `21` (inversion), `29` (écran allumé), `2c`.
- **Variante B** :
  - `11`, puis 120 ms ;
  - `df 5a 69 02 01` (déverrouillage propre au fabricant) ;
  - `36 60`, `3a 05`, `b2 0b 0b 00 33 33`, `b7 75`, `bb 28`, `c0 2c`, `c2 01`, `c3 1f`,
    `c5 2a`, `c6 13`, `d0 a7`, `d0 a4 a1`, `d6 a1` ;
  - gamma : `e0 f0 05 0a 06 06 03 2b 32 43 36 11 10 2b 32` et
    `e1 f0 08 0c 0b 09 24 2b 22 43 38 15 16 2f 37` ;
  - `21`, `29`, `2c`.

**Fenêtre** (`0x0307928a(x0, y0, x1, y1)`) :
- `2a` donne les colonnes x0 à x1 (0 à 319) ;
- `2b` donne les lignes y0 + 35 à y1 + 35 (`0x23` : les 170 lignes de la dalle commencent à la
  ligne 35 du contrôleur) ;
- puis `2c`, et les pixels en RGB565, poids fort en premier.

**Autres** : `28 10` (écran éteint, veille, `0x03079210`) et `11 29` (réveil, `0x03079238`).

**Recoupement avec RECHO 0.10.0** : table de paramètres en `0x0306b980`–`0x0306b9f0`.
- On y trouve les deux gammas de chaque variante, les porches `0c 0c 00 33 33` / `0b 0b 00 33 33`,
  le déverrouillage `5a 69 02 01`, la fenêtre `0023..00cc` / `0000..00ef` et les autres valeurs,
  **identiques octet pour octet**.
- Juste avant, la liste de broches `10 11 12 13 14 15 16 17 18 19 0f 0c`, soit GPIO0_16 à 0_25,
  0_15 et 0_12 : le même bus.
- Les deux firmwares s'accordent donc sur la dalle et ses deux variantes.

Ces séquences sont des valeurs de configuration de la dalle, des faits matériels. Notre pilote
les reprendra sous la forme d'une table, avec notre propre code.

### 3.2 Touches, alimentation, batterie
Sources : le chargeur d'origine (test « USBBoot »), le code et les journaux de RECHO 0.10.0
(`ECHOLOG.TXT`, médianes sur l'appareil de l'utilisateur).

**SARADC** (`0x400d0000`), 10 bits :
- `+0x08` : contrôle (bits 2:0 = canal, bit 3 = démarrage, bit 4 = mise sous tension) ;
- `+0x04` : état, bit 0 = occupé ;
- `+0x00` : donnée.

| Canal | Rôle |
|---|---|
| 0 | batterie, pont diviseur 2:1 (« battery %u mV (ch0, 2:1) ») |
| 1 | **échelle des touches** |
| 2, 3 | varient entre 130 et 450 environ : détection des prises casque ou de la charge, à identifier |

Échelle des touches, canal 1 :

| Touche | Valeur (médiane) | Plage observée |
|---|---|---|
| VOL− | 1 | 1–2 |
| VOL+ | 126 | 124–179 |
| Play | 270 | 218–304 |
| Menu | 384 | 331–400 |
| Back | 515 | 481–545 |
| aucune | 1023 | 1010–1023 |

Le chargeur ne teste que « ≤ `0x40` », donc VOL−.

**GPIO** : banques GPIO0 `0x40160000`, GPIO1 `0x40170000`, GPIO2 `0x50030000`, avec le registre
de donnée en `+0`, la direction en `+4`, et l'iomux dans le GRF. Broches repérées :

| Broche | Rôle | Source |
|---|---|---|
| **GPIO2_5** | touche Power (entrée) | RECHO « keys: power pin » |
| **GPIO2_6** | **maintien de l'alimentation** : mis à 1 au démarrage, à 0 pour éteindre | RECHO `0x03051e28` |
| GPIO2_15 | reset de la dalle | firmware d'origine, overlay 4 |
| GPIO2_9 | sélection de variante de dalle (entrée) | firmware d'origine, overlay 4 |
| GPIO0_12 à GPIO0_25 | bus de la dalle | firmware d'origine, §3.1 |
| VBUS | bit 1 de GRF `+0xe8` | chargeur |
| GPIO2_0, GPIO2_14, GPIO2_2 | sorties mises à 1 avant les DAC : alimentations, probablement (§3.5) | RECHO `0x030562cc`, origine `0x0306296e` |
| GPIO2_1 | impulsion à 0 avant l'I2C : reset des CS43131, vraisemblablement | RECHO |
| GPIO2_10, GPIO2_11 | I2C0 (SCL et SDA, fonction 1) | RECHO, origine `0x03060c54` |
| GPIO1_0 à GPIO1_3 | I2S1 (fonction 1, « pad group A ») | RECHO `0x0305598c` |
| GPIO0_13 (1 = présente, **tirage interne coupé** : GRF `+0x14` = `0x00200020`, sinon la lecture flotte ; vérifié sur l’appareil, 0.3) | détection de prise : bit 0 de l’état « jack », la 3,5 mm d’après l’ordre « 0 none, 1 3.5 mm, 2 4.4 mm » | RECHO `0x03055ec0` |
| GPIO2_8 (0 = présente) | détection de prise : bit 1, la 4,4 mm | RECHO `0x03055ec0` |
| GPIO2_3 | état du chargeur, lu quand VBUS est présent | RECHO `0x03069eb8`, fonction batterie |
| GPIO0_11 (active basse, avec tirage) | probablement la détection de carte microSD | RECHO `0x03065890` |
| GPIO2_4, 2_7, 2_16, 2_17, GPIO1_4 (sorties) | à identifier | RECHO |

**Recoupement avec le firmware d'origine** :
- GPIO2_6 est mis à 1 en `0x03053924` et `0x03061f86` (maintien de l'alimentation) ;
- GPIO2_5 est lue en `0x030621f6` et `0x03062322` (Power) ;
- GPIO2_0, 2_2 et 2_14 sont des sorties pilotées au démarrage (`0x0306296e`, `0x03053924`).

Les deux firmwares concordent.

**Multiplexage des broches** (GRF, d'après RECHO `0x03051570`) : registre
`0x50010000 + 0x20·banque + 4·(broche >> 3)`, 2 bits par broche au décalage `2·(broche & 7)`,
avec un masque d'écriture dans les 16 bits hauts (`0x30000 << décalage`).

**Batterie** (RECHO `0x03069bfc`) : ADC canal 0 × 2.
- **3 400 mV** : « battery low: charge soon » ;
- **3 350 mV** au démarrage : extinction immédiate ;
- **3 300 mV** maintenus un certain temps : extinction.

RECHO estime aussi la pente en mV/min et le temps restant. La courbe de pourcentage reste à
relever.

**Démarrage par appui long** : RECHO n'allume que si Power est tenue assez longtemps (« hold to
boot »). Sinon, il relâche GPIO2_6 et s'éteint. Au démarrage sur câble, l'alimentation vient de
VBUS.

### 3.3 Rétroéclairage et chien de garde (firmware d'origine)
**Rétroéclairage** : PWM0 (`0x400f0000`), **canal 0, 10 kHz**, rapport cyclique de 1 à 100 %.
- Réglage : `0x03061d0c(canal, %, fréquence)`, appelé par `0x03065cfa` et `0x03065d14` avec
  canal 0 et 10 000 Hz. Il écrit, pour le canal n à `+0x10·n` :
  - `+0` CNT = 0 ;
  - `+4` PERIOD = pclk / fréquence ;
  - `+8` DUTY = PERIOD × % / 100 ;
  - `+0xc` CTRL = `0x00010003`.
- Démarrage du canal : `0x03061c40`. Il active l'horloge 0x90, fait un reset 0x17, puis met le
  bit 0 de CTRL à 1. Arrêt : `0x03061ca8`.
- Les canaux 4 à 7 sont sur PWM1 (`0x40100000`).
- La broche de sortie (iomux) reste à relever.

**Chien de garde** : DesignWare WDT en `0x40120000`.
- Rafraîchissement : écrire `0x76` dans `+0x0c` (CRR), en `0x03061da8`. Le firmware d'origine
  l'appelle dans toutes ses boucles d'attente longues.
- Délai : `+0x04` (TORR, `0x03061db2`) ; initialisation en `0x03061dba`.
- À la mise en route, notre firmware devra le rafraîchir pendant les opérations longues
  (écriture du firmware, montage du disque), comme le firmware d'origine.

### 3.4 eMMC et carte SD
**Contrôleurs** : DesignWare MSHC (dw_mmc), sans DMA interne (TRM chap. 10 et 11).
- eMMC en `0x60030000`, carte SD en `0x60020000`, même pilote dans le firmware d'origine
  (`0x0305eb34`–`0x0305fc10`, la pile SD/MMC générique du SDK).
- Registres : CTRL `+0`, PWREN `+4`, CLKDIV `+8`, CLKENA `+0x10`, TMOUT `+0x14`, CTYPE `+0x18`,
  BLKSIZ `+0x1c`, BYTCNT `+0x20`, INTMASK `+0x24`, CMDARG `+0x28`, CMD `+0x2c`, RESP0 à 3
  `+0x30`…`+0x3c`, RINTSTS `+0x44`, STATUS `+0x48`, FIFOTH `+0x4c`, FIFO `+0x200`.
- Données transférées par le CPU via la FIFO (16 mots, d'après RECHO) ou par le DMAC1
  (requête 12 = SDMMC, 13 = EMMC).

**Initialisation de l'eMMC** : protocole JEDEC standard, vu dans `0x0305f9xx`–`0x0305fbxx`.
- Lecture de l'EXT_CSD. Si le type de carte le permet (bit 1), haute vitesse à **52 MHz**
  (`0xcb20` kHz), sinon 26 MHz (`0x6590`).
- Test de la largeur de bus par CMD19 / CMD14 (motifs `55 aa`), puis CMD6 avec l'argument
  `0x03b70000 | (largeur << 8)` : octet 183 BUS_WIDTH, 2 = 8 bits, 1 = 4 bits.

**Mesuré sur l'appareil** (journaux de RECHO) :
- eMMC : 15 269 888 secteurs, FIFOTH au reset `0x300f0010`.
- Volume interne : FAT32 dans la zone utilisateur (`0x81000`), secteur de démarrage à +64,
  clusters de 8 secteurs (4 Kio), 1 838 584 clusters, 2 FAT à +4040, données à +32768.
- Carte microSD : SDHC/SDXC sur **4 bits**, 244 016 Mio, partition à 65 536, clusters de
  256 Kio (exFAT). Initialisation et montage en 26 ms.

### 3.5 DAC : deux CS43131 en double mono (firmware d'origine)
**Accès** :
- `0x0306168c(reg24, valeur)` écrit **dans les deux circuits** : bus I2C0 à 400 kHz, adresses 8 bits
  `0x60` (« dac60_Left ») puis `0x66` (« dac66_Right »), c'est-à-dire `0x30` et `0x33` en 7 bits.
  Message de 5 octets : registre (3 octets, poids fort en premier), `0x01`, valeur.
- Chaque circuit a son compteur d'échecs (`0x0300411e` / `0x0300411f`), et le firmware cesse de lui
  écrire après 2 échecs.
- Lecture : `0x030617a8`.
- **Balance** : sur `PCM_VOL_B` / `PCM_VOL_A` (`0x090001` / `0x090002`), le circuit de gauche et
  celui de droite reçoivent chacun une correction tirée de l'octet `[0x0301d158 + 0x35d]` (12 =
  centre).
- Les registres `0x05000a` / `0x05000b` (`ASP_CH_x_SZ_EN`) reçoivent 7 pour le circuit de gauche et
  `0xf` pour celui de droite ; ils commandent sans doute l'activation et la position du canal dans
  la trame I2S.

Noms des registres d'après l'en-tête public du pilote Linux `cs43130.h` (noms seulement ; aucune
ligne recopiée).

**Séquence d'initialisation** (`0x03051cc0`, après la PLL `0x03051bd4`) :

| Étape | Registres |
|---|---|
| PLL (`0x03051bd4`) | `PLL_SET_2..8` = 0, 0, 0, `40`, `08`, `80`, `02`, puis `PLL_SET_9` (`0x040002`) = `02` |
| Quartz, mise sous tension | `CRYSTAL_SET` (`0x020052`) = `02` ; lecture de `INT_STATUS_1` ; `PWDN_CTL` (`0x020000`) = `e7`, puis `f6` |
| Port série | `SP_BITSIZE` (`0x01000c`) = `04` |
| ASP | `ASP_NUM` = `0001`, `ASP_DEN` = `0008`, `ASP_LRCK_HI_TIME` = `001f`, `ASP_LRCK_PERIOD` = `003f` (64 bits par trame), `ASP_CLOCK_CONF` = `0c`, `ASP_FRAME_CONF` = `0a` |
| Canaux | `ASP_CH_1/2_LOC` = 0, `ASP_CH_1/2_SZ_EN` = `07` |
| Filtre PCM | `PCM_FILT_OPT` (`0x090000`) = `02`, `42`, `82`, `c2`, puis `22` |
| Volume | `PCM_VOL_B` / `PCM_VOL_A` = 0, `PCM_PATH_CTL_1` = `ec`, `PCM_PATH_CTL_2` = 0 |
| Bloc non documenté | `0x18xxxx` : 4 groupes `2f b4 25 80 xx xx` (`0x180009`–`0x180040`), puis `0x180000` = `45` ; coefficients ou calibrage |
| Sortie casque | `CLASS_H_CTL` (`0x0b0000`) = `1f`, `HP_OUT_CTL_1` (`0x080000`) = `31`, `HP_DETECT` (`0x0d0000`) = `04`, `0x0d0001` = 0 |
| Interruptions | lecture de `INT_STATUS_1/2`, `INT_MASK_1` = `87`, `INT_MASK_2` = `07` |
| Fin | `SYS_CLK_CTL_1` (`0x010006`) = `04`, `PAD_INT_CFG` (`0x01000d`) = `02`, … |

**Mise sous tension des DAC et I2C** (RECHO `0x030562cc`, recoupé avec l'origine) :
1. GPIO2_0, GPIO2_14 et GPIO2_2 à 1 : alimentations, probablement.
2. 2 ms d'attente, puis GPIO2_1 à 0 pendant 1 ms, puis à 1 : vraisemblablement le reset des deux
   CS43131.
3. 8 ms d'attente, puis I2C0 (`0x40040000`, contrôleur I2C Rockchip du TRM chap. 22) :
   - horloge activée (porte `0x83`), impulsion de reset `0x1a` (100 µs) ;
   - routage du GRF (`+0xbc`) ;
   - **GPIO2_10 / GPIO2_11 en fonction 1** ;
   - 400 kHz.

   Le firmware d'origine (`0x03060c54`) connaît d'autres brochages (GPIO2_16/17 en fonction 2,
   GPIO0_5/6 en fonction 3, GPIO2_8/9) ; RECHO confirme celui de l'appareil.

**Recoupement avec RECHO 0.10.0** (`0x03056400`–`0x030566a0`) : écriture dans les deux circuits par
`0x03055c90(reg, val)`, qui appelle `0x03055c00(dac, reg, val)` pour dac 0 puis dac 1 ; identifiant
lu en `0x010000` (« dac id »).
- **Identique à l'origine** : `CLASS_H_CTL` = `1f`, `CRYSTAL_SET` = `02`, lecture de
  `INT_STATUS_1`, **`INT_MASK_1` (`0x0f0010`) = `e7`** (le registre « ? » de l'origine), `PWDN_CTL`
  = `f6`, `SP_BITSIZE` = `04`, toute la configuration ASP `0x040010`–`0x040019` (mêmes valeurs),
  `ASP_CH_LOC` = 0, `PCM_PATH_CTL_1/2` = `ec` / 0, `HP_DETECT` = `04` / 0, `INT_MASK_1/2` =
  `87` / `07`, `SYS_CLK_CTL_1` = `04`, `PAD_INT_CFG` = `02`.
- **Précisé par RECHO** : `0x010010` (DXD1) = `99` ouvre l'accès aux registres non documentés,
  avant `0x180000` = `45`, puis vaut de nouveau 0. Ensuite `0x080032` (DXD13) = `20`, 10 ms
  d'attente, puis 0. Les pilotes Linux de cette famille utilisent la même clé `0x99`.
- **Différences** :
  - `HP_OUT_CTL_1` = `10` chez RECHO, contre `31` à l'origine ;
  - un seul `PCM_FILT_OPT` = `02` chez RECHO, contre cinq écritures (`02` à `22`) à l'origine ;
  - ~~pas de bloc `0x18xxxx` chez RECHO~~ : **corrigé le 2026-10-18** (§15), RECHO écrit bien
    ce bloc (4 groupes `2f b4 25 80 xx xx`, avec ses propres valeurs pour les deux derniers
    octets), puis `0x180000` = `45`.
- Volume : réglé ailleurs chez RECHO (« ui: volume %d dB (pcm_vol %02x) », 1 dB par appui,
  plafond −10 dB).

Volume, mute et rampe, côté origine : `0x030619ee` et `0x03061b1c`.

**Échelle de `PCM_VOL`, confirmée par RECHO** (`0x0305a7e0`) : valeur = 2 × atténuation en dB
(pas de 0,5 dB, 0 = 0 dB) et `0xff` = muet (cas −128 dB). L'écriture dans les deux circuits se
fait par `0x030561bc`.

### 3.6 USB (contrôleur Synopsys DWC2, `0x60080000`)
**Mesuré par RECHO sur l'appareil** :
- `hwcfg2` = `0x0820cc42` (OTG, mode esclave sans DMA interne, PHY UTMI+), `hwcfg3` =
  `0x01340480` (FIFO de 308 mots, compteur de transfert limité ; RECHO plafonne à 1 536 octets),
  `hwcfg4` = `0x09e04061` (FIFO partagée), `usbcfg` = `0x1408` ;
- RX FIFO de 147 mots, `nptx` = `0x00800093` ;
- **énumération en haute vitesse** (« speed 0 »).

**Pilotes** :
- Côté origine : overlays 10 (stockage de masse), 11 (cœur) et 12 (contrôle, avec les commandes
  propres à Rockchip), plus le résident (`0x03065154`, `0x030680f8`, `0x03068304`, `0x030695cc`).
- Côté RECHO : un stockage de masse complet, avec journal des commandes SCSI et des temps eMMC et
  USB.
- Le DWC2 est une IP publique et bien documentée. Notre pilote sera écrit à partir du TRM
  (chap. 14) et de cette documentation, sans rien reprendre des deux firmwares.

**Pour l'image minimale, l'USB n'est pas indispensable** : le mode USB du chargeur (VOL− et USB
au démarrage, §2.2) permet d'écrire une image avec `rkdeveloptool wl` pendant le développement.
Le stockage de masse viendra ensuite.

## 4. Sortie audio sans égaliseur
- **I2S** : il accepte directement du PCM 16 bits stéréo entrelacé. Avec VDW = 16 bits et HWT = 0,
  chaque mot de la FIFO contient deux échantillons 16 bits (TRM chap. 13).
  - Les conversions 16 → 24 → 32 bits et le décalage du firmware d'origine (`0x03066600`–) ne
    servent qu'à l'égaliseur : on peut les supprimer.
  - Pour une source 24 bits, VDW = 24 bits, avec un mot de 32 bits par échantillon.
- **Qui alimente l'I2S ?**
  - Le DMAC2 (domaine HiFi) n'a que deux requêtes matérielles, celles du HIFIACC.
  - La requête « i2s1 tx » est la n° 2 du **DMAC1** (`0x60000000`, TRM tableau 2-2).
  - Le cœur B atteint le bus AHB en `0x6000_0000`, puisque le module MP3 écrit dans IMDCT36 et
    SYNTH, deux blocs voisins. Il pourrait donc programmer lui-même un canal du DMAC1. **À
    vérifier** : vers quel cœur part l'interruption du DMAC1, et comment partager ses canaux avec
    le cœur A (eMMC, SD et écran utilisent aussi le DMAC1).
  - **Solution prudente** : le cœur A garde le DMAC1, et une liste chaînée (LLP) lit directement
    les tampons PCM en RAM HiFi. On n'a alors ni copie, ni conversion, ni égaliseur. Cela supprime
    la copie DMA vers le cœur A et les tampons ping-pong `0x0303d47c` / `0x0303447c`.
- **Volume** : à faire dans le DAC, en I2C, plutôt qu'en logiciel. Le firmware d'origine fait déjà
  ainsi (`0x030619ee`).

**RECHO le fait déjà ainsi** (0.10.0, émulé et lu, 2026-10-18) :
- **Anneau PCM de 48 Kio en PMU-SRAM**, `0x03090000`–`0x0309c000`. Il est mis à zéro (`0x03056912`)
  avant la programmation de la DMA. Il se trouve juste sous la zone libre `0x0309c090` du §20 du
  rapport et sous `NanoCRebootFlag`, et les deux cœurs y ont accès.
- **Le module du cœur B (m78) référence `0x0309c000`**, la fin de l'anneau : il y écrit lui-même
  le PCM.
- **DMAC1, canal 5** (`0x030557dc`) :
  - DAR = `0x40030024` (I2S1 TXDR) ;
  - SAR dans l'anneau ;
  - LLP = une liste en SRAM (`0x0302cb90`) qui boucle sur l'anneau ;
  - `CTL` = `0x18909124` (32 bits en source et en destination, salves de 8, mémoire vers
    périphérique, LLP en source et en destination), `CTL_H` = `0x600` ;
  - `CFG` = `0x840`, `CFG_H` = `0x1007` (périphérique de destination 2 = i2s1 tx) ;
  - interruptions de bloc et de transfert démasquées pour le canal 5 ; `DmaCfgReg` = 1,
    `ChEnReg` = `0x2020`.
- Le cœur A ne fait que surveiller : messages « SAR outside the ring », « ring ur ».
- Le module B de RECHO utilise aussi **IMDCT36 (`0x60010000`) et SYNTH (`0x60050000`)** : son
  décodeur MP3, écrit par atzkey, se sert des accélérateurs, comme celui d'origine.

C'est l'architecture voulue : décodage dans le cœur B, écriture directe dans un anneau partagé,
DMA vers l'I2S, pas d'égaliseur, pas de copie par le cœur A. Elle est validée sur l'appareil par
RECHO.

### 4.1 Horloges audio et I2S1 (RECHO émulé, recoupé avec l'origine)
Outil : `tools/emu_mmio.py FW.IMG FONCTION [--args …] [--stub …] [--read …]`. Il exécute une
fonction de l'un ou l'autre firmware et journalise tous les accès aux périphériques. Une lecture
attendue sans fin est signalée, ce qui montre ce que le code attend.

**RECHO `0x0305598c(entrée)`, à 44,1 kHz** (table par fréquence en `0x0305fc88`, entrées de
16 octets : fréquence, 4 paramètres de PLL, fréquence de base, indice) :
1. Horloge GPIO1 (`CRU+0xa4`). iomux GRF `+0x20` : GPIO1_0 à 1_3 en fonction 1, ce sont les
   **broches de l'I2S1** (« i2s1 pad group A »). Routage GRF `+0xbc` = `0x20000000`.
2. Horloges de l'ACODEC (`CRU+0x80`, `CRU+0xa4`) et `CRU+0x10` = `0x200020`.
3. **PLL de l'ACODEC** (`0x40090200`) :
   - `+0x200` = 0, `+0x214` = 4, `+0x200` = `0x10` ;
   - `+0x204`/`208`/`20c`/`210` = `06`, `24`, `19`, `30` ;
   - `+0x214` = 4 puis 0, 5 ms d'attente, puis `+0x200` = `0x14`.
4. Horloge de l'I2S : `CRU+0x20` (source, diviseur), portes `CRU+0x84`, reset `CRU+0xc0`.
5. **I2S1** (`0x40030000`) :
   - XFER = 0, CLR = 1 (attente du retour à 0) ;
   - `TXCR` = `RXCR` = `0x401f` (mots de 32 bits) ;
   - **`CKR` = `0x33f3f`** : 64 SCLK par trame, MCLK = 4 × SCLK = 256 fs ;
   - `INTCR` = `0x1f00102`.

**Recoupement** :
- Le firmware d'origine (`0x03051b08(indice, famille)`) écrit la même suite de registres ACODEC
  (`+0x200`…`+0x214`) à partir de 3 tables (`0x0306f4c0` 48k, `0x0306f550` 44,1k, `0x0306f5e0`
  8k). Chaque entrée contient le quartz (24 000 kHz), la sortie de la PLL et 4 paramètres.
- **Les paramètres sont identiques à ceux de RECHO** :
  - famille 44,1 kHz : `03 12 19 03` (11,025 et 22,05), `06 24 19 03` (44,1 et 88,2 kHz, sortie
    56 448 kHz), `04 49 19 03` (176,4) ;
  - famille 48 kHz : `00 18 19 0a`, `00 30 19 0a`, `00 30 19 09` ;
  - famille 8 kHz : `00 30 32 08`, `00 60 32 08`.
- La trame I2S (64 bits, demi-trame de 32 bits) correspond à la configuration ASP des CS43131
  (§3.5 : `ASP_LRCK_PERIOD` = 63, `ASP_LRCK_HI_TIME` = 31).
- **Conséquence pour notre firmware** : I2S1 en mots de 32 bits. Pour éviter toute conversion,
  il faudrait passer l'I2S en 16 bits (VDW = 16, HWT = 0) et l'ASP des DAC en 16 bits, ou bien
  faire produire au cœur B des échantillons sur 32 bits. Le second choix est le plus sûr, car
  c'est la configuration déjà validée par les deux firmwares ; il ne coûte qu'un décalage dans le
  décodeur.

## 5. Codecs : recompiler ou garder ?
| Format | Module d'origine | Source ouverte possible | Choix proposé |
|---|---|---|---|
| Opus | (le nôtre, 74) | libopus, BSD-3 | le nôtre, plus la structure de retour (§6) |
| Vorbis | (le nôtre, 77) | Tremor, BSD-3 | idem |
| FLAC | 78, avec LPC du HIFIACC, bibliothèque Rockchip | libFLAC, BSD-3 ; RECHO utilise aussi le LPC matériel, avec repli logiciel | recompiler ; le LPC matériel en option, une fois ses registres compris à partir du module 78 |
| WAV, AIFF | 76 | trivial | réécrire |
| ALAC | 79 (LPC du HIFIACC) | décodeur d'Apple, Apache-2.0 | recompiler |
| APE | 80 (HIFIACC) | SDK Monkey's Audio, BSD-3 dans les versions récentes (**licence à vérifier**) | recompiler, ou garder 80 |
| AAC-LC | 75 (Helix sans SBR) | Helix AAC (RPSL/RCSL, pas BSD ; version durcie dans ESP8266Audio) | garder 75 au début |
| MP3 | 73 (libMad Rockchip, IMDCT36 + SYNTH) | libMad (GPL-2), Helix MP3 (RPSL), minimp3 (CC0, mais en flottant : lent sur Cortex-M3) | **garder 73** (31 MHz mesurés) |
| DSF / DFF | 81, 82 | DSD vers PCM : filtre simple ; le DST (DFF compressé) est plus lourd | garder au début |

Tous les codecs ne sont pas sous BSD : MP3 et AAC n'ont pas d'équivalent BSD rapide en virgule fixe.
Garder ces deux modules d'origine est la voie la plus courte.

## 6. Structure de retour d'information (proposition)
Un bloc d'état à adresse fixe en RAM HiFi, écrit par le cœur B et lu par le cœur A :
- `magic`, version ;
- type de codec ;
- cycles actifs et totaux par fenêtre d'une seconde d'audio : méthode de la sonde HIFIEC64, avec le
  SysTick `VAL` et `LOAD` au maximum ;
- pic d'activité ;
- fréquence mesurée du cœur ;
- compteurs d'erreurs (trames rejetées, resynchronisations, dernier code d'erreur) ;
- tampons manqués ;
- marque haute de la pile.

Usages côté A : ajuster la fréquence du cœur B (garder 1,5 fois le pic, comme RECHO qui ajuste son
diviseur selon le taux d'occupation) et afficher un écran de diagnostic. Pour les modules d'origine
gardés tels quels (73, 75), on y greffe la sonde comme dans HIFIEC64 (patch de leur runtime).

## 7. Ressources graphiques
- **Polices** : RECHO 0.10.0 embarque les deux polices bitmap d'origine, 12 et 16 px, 65 536
  glyphes (format au §2 du rapport), **identiques octet pour octet** (SHA-256 `39a8c796…` et
  `17fdfc59…`). Il y ajoute une troisième ressource, sans doute ses icônes (« EchoIcon »).
- Pour notre firmware : extraire ces polices de l'image de l'utilisateur au moment de la
  construction, comme les modules de codecs, et ne pas les publier. Les icônes seront les nôtres.

## 8. Image minimale (étape 3) : ce que l'on sait faire
**Format de l'image** :
- en-tête `RKnanoFW` (§2 du rapport), avec sa copie en fin d'image ;
- table des overlays où l'**entrée 0** porte notre code et nos données, et où la **table de
  vecteurs se trouve au début des données de l'entrée 0** (`0x03004000`), puisque c'est elle que
  le chargeur utilise ;
- entrées 1 et 2 vides ou valides, car le chargeur les charge aussi ;
- les polices extraites de l'image d'origine ;
- CRC Rockchip en fin de fichier.

**Séquence de démarrage** :
1. Au reset, avant toute autre chose : recopier les données et mettre le BSS à zéro, comme RECHO.
   Lire la touche VOL− au SARADC canal 1 (≤ `0x40`) ; si elle est appuyée, `PMU_SYS_REG3` =
   `0xffff18af`, puis reset : mode USB du chargeur (§2.2).
2. GPIO2_6 à 1 pour maintenir l'alimentation ; si Power (GPIO2_5) n'est pas tenue assez
   longtemps sans câble, GPIO2_6 à 0 (§3.2).
3. Horloges : PLL à 200 MHz avec le marqueur de repli (§3.0), puis le chien de garde (§3.3),
   rafraîchi dans les boucles longues.
4. Écran (§3.1) : reset GPIO2_15, lecture de l'identifiant, séquence A ou B, effacement, puis
   rétroéclairage PWM0 canal 0 à 10 kHz.
5. Affichage d'un écran de test ; touches lues en boucle (§3.2).
6. eMMC (§3.4) : lecture de l'IDB (P, R), puis montage FAT32 en lecture.
7. Mise à jour : si `HIFIECnn.IMG` est présent, contrôles (signature, copie de l'en-tête, CRC,
   **pas de date**), puis écriture de FW2, puis de FW1, avec relecture et l'en-tête en dernier
   (§2).

**Ce qui n'est pas nécessaire pour cette image** : le son, l'USB (l'image s'écrit avec
`rkdeveloptool wl` en mode USB du chargeur) et la carte SD.

**Étapes suivantes** :
- **son** : DAC (§3.5), horloges audio et I2S1 (§4.1), anneau PCM en PMU-SRAM et DMAC1 canal 5
  (§4), cœur B démarré avec un module de codec (§1.4) et le protocole du §1 ;
- **navigateur** trié, tags, interface ;
- **USB** en stockage de masse ;
- **ajustement dynamique** de la fréquence du cœur B (§1.4) à partir du bloc de retour (§6).

## 9. Architecture mémoire (état de la 0.60, 2026-10-24)
Mise à jour de la proposition de départ, validée au début de l'étape 3. Les modules de codec du firmware d'origine sont abandonnés au profit de nos codecs libres (WAV, dr_flac, pvmp3dec, Tremor, libopus) : les réservations prévues pour eux sont retirées du plan (anneau PCM en PMU-SRAM, anneau du DAC en HIGHRAM1 ou HIGHRAM0, bloc de paramètres de 0x90 octets du runtime d'origine).

**Cœur A (Cortex-M3 principal), SYSRAM et PMU-SRAM** (`fw/link.ld`, `build/fw.map`) :

| Adresses | Taille | Contenu | Marge |
|---|---|---|---|
| `0x03000000`–`0x03003fff` | 16 Kio | pile du cœur A (SP initial `0x03004000`, comme l'origine) | — |
| `0x03004000`–`0x03004127` | 296 o | `.data` : table des vecteurs en tête (le chargeur saute par elle, §2.2), puis variables initialisées | — |
| `0x03004128`–`0x0301db03` | 102,7 Kio | `.bss` | 9,2 Kio jusqu'à `0x03020000` |
| `0x03020000`–`0x0304ffff` | 192 Kio | anneau du DAC (`.dacring`, 1,1 s à 44,1 kHz / 16 bits), lu par le DMAC1 canal 5 vers l'I2S1 ; ni chargé ni effacé | plein |
| `0x03050000`–`0x0306237b` | 72,9 Kio | code et constantes (entrée 0 de la table des overlays, adresse imposée) | — |
| `0x0306237c`–`0x030624a3` | 296 o | image de `.data`, recopiée au démarrage | 54,8 Kio jusqu'à `0x03070000` |
| `0x03070000`–`0x03077aff` | 30,8 Kio | file de lecture (`.queuebuf` : 400 entrées, 20 Kio de noms, dossiers de `q_from_tree`) | 1,3 Kio sur 32 Kio |
| `0x03078000`–`0x0308f6af` | 93,7 Kio | liste du dossier affiché (`.dirbuf` : 1 000 entrées, 80 Kio de noms) | 2,3 Kio sur 96 Kio |
| `0x0308f6b0`–`0x0308ffff` | 2,3 Kio | libre | — |
| `0x03090000`–`0x0309bfff` | 48 Kio | PMU-SRAM libre (ancien emplacement prévu de l'anneau PCM) | 48 Kio |
| `0x0309c000`–`0x0309efff` | 12 Kio | anneau du journal (`log.c`), conservé à travers un reset | — |
| `0x0309ffe0` | 4 o | marque `FAST` (essai à 200 MHz, repli de la PLL) | — |

Le chargeur IDB tourne, pendant qu'il nous charge, depuis sa copie en `0x0304a000`–`0x0304dfff`, avec données et pile en `0x03088000`–`0x0308ffff` (§23.2) : l'anneau du DAC et la liste de dossier, posés sur lui, ne sont ni chargés ni effacés et ne servent qu'une fois le firmware lancé.

Plus gros blocs de `.bss` : `first` 17,2 Kio, tampon de lecture des tags (`rd`, `tags.c`) 16 Kio, cache de la police 14 Kio, `group` 10,2 Kio, `index_` 8,6 Kio, `buf.0` 8 Kio.

**Cœur B (HiFi), HIGHRAM** (`fw/hifi/link.ld`, `hifi_proto.h`) :

| Adresses (vue du cœur A) | Taille | Contenu |
|---|---|---|
| HIGHRAM0 `0x01000000`–`0x0101ffff` (vue en 0 par le cœur B) | 128 Kio | code du codec chargé : Opus 67,4 Kio, Vorbis 58,6, MP3 34,9, FLAC 32,8, WAV 2,8 |
| HIGHRAM1 `0x01020000`–`0x01020fff` | 4 Kio | bloc de contrôle `struct hp_ctl` (protocole entre les cœurs, état, mesures) |
| `0x01021000`–`0x01022fff` | 8 Kio | pile du cœur B (SP `0x01023000`) |
| `0x01023000`–`0x0104ffff` | 180 Kio | données et `.bss` du codec : Vorbis 114,6 Kio (dont 96 Kio de tas), Opus 72,8, MP3 40,7, FLAC 4,1, WAV 4 |
| `0x01050000`–`0x01057fff` | 32 Kio | anneau d'entrée : données compressées écrites par le cœur A |
| `0x01058000`–`0x0105ffff` | 32 Kio | anneau de sortie : PCM écrit par le cœur B (garde de 4 Kio) |

**Flux audio** :
1. Le cœur A lit le fichier (eMMC ou carte) dans l'anneau d'entrée en HIGHRAM1.
2. Le cœur B décode et écrit le PCM dans son anneau de sortie.
3. Le cœur A le recopie dans l'anneau du DAC en SYSRAM (192 Kio), qui absorbe les pointes de charge et les lectures lentes.
4. Le DMAC1, canal 5, lit l'anneau du DAC en boucle vers le TXDR de l'I2S1 ; le cœur A suit les positions, règle le volume des DAC (I2C, ReplayGain compris) et la fréquence du cœur B.

**eMMC** : IDB en `64 + 1024·i` (5 copies, jamais écrites) ; FW1 en `0x2000` ; FW2 en `0x2000 + R` (`R` = `0xd800` sur cet appareil) ; volume utilisateur FAT32 à `0x81000` (secteur de démarrage `0x81040`), qui contient `NEWFWLOG.TXT` (journal et état de reprise).

**Marges à surveiller** : la `.bss` (9 Kio ; la variante de test `SD_WALK` l'a dépassée), la file et la liste de dossier (1 à 2 Kio chacune). La PMU-SRAM basse (48 Kio) et l'espace après le code (55 Kio) sont les réserves disponibles.

## 10. Reste à faire dans l'étape 1
1. Chargeur de l'IDB (§2.1) :
   - **fait** : géométrie, chargement de l'entrée 0, passage de relais, deux modes USB accessibles
     par logiciel ;
   - **fait aussi (§2.2)** : binaire du chargeur obtenu par le BOOTDUMP de RECHO. Il démarre
     toujours FW1 s'il est valide, sans contrôle de date ;
   - VOL− + USB au démarrage **confirmé sur l'appareil** (§2.2) ;
   - ~~déclencheur USB du message `0xbf`~~ : trouvé. Overlay 12 (`0x0307c2f0`–`0x0307c37c`),
     gestionnaire des commandes SCSI propres à Rockchip qui s'identifie « RKNANOC » à l'outil de
     mise à jour. Le code de commande −2 appelle `0x0307bc6c` : avec une donnée (longueur non
     nulle), message `0xbf`, donc mode USB du chargeur ; sans donnée, `0xc0`, donc ROM.
     L'opcode SCSI exact n'est pas relevé. Avec le firmware d'origine, l'outil de FiiO peut donc
     faire passer le lecteur en mode chargeur depuis le PC.
2. ~~Démarrage du cœur B côté A~~ : fait (§1.4).
3. Pilotes, dans l'ordre de l'image minimale :
   - ~~écran~~ et ~~rétroéclairage~~ : faits (§3.1, §3.3) ;
   - ~~touches et SARADC~~ : fait (§3.2) ; maintien d'alimentation GPIO2_6 et Power GPIO2_5
     identifiés, d'autres broches restent à identifier ;
   - ~~eMMC et carte SD~~ : faits (§3.4) ;
   - ~~chien de garde~~ : fait (§3.3) ;
   - ~~DAC et I2C~~ : faits (§3.5, recoupés avec RECHO) ;
   - ~~batterie et charge~~ : seuils et broches (§3.2) ; ~~USB~~ : contrôleur identifié (§3.6), le
     pilote reste à écrire.
4. ~~Registres du LPC du HIFIACC~~ : faits et validés (§1.5 ; FLAC d'origine exact en émulation).
   Restent les modèles de l'ALAC et de l'APE, facultatifs.
5. ~~Alimentation de l'I2S~~ : résolu (§4). RECHO utilise le canal 5 du DMAC1 en anneau depuis la
   PMU-SRAM, alimentée directement par le cœur B. Reste à répartir les canaux du DMAC1 entre l'I2S
   (canal 5 chez RECHO), l'eMMC et, plus tard, l'écran. RECHO 0.10.0 pilote l'écran en GPIO, sans
   DMA.
6. ~~Pour démarrer l'étape 3~~ : architecture (§9) et plan (§8) validés par l'utilisateur le
   2026-10-18, avec l'option B pour les modules d'origine (patch de leur runtime pour écrire
   directement dans l'anneau PCM). Première image : §11.

## 11. Étape 3 : image 0.1 (2026-10-18)
Sources dans `newfw/fw/` (C, sans bibliothèque) : `make STOCK=…/HIFIEC39.IMG` produit
`build/HIFIEC70.IMG` (1 Mio). La police 16 px est extraite de l'image de l'utilisateur à la
construction (`tools/mkfont.py`) et n'est jamais versionnée.

**Contenu**
- Démarrage (`start.c`) : table de vecteurs au début de `.data` (`0x03004000`), données
  recopiées depuis leur copie dans le code, BSS mis à zéro, VTOR, interruptions laissées par le
  chargeur coupées. r0 à r3 et `PMU_SYS_REG3` relevés pour l'écran de test.
- Horloges (`board.c`) : séquence PLL de RECHO (§3.0), cœur à 200 MHz, avec le marqueur `FAST`
  (repli à 24 MHz si le démarrage précédent ne l'a pas effacé après 5 s). SysTick à 1 ms.
- Écran (`lcd.c`) : VOP en mode bypass (un octet par écriture, ordre des octets maîtrisé),
  temporisation selon la formule d'origine (`0x3083` à 200 MHz), lecture de l'identifiant en GPIO,
  variantes A et B, rétroéclairage GPIO2_4 à 1 (pleine luminosité, comme RECHO à 100 %).
- Touches, ADC, batterie (5 000 mV / 1 024 par pas, RECHO `0x03058350`), prises, VBUS.
- Power 2 s : extinction (l'appui qui a allumé l'appareil est ignoré jusqu'au relâchement).
  VOL− 3 s : `PMU_SYS_REG3` = `0x18af`, puis reset, donc mode USB du chargeur.
- eMMC (`emmc.c`) : contrôleur repris dans l'état laissé par le chargeur (RECHO fait de même :
  pas d'initialisation de carte dans son journal), transferts par la FIFO, CMD17/18/24/25 avec
  arrêt automatique. **Écriture refusée hors de `[0x2000, 0x2000 + P)`.**
- FAT32 en lecture (`fat.c`) : recherche du volume par pas de 1 Mio à partir de `0x2000 + P`
  (MBR ou secteur de démarrage), racine en noms 8.3, chaînes de clusters.
- Installateur (`update.c`, menu MENU) : liste des `*.IMG` de la racine ; contrôle de la signature,
  de la copie d'en-tête, de la taille (règle `fw_size` de RECHO, §2.1) et du CRC ; **aucun
  contrôle de date ni de version**. Écriture de FW2 si l'image tient dans une copie, en-tête de FW1
  effacé, FW1 à partir du 2ᵉ secteur, en-tête de FW1 en dernier, chaque bloc relu et comparé.
  Image trop grande (image d'origine de 32 Mio) : FW1 seule, comme RECHO. Refus si la batterie est
  sous 3 600 mV sans câble.

**Validé en émulation** (`tools/emu_fw.py`, chargement fait comme le chargeur réel) :
- séquence envoyée à la dalle identique à celle de l'origine (variante A), écran de test rendu ;
- VOL− 3 s : `0x18af` puis reset ; Power maintenue dès l'allumage : pas d'extinction ; marqueur
  `FAST` : 24 MHz ;
- disque virtuel : IDB réelle de `BOOTDUMP.BIN` (P = `0x1b000`, R = `0xd800` relus correctement),
  volume FAT32 à clusters de 4 Kio en `0x81000` + 64, comme sur l'appareil ;
- installation : notre image (FW2 puis FW1, identiques à l'image), RECHO 0.10.0 (idem), image
  d'origine (FW1 seule, identique), image au CRC faux (refusée, rien d'écrit). Aucune écriture hors
  de la zone firmware.

**Noms de fichiers** : RECHO installe au démarrage `ECHOMINI.IMG` ou le `HIFIECnn.IMG` de plus
grand numéro (nn ≤ 97, RECHO `0x03053e6c`). Les images de secours destinées à notre menu doivent
donc porter un autre nom (`RECHO010.IMG`, `STOCK39.IMG`), en 8.3.

**`rkdeveloptool` en mode USB du chargeur** : les fonctions de lecture et d'écriture du chargeur
(`0x0300241c`, `0x03002438`) ajoutent `0x2000` au LBA. LBA 0 = FW1, ce qui est cohérent avec la
procédure communautaire `wl 0 <image>`. À confirmer par une lecture (`rl 0 1`) avant toute
écriture.

**Reste à voir sur l'appareil** : ordre des couleurs (rouge, vert, bleu, blanc), lisibilité,
échelle des touches, écriture eMMC réelle (première installation de test avec une image qui tient
en deux copies, RECHO par exemple).

### 11.x Essais sur l'appareil, images 0.1 à 0.7 (2026-10-18)
| Image | Constat sur l'appareil | Correction |
|---|---|---|
| 0.1 | démarre ; écran, couleurs, IDB et volume corrects ; identifiant de dalle lu `ffffffff` (variante A retenue, correcte) ; touches lentes, VOL+/VOL−/Play ouvrent le menu | 0.2 : anti-rebond de 30 ms (la tension de l'échelle traverse la plage de MENU), lignes d'état redessinées seulement si elles changent |
| 0.2 | temps 16 fois trop lents | 0.3 : verrouillage de la PLL = bit 10 de `PLL_CON1` (et non 15) |
| 0.3 | PLL verrouillée (`9446`) mais toujours lent ; prise 3,5 mm détectée à tort | tirage interne de GPIO0_13 coupé (vérifié) |
| 0.4–0.5 | DWT et SysTick sur STCLK lisent 0 ; `MODE_CON` relu à `0000` : cœur sur le quartz / 2 = 12 MHz | 0.6 : attentes après `CLKSEL0`, `CLKSEL1` et `MODE_CON`, relecture ; fréquence déduite des registres relus |
| 0.6 | 200 MHz (`MODE_CON` `0001`) ; l'ADC rend parfois le canal précédent (batterie 4995 mV) | 0.7 : attendre le début puis la fin de la conversion |
| 0.7 | 30 s de `up` = 30 s ; batterie stable (4116–4121 mV) ; `adc2`/`adc3` stables | — |
| 0.7 | touches : VOL− 1, VOL+ 123, Play 244–245, Back 490–491, MENU reconnue | — |
| 0.7→0.8 | entrée en mode USB par VOL− 3 s : le chargeur répond mal (PLL et attentes mémoire laissées à 200 MHz) | 0.8 : retour au quartz (valeurs de RECHO `0x030516c4`) avant tout redémarrage |
| 0.8 | installée par RECHO ; **installateur validé sur l'appareil** : `RECHO010.IMG` (5 Mio) vérifiée (FAT32, CRC), écrite dans FW2 puis FW1 avec relecture, redémarrage sous RECHO | — |

Écriture : par le mode USB du chargeur avec `re/newfw/flash/flash.sh` (local, non publié). Le Rockusb du chargeur se bloque après une erreur de transfert tant que son point d'accès IN n'est pas vidé ; `rkdiag.py` (lecture seule) le débloque avant chaque étape.

## 12. Étape 4 : USB en stockage de masse, image 0.9 (2026-10-18)
- `usb.c` : DWC2 en mode esclave, interrogé (sans interruptions), FIFO d'émission partagée ;
  point d'accès 0 (contrôle) et 1 (bulk IN/OUT, 512 octets en haute vitesse, transferts de
  3 paquets au plus, champ de taille sur 11 bits). Mise sous tension du PHY : valeurs relevées
  en émulant RECHO `0x03054fb8` (portes 7 et 6·16+10, resets 0 à 2, GRF `+0x80`–`+0xb8`,
  GPIO1_4 = 0, GPIO2_0 = GPIO2_2 = 1), puis reset logiciel du cœur.
- `msc.c` : transport bulk only + SCSI (INQUIRY, TEST UNIT READY, REQUEST SENSE, READ
  CAPACITY, READ FORMAT CAPACITIES, MODE SENSE 6/10, READ 10, START STOP, PREVENT ALLOW,
  VERIFY, SYNCHRONIZE CACHE). **Lecture seule** : support annoncé protégé, écritures refusées
  (DATA PROTECT), leurs données lues et jetées.
- Disque exposé : `[0x81000, fin de l'eMMC)`, taille par EXT_CSD (CMD8) : `0xe0f000` secteurs,
  comme le journal de RECHO. Identifiants de test pid.codes `1209:0001`.
- **Sur l'appareil** : énumération en haute vitesse du premier coup, « Write Protect is on »,
  montage automatique en lecture seule ; SHA-256 de `RECHO010.IMG` (5 Mio) et `STOCK39.IMG`
  (32 Mio) lus par notre pilote identiques aux originaux (37 Mio en 3,9 s, ≈ 9,5 Mo/s).

### 12.1 Lecture-écriture et état « non démonté », image 0.10
- `emmc_write_user()` : seconde clôture d'écriture, limitée à la zone déclarée par
  `emmc_user_area()`, qui doit commencer après la zone firmware (`FW1 + P`). L'IDB et les
  firmwares restent inaccessibles ; l'installateur garde sa propre clôture (`emmc_write_fw`).
- `msc.c` : WRITE (10) en mode lecture-écriture (VOL+ sur l'écran principal ; PLAY reste en
  lecture seule). Chaque bloc est sur l'eMMC avant le CSW (pas de cache). Bornes et cohérence
  longueur/secteurs vérifiées ; sinon refus et données jetées.
- Test hors appareil : `tools/msc_test.c` (faux USB jouant l'hôte, faux eMMC avec les mêmes
  règles de clôture) : lecture seule, refus d'écriture, écriture de 40 secteurs à cheval sur deux
  blocs, relecture, premier et dernier secteur, dépassement, longueur incohérente, éjection ; le
  disque entier est comparé à l'attendu.
- **« Volume was not properly unmounted »** : bit 0 de l'octet d'état `+0x41` du secteur de
  démarrage FAT32, que Linux pose au montage en écriture et n'efface au démontage que s'il
  l'avait trouvé propre ; il reste donc posé jusqu'à `fsck.fat`. Windows utilise le bit 27
  (arrêt propre) de l'entrée FAT 1, et le bit 26 pour les erreurs. L'écran principal affiche
  `clean` ou `dirty N` (1 : secteur de démarrage, 2 : FAT[1], 4 : erreur), relu après chaque
  session USB. Procédure avant `fatsort` : mode lecture-écriture, volume **non monté**, puis
  `fsck.fat -a /dev/sdX1` (efface la marque), puis `fatsort /dev/sdX1`. Notre firmware ne
  modifie jamais le système de fichiers lui-même.

### 12.2 Essais sur l'appareil, 0.10 et 0.11 (2026-10-18)
- 0.10, mode VOL+ : 50 Mio de fichiers aléatoires écrits (2,9 Mo/s), relus identiques en mode
  PLAY après démontage ; `RECHO010.IMG` et `STOCK39.IMG` inchangés ; `fsck.fat -a` : marque
  « dirty » effacée, « FATs differ but appear to be intact » (première FAT recopiée ; écart
  antérieur à l'essai, probablement laissé par RECHO ou l'origine) ; écran ensuite `clean`.
- 0.11 installée par notre installateur (`NEWFW011.IMG`, 2ᵉ installation réelle) ; écritures
  eMMC par séries de 128 secteurs et copie USB par mots : 50 Mio en 7,5 s (≈ 7 Mo/s), relus
  identiques après remontage. Sortie du mode USB après 5 s de bus suspendu
  (`udisksctl power-off`).

## 13. Étape 5 : le son — état des connaissances (2026-10-18)
Sources : §1 (protocole des modules, émulé), §1.4 (démarrage du cœur B), §3.5 (DAC), §4 et §4.1
(I2S1, DMA, anneau), le TRM, et RECHO 0.10.0 : ses chaînes de journal (qui décrivent son
fonctionnement) et quelques fonctions émulées. Rien n'est recopié de RECHO : on en retient le
comportement et les valeurs de registres.

### 13.1 Ce qui est établi
| Maillon | État | Où |
|---|---|---|
| Alimentation et reset des DAC | GPIO2_0, 2_14, 2_2 à 1 ; impulsion GPIO2_1 ; délais | §3.5 (RECHO et origine concordent) |
| I2C0 vers les deux CS43131 | GPIO2_10/11 fonction 1, 400 kHz, adresses `0x30` et `0x33`, trames de 5 octets | §3.5 |
| Initialisation des CS43131 | séquence complète de l'origine et variante plus courte de RECHO (sans bloc `0x18xxxx`) | §3.5 |
| Volume | `PCM_VOL_A/B` = 2 × atténuation en dB, `0xff` muet ; dans les deux DAC | §3.5 |
| PLL de l'ACODEC (horloges audio) | tables 44,1 / 48 / 8 kHz, **identiques chez RECHO et à l'origine** | §4.1 |
| I2S1 | broches GPIO1_0–1_3, mots de 32 bits, 64 SCLK par trame, MCLK = 256 fs | §4.1 |
| DMA vers l'I2S | DMAC1 canal 5, requête 2 (i2s1 tx), liste chaînée bouclant sur un anneau de 48 Kio en PMU-SRAM | §4 (valeurs de RECHO) |
| Démarrage du cœur B | domaine PD_HIGH, horloges, reset, chargement en HIGHRAM, bloc de paramètres | §1.4 (origine et RECHO) |
| Protocole des modules d'origine | boîte aux lettres, OPEN/DECODE/SEEK/CLOSE, requêtes de lecture, structure info | §1.1–1.3 (émulé ; 74, 75, 76, 77, 78 validés) |

Rien de tout cela n'a encore tourné sur l'appareil avec notre code. Tout a été soit relevé dans
un firmware qui fonctionne (origine, RECHO), soit validé en émulation.

### 13.2 Ce que fait RECHO de plus (d'après ses journaux et son code)
**Organisation** :
- Un **seul programme pour le cœur B** (FLAC, WAV, MP3 ; 41 Ko en HIGHRAM0), avec son propre
  protocole A↔B dans un bloc partagé en `0x01020000` (pas le protocole des modules d'origine).
  Le cœur B écrit le PCM directement dans l'anneau de la PMU-SRAM ; il renvoie aussi ses propres
  lignes de journal au cœur A (« B: %u log lines dropped »).
- Le cœur A sert les fichiers par un anneau d'entrée (« in min », « starved »), surveille la DMA
  (« SAR outside the ring ») et ne touche jamais au PCM.

**Lecture gapless** :
- Le cœur A prépare une **file de lecture** (dossier ou arborescence) et annonce la piste suivante
  **avant la fin** de la courante : « player: next in the stream: track %u at %u, %u bytes ».
- Le cœur B passe d'une piste à l'autre **sans vider l'anneau** : « B: boundary to track %u: %s
  %u Hz, %u frames queued ». La DMA ne s'arrête pas.
- **Changement de fréquence d'échantillonnage** : impossible sans coupure. RECHO laisse l'anneau se
  vider (« rate change: draining », « B: drained for %u Hz »), reprogramme les horloges audio
  (« player: B drained, audio to %u Hz »), puis repart.
- **Retard et remplissage des encodeurs** : RECHO lit les en-têtes `Xing` / `Info` / `LAME` des
  MP3 (chaînes présentes), ce qui permet de retirer le silence ajouté au début et à la fin par
  l'encodeur. C'est indispensable pour un vrai gapless en MP3. Rien de tel dans le firmware
  d'origine.
- L'interface l'affiche (« now playing … (gapless) ») et journalise le moment où la piste suivante
  devient audible (« now hearing track %u »).

**Gestion de la DMA et de la charge** :
- Anneau de 48 Kio en PMU-SRAM, liste chaînée bouclée : la DMA tourne sans interruption du cœur A.
- Compteurs : sous-alimentation de la FIFO I2S (« i2s ur », « fifo min ») et de l'anneau
  (« ring ur »), marges minimales des anneaux d'entrée et de sortie, temps de décodage, pile libre
  du cœur B.
- **Fréquence du cœur B ajustée** à chaque piste et selon la charge (« B /%u -> /%u (%u MHz; busy
  %u%%, out min %u) ») ; cœur B éteint après un temps de pause (« stopped for %u s: B off »).
- La DMA sert aussi à l'écran (« lcd: push adma », bandes envoyées par DMA avec repli en PIO).

### 13.3 Conséquences pour notre firmware
1. **Gapless = flux continu dans le cœur B.** Il faut que le décodeur enchaîne les pistes sans
   arrêter l'anneau ni la DMA, et que le cœur A annonce la piste suivante à l'avance. Avec les
   modules d'origine (73 MP3, 75 AAC), qui fonctionnent piste par piste (OPEN … CLOSE), deux voies :
   - **(a)** fermer et rouvrir le même module pendant que l'anneau continue de jouer. La marge est
     la durée de l'anneau : 48 Kio en 32 bits stéréo à 44,1 kHz = **139 ms**. C'est court pour une
     ouverture (lecture du début du fichier, première trame) ; à mesurer ;
   - **(b)** patcher leur runtime (option B, déjà retenue) pour qu'il écrive dans l'anneau et
     enchaîne la piste suivante sans s'arrêter. Plus de travail, mais sans trou possible.
   Pour nos propres décodeurs (Opus, Vorbis, FLAC, WAV), le flux continu se conçoit dès le départ.
2. **Agrandir l'anneau** donne de la marge à (a) et à tout le reste. D'après la carte d'adresses
   du TRM (Fig. 2-1), la HIGHRAM1 (256 Ko) est visible à la même adresse par les trois maîtres,
   donc a priori par le DMAC1. Un anneau de 128 Ko y durerait 370 ms. **À vérifier sur l'appareil**
   (RECHO a choisi la PMU-SRAM, peut-être pour une raison qu'on ignore : arbitrage, consommation).
3. **Retrait du retard d'encodeur** (vrai gapless) : MP3 (`Xing`/`Info`/`LAME`), AAC (`iTunSMPB`
   ou `edts`), Opus (`pre-skip` et position finale de l'Ogg), Vorbis (position finale de l'Ogg).
   FLAC et WAV n'en ont pas besoin. Le cœur A peut lire ces valeurs et dire au cœur B combien
   d'échantillons sauter au début et garder à la fin ; pour 73 et 75, c'est au runtime patché de
   l'appliquer.
4. **Changement de fréquence** : vider puis reprogrammer, comme RECHO. Les tables de PLL sont
   connues (§4.1).
5. **Surveillance** : reprendre l'idée des compteurs (I2S, anneau, marges, charge du cœur B) dans
   le bloc de retour du §6, et l'ajustement de fréquence du cœur B.

**Périmètre du gapless (décision de l'utilisateur, 2026-10-18)** : uniquement entre pistes
consécutives **du même format et de la même fréquence d'échantillonnage**, typiquement un album
encodé d'un seul tenant. Les listes de lecture et les collections mêlant les formats sont hors
périmètre : un changement de format impose de recharger le module du cœur B, donc un arrêt, et
une transition ordinaire (anneau vidé, éventuellement horloges reprogrammées) suffit. Le module
reste chargé d'une piste à l'autre ; seuls le flux continu, l'annonce de la piste suivante et le
retrait du retard d'encodeur sont nécessaires.

### 13.4 Questions à trancher sur l'appareil
1. Le DMAC1 lit-il la HIGHRAM1 (anneau plus grand) ? Vers quel cœur va son interruption ?
2. Notre initialisation des DAC (version RECHO, plus courte) donne-t-elle un son correct ? Le bloc
   `0x18xxxx` de l'origine change-t-il quelque chose à l'écoute ?
3. Durée réelle d'un CLOSE + OPEN du module 73 entre deux pistes (voie (a)).
4. Les accélérateurs IMDCT36 et SYNTH du module 73 : leur fonctionnement n'est pas modélisé, donc
   le module MP3 ne se teste que sur l'appareil.

### 13.5 Découpage proposé
| Sous-étape | Contenu | Test |
|---|---|---|
| 5a | DAC (alimentation, I2C, initialisation), PLL de l'ACODEC, I2S1, DMAC1 canal 5 et anneau ; **sinusoïde générée par le cœur A**, sans cœur B | une note à 1 kHz au casque ; compteurs de l'I2S et de l'anneau |
| 5b | Volume, mute, rampe ; changement de fréquence (44,1 / 48 kHz) ; essai de l'anneau en HIGHRAM1 | à l'écoute ; mesure des sous-alimentations |
| 5c | Cœur B démarré avec le module WAV d'origine (76), protocole du §1 ; lecture d'un WAV depuis le volume | WAV joué, comparé au fichier |
| 5d | Module MP3 (73) puis AAC (75) avec le runtime patché (option B) écrivant dans l'anneau | écoute ; charge mesurée |
| 5e | Gapless : file de lecture, piste suivante annoncée, retrait du retard d'encodeur | album gapless (Daft Punk *Discovery*) sans clic aux transitions |
| 5f | Nos décodeurs (Opus, Vorbis, FLAC) en flux continu | écoute ; comparaison au bit près en émulation |

5a est la base de tout et ne dépend d'aucun module : c'est par là qu'il faut commencer.

## 14. Journal du firmware, image 0.12 (2026-10-18)
- **Anneau en PMU-SRAM** (`0x0309c000`, 12 Kio, conservé au reset) : chaque ligne
  `[ssss.mmm] texte` y est d'abord écrite. Après une faute ou un redémarrage, les lignes pas
  encore copiées sont retrouvées au démarrage suivant (« recovered »).
- **Fichier `NEWFWLOG.TXT`** à la racine, créé une fois sur le PC (contigu, 8 Kio à 16 Mio) et
  seulement réécrit sur place : ni cluster alloué, ni FAT, ni répertoire modifiés. Troisième
  clôture d'écriture, `emmc_write_log()`, limitée aux secteurs de ce fichier (et après la zone
  firmware). Secteur 0 : en-tête texte (`next`, `fill`, `wraps`) ; secteurs suivants : texte
  circulaire. `tools/readlog.py` le remet dans l'ordre.
- Copie dans le fichier toutes les 5 s, ou dès 2 Kio en attente, et avant chaque action
  sensible (installation, extinction, redémarrage vers le chargeur). **Aucune écriture pendant
  le mode USB** (le fichier est fermé, puis recherché à nouveau à la sortie, le PC ayant pu le
  modifier).
- Contenu actuel : démarrage (version, étiquette, horloges), eMMC et IDB, volume (géométrie,
  état propre ou non), état du journal, batterie (au démarrage puis chaque minute), sessions USB
  (configuration, vitesse, sortie et raison, compteurs), installations (contrôle, écriture,
  durée, résultat), extinction, fautes (pc, lr, cfsr, hfsr, bfar).
- L'écran principal indique `log on` ou `log off N` (1 : pas de fichier, 2 : fragmenté,
  3 : taille, 4 : hors volume, 5 : lecture, 6 : écriture).
- Tests : `tools/log_test.c` (sur le PC : remplissage, bouclage, reprise après redémarrage, aucune
  écriture hors du fichier) ; émulateur avec un disque contenant le fichier : seuls ses secteurs
  sont écrits, journal relu par `readlog.py`.

## 15. Étape 5a : DAC, I2S, DMA et sinusoïde, image 0.13 (2026-10-18)
**Séquence relevée chez RECHO** (`0x030562cc(44100)` émulé avec `tools/emu_mmio.py`, retours
d'I2C simulés ; trace décodée en messages I2C, valeurs seulement) :
1. GPIO2_0, 2_14, 2_2 à 1 ; 2 ms ; GPIO2_1 : 0 (1 ms), 1 (8 ms), 0 ; 2 ms.
2. I2C0 : porte `0x83`, reset `0x1a` (100 µs), GRF `+0xbc` = `0x30000`, GPIO2_10/11 fonction 1,
   `CLKDIV` = `0xe000e`. Messages : `[adresse, reg23:16, reg15:8, reg7:0, 0x01, valeur]` ; une
   lecture envoie les cinq premiers octets puis lit un octet à l'adresse | 1.
3. CS43131 (les deux, sauf `ASP_CH_SZ_EN` : 7 pour `0x60`, `0xf` pour `0x66`) : lecture des
   identifiants `0x010000`–`0x010005`, `CLASS_H` `1f`, 80 ms, **GPIO1_4 à 1**, `CRYSTAL_SET`,
   masques, `PWDN_CTL` `f6`, `SP_BITSIZE` 4, ASP (64 bits par trame), filtre, volumes `ff`,
   `PCM_PATH`, **bloc `0x18xxxx`** (24 octets), `CLASS_H` `1f`, 80 ms, `HP_OUT_CTL_1` `10`,
   `HP_DETECT`, masques `87`/`07`, `SYS_CLK_CTL_1`, `PAD_INT_CFG`, clé `0x99` puis `0x180000` =
   `45`, mise sous tension par effacement de bits de `PWDN_CTL` (`0x40`, puis `0x10` avec DXD13 =
   `20` pendant 10 ms), `0x01000b` = 1.
4. Horloges audio, PLL de l'ACODEC et I2S1 comme au §4.1 ; `MODE_CON` bit 5 (ACODEC sur sa PLL).
5. DMAC1 : `CLKGATE7` = `0xffff0000`, reset logiciel 1·16+0, canal 5 comme au §4, liste
   chaînée de 8 blocs de 6 Kio en boucle ; I2S `DMACR` = `0x110`, 80 ms, `XFER` = 1, 20 ms.
6. GPIO2_0 à 0 une fois la sortie lancée ; volume en rampe de 3 dB toutes les 2 ms depuis `ff`.

**Notre code** (`src/i2c.c`, `src/audio.c`, écrits à partir du TRM et de ces valeurs) :
- En émulation (modèle minimal de l'I2C ajouté à `tools/emu_fw.py`), **les messages I2C
  envoyés aux DAC sont identiques octet pour octet à ceux de RECHO**, à l'ordre de deux
  lectures d'état près, plus une lecture de diagnostic ; les écritures CRU, GRF, ACODEC, I2S et
  DMAC1 sont les mêmes, à l'adresse de la liste chaînée près.
- Écran de test (BACK depuis l'écran principal) : sinusoïde de 997,6 Hz (139 périodes dans les
  6144 trames de l'anneau, donc sans raccord audible), −20 dBFS, écrite par le cœur A dans
  l'anneau ; PLAY l'allume ou l'éteint, VOL+/VOL− par pas de 2 dB à partir de −40 dB (plafond
  −10 dB comme RECHO) ; affichage des identifiants des DAC, des erreurs I2C, de la position de
  la DMA et du niveau de la FIFO ; tout est journalisé.

**Sur l'appareil (0.13, 2026-10-18)** : **la note est audible**, volume réglable. Journal :
identifiants des deux DAC `43 13 10 00 a1 06` (CS43131), **0 erreur I2C**, `PWDN_CTL` final
`a6` (`f6` moins les bits `0x40` et `0x10`), PLL de l'ACODEC et I2S comme prévu, FIFO de l'I2S
entre 17 et 24 mots. La DMA avance de `0x4440` octets en 2 s modulo l'anneau, soit
705 600 octets : **exactement 44 100 trames par seconde**. 0.14 : volume réglable aussi note
coupée, affichage du volume sur la ligne d'état.

### 15.1 Anneau plus grand en HIGHRAM1 : essai, image 0.15
- Mise sous tension de PD_HIGH avec les valeurs de RECHO (`0x03064c90` émulé) : `PMU+0x0c` = 0,
  portes 9·16+2, `CLKGATE4` = `0x170f0000`, `CLKGATE3` = `0x01ff0000`, cœur B maintenu en reset
  (`SOFTRST3` bit 2), resets 1·16+14, 3·16+6, 2·16+0..2, 0·16+14..15 relâchés, portes 0·16+2 et
  6·16+14. Puis motif écrit et relu par le cœur A dans `0x01040000`–`0x0105ffff`.
- Anneau de 128 Kio en `0x01040000` (moitié haute de la HIGHRAM1), 16 blocs de 8 Kio
  (`BLOCK_TS` = `0x800`), soit **371 ms à 44,1 kHz** (et non 743 ms, erreur corrigée) au lieu de 139 ms ; note de 1001,4 Hz
  (372 périodes dans 16 384 trames). MENU bascule entre les deux anneaux dans l'écran de test.
- À vérifier sur l'appareil : la DMA avance-t-elle (position et `RawErr` du DMAC1 journalisés),
  la note est-elle propre ?
- **Sur l'appareil (0.15, 2026-10-18) : le DMAC1 lit la HIGHRAM1.** Sur l'anneau de 128 Kio, la
  DMA avance de `0xc440` octets en 2 s modulo 128 Kio (705 600 octets : 44 100 trames/s), aucune
  erreur (`RawErr` 0), FIFO de l'I2S entre 18 et 24 mots, note propre à l'écoute ; bascules
  répétées entre les deux anneaux sans incident. **L'anneau de 128 Kio en HIGHRAM1 est adopté**
  (371 ms de marge ; décision remplacée par celle du §16). Conséquence pour le plan mémoire (§9) : HIGHRAM1 basse (`0x01020000`–
  `0x0103ffff`, 128 Kio) pour le bloc de paramètres, le bloc d'état et les données du cœur B ;
  haute (`0x01040000`–`0x0105ffff`) pour l'anneau PCM, que le cœur B remplit dans sa propre RAM.
  La PMU-SRAM reste libre pour le journal et les marqueurs. PD_HIGH doit rester alimenté.

### 15.2 Place pour les codecs : budget de la HIGHRAM (2026-10-18)
Fin de la RAM utilisée par chaque module d'origine (BSS remis à zéro par son runtime, lu dans la
constante « début − 1, taille » de son code) :

| Module | Code (HIGHRAM0) | Données | Fin de la RAM utilisée |
|---|---|---|---|
| 73 MP3 | 35 Ko | 49 Ko | `0x01049730` |
| 74 WMA (notre Opus y est logé) | 88 Ko | 25 Ko | `0x01047632` |
| 75 AAC | 72 Ko | 9 Ko | `0x01043658` |
| 76 WAV | 34 Ko | 19 Ko | `0x0105f2bc` |
| 77 OGG (notre Vorbis y est logé) | 61 Ko | 52 Ko | `0x0105e9d4` |
| 79 ALAC / 80 APE | 26 / 22 Ko | 8 / 9 Ko | `0x010559e0` / `0x01059f84` |
| 81 DSF / 82 DFF | 24 / 23 Ko | 8 Ko | `0x01049e28` / `0x01049970` |

Une grande partie de ces BSS est le tampon de fichier du runtime d'origine (88 Ko) et ses deux
tampons PCM, qui deviennent inutiles quand le cœur B écrit directement dans l'anneau.

**Conséquences** :
- Un anneau de 128 Ko en `0x01040000` chevauche les modules 73 et 75 tels quels. **Un anneau de
  64 Ko en `0x01050000`–`0x0105ffff` (186 ms à 44,1 kHz) ne chevauche ni 73, ni 75, ni 81/82**, sans
  aucun patch.
- **Nos modules recompilés** (Vorbis, Opus, FLAC, WAV), avec notre propre runtime et notre
  propre édition de liens, n'ont plus ce tampon de 88 Ko : code et tables en HIGHRAM0 (Vorbis
  pleine précision : 24 Ko de code + 44 Ko de tables, sur 128 Ko), données en HIGHRAM1 (Tremor :
  pic d'environ 55 à 65 Ko, commentaires non stockés ; plus un tampon d'entrée de 16 à 32 Ko).
  Ils tiennent largement sous `0x01050000` (192 Ko de données).
- **Proposition** : l'emplacement et la taille de l'anneau deviennent une propriété du module
  chargé : 64 Ko en haut de la HIGHRAM1 par défaut (tous les modules utiles), 128 Ko pour un
  module qui déclare tenir sous `0x01040000`, la PMU-SRAM (48 Ko) en dernier recours (76, 77,
  79, 80 d'origine, si on les garde).

## 16. Architecture mémoire : réflexion avant décision (2026-10-18)
### 16.1 Les mémoires et leurs propriétés
| Mémoire | Taille | Domaine | Survit à | Qui y accède | Remarques |
|---|---|---|---|---|---|
| SYSRAM (IRAM + DRAM du cœur A) `0x03000000`–`0x0308ffff` | 320 + 256 Ko | PD_LOGIC | rien (perdue en veille profonde) | cœur A, DMAC1 | notre code, données, pile |
| PMU-SRAM `0x03090000`–`0x0309ffff` | 64 Ko | **PD_PMU (toujours alimenté)** | **reset et veille profonde** (VD_LOGIC coupé) | cœur A, cœur B, DMAC1 (vérifié : RECHO y met son anneau) | peut être placée à l'adresse 0 (`noc_remap`, GRF) pour redémarrer sur du code qu'elle contient ; réveil par GPIO2 (Power = GPIO2_5) |
| HIGHRAM0 `0x01000000`–`0x0101ffff` | 128 Ko | PD_HIGH | rien (coupée avec PD_HIGH) | cœur B (vue en 0), cœur A, DMAC1 a priori (non testé) | code du module ; 2 bancs de 64 Ko |
| HIGHRAM1 `0x01020000`–`0x0105ffff` | 256 Ko | PD_HIGH | rien | cœur B, cœur A, **DMAC1 (vérifié, 0.15)** | données du module ; 4 bancs de 64 Ko à porte d'horloge séparée |

Deux conséquences générales :
- Ce qui doit survivre à une mise en veille, à un plantage ou à un redémarrage ne peut aller
  qu'en PMU-SRAM. C'est sa vraie valeur, et elle est unique.
- PD_HIGH (HIGHRAM, cœur B) peut être coupé hors lecture pour économiser l'énergie : tout ce
  qui y vit (codec, anneaux) est transitoire par nature, ce qui convient.

### 16.2 Besoins, par composant
**Cœur B, par module** (code en HIGHRAM0, données en HIGHRAM1) :

| Module | Code + tables | Données | Source de l'estimation |
|---|---|---|---|
| MP3 d'origine (73) | 35 Ko | jusqu'à `0x01049730` (166 Ko, dont son tampon de fichier interne) | §15.2, disposition figée |
| AAC d'origine (75) | 72 Ko | jusqu'à `0x01043658` (142 Ko) | §15.2 |
| **Opus** (notre libopus, recompilé avec notre runtime) | 54 Ko de code + 21 Ko de constantes ≈ 75 Ko | état 27 Ko, pseudo-pile 16 Ko, paquet 8 Ko, sortie d'un paquet de 120 ms au plus (23 Ko en 16 bits) : **≈ 75–80 Ko** | §10 du rapport (build du 74) ; les deux tampons PCM de 30 Ko du build actuel disparaissent si le cœur B écrit dans l'anneau |
| **Vorbis** pleine précision (notre Tremor) | 24 Ko de code + 44 Ko de tables ≈ 68 Ko | pic 55–65 Ko (commentaires non stockés) + sortie ≈ **70–80 Ko** | §27–28 du rapport |
| FLAC (à recompiler) | ≈ 25–40 Ko | blocs de 4096 échantillons : résidus et sortie ≈ 64–70 Ko ; en 192 kHz, blocs de 8192 : ≈ 130 Ko | estimation |
| WAV (à réécrire) | quelques Ko | quelques Ko | — |

Plus, côté cœur B : bloc de paramètres et bloc d'état (§6) ≈ 4 Ko, pile 8 Ko.

**Anneau d'entrée** (données compressées que le cœur A dépose pour le cœur B, si l'on n'utilise
pas le protocole d'origine où le module tire lui-même ses lectures) : 32 Ko = 2,6 s de MP3 à
96 kb/s, 0,3 s de FLAC 44,1/16, 27 ms de FLAC 192/24. Plus il est grand, plus le cœur A peut
faire autre chose (pochette, écran) sans affamer le cœur B.

**Anneau de sortie** (PCM 32 bits stéréo) : sa durée dépend de la fréquence :

| Taille | 44,1 kHz | 48 kHz | 96 kHz | 192 kHz |
|---|---|---|---|---|
| 48 Ko | 139 ms | 128 ms | 64 ms | 32 ms |
| 64 Ko | 186 ms | 171 ms | 85 ms | 43 ms |
| 96 Ko | 279 ms | 256 ms | 128 ms | 64 ms |
| 128 Ko | 371 ms | 341 ms | 171 ms | 85 ms |

(**Corrigé le 2026-10-18** : la première version de ce tableau doublait les durées des lignes
64 à 128 Ko. Débit de l'anneau : fréquence × 8 octets, soit 352,8 Ko/s à 44,1 kHz.)

Il ne protège que contre les retards du cœur B, qui décode en continu ; sa taille compte
surtout pour le gapless par fermeture et réouverture d'un module d'origine (§13.3, voie (a)).

**Cœur A** (SYSRAM, 576 Ko, aujourd'hui ≈ 25 Ko de code et ≈ 110 Ko de données dont 64 Ko pour
l'USB) : à venir, un tampon d'écran complet (320 × 170 × 2 = 106 Ko) pour pousser l'écran par
DMA comme RECHO, le décodage des pochettes, l'index trié du navigateur, la file de lecture,
l'écriture FAT. La SYSRAM suffit ; elle n'a pas besoin de la PMU-SRAM.

**PMU-SRAM** (64 Ko), usages possibles si elle ne porte pas l'anneau :
1. **Journal circulaire** (12 Ko, déjà) : il garde la trace d'un plantage.
2. **Marqueurs de démarrage** (`FAST`, étiquettes) : quelques octets, déjà.
3. **État de reprise** : piste, position, file de lecture, volume, réglages. Il survit à un
   plantage ou un reset (reprise immédiate) et à la veille profonde.
4. **Veille profonde** (pause prolongée, écran éteint) : VD_LOGIC coupé, réveil par Power ;
   le code de réveil et l'état vivent en PMU-SRAM. Consommation bien inférieure à un simple
   arrêt du cœur. Mécanisme à étudier et à mesurer (le TRM le décrit, rien n'est testé).
5. **Listes chaînées de la DMA**, petites : elles peuvent tout aussi bien rester en SYSRAM.

### 16.3 Trois dispositions possibles de la HIGHRAM1
| | A. Anneau de sortie en PMU-SRAM (RECHO) | B. Anneau fixe de 64 Ko en haut de la HIGHRAM1 | C. Disposition décidée par module |
|---|---|---|---|
| Anneau de sortie | 48 Ko (139 ms à 44,1 kHz) | 64 Ko (186 ms à 44,1 kHz, 43 ms à 192 kHz), banc 3 entier | 64 à 128 Ko selon le module |
| Données du codec + anneau d'entrée | 256 Ko | 192 Ko | ce qui reste |
| Modules d'origine 73, 75 | oui | oui, sans patch | oui (64 Ko) |
| Écriture du PCM par le cœur B | à travers le pont, plus lente | dans sa propre RAM | dans sa propre RAM |
| PMU-SRAM | occupée aux trois quarts | libre pour 16.2 | libre pour 16.2 |
| Complexité | la plus simple (validée sur l'appareil) | simple | une table par module, calculée au chargement |

**Mon avis** : la disposition B maintenant, en gardant la possibilité de C. Elle libère la
PMU-SRAM pour ce qu'elle seule sait faire (survie au reset et à la veille), laisse 192 Ko aux
codecs (assez pour Opus, Vorbis, FLAC 44,1/48 et les modules 73 et 75 tels quels), et met
l'anneau dans un banc entier, sans concurrence d'accès avec les données du codec. On passerait
à C si le FLAC haute résolution l'exige (plus de données, ou un anneau plus long à 192 kHz).

### 16.4 À vérifier avant de figer
1. Anneau en `0x01050000` (64 Ko) sur l'appareil, comme celui de 128 Ko (0.15).
2. Le DMAC1 lit-il la HIGHRAM0 ? Cela ouvrirait sa partie libre (93 Ko avec le module MP3) à
   l'anneau.
3. La PMU-SRAM survit-elle à notre extinction (GPIO2_6 à 0) ou seulement au reset ? Et que
   devient-elle en mode basse consommation (étape ultérieure) ?
4. Besoins réels de Opus et Vorbis une fois recompilés avec notre runtime (édition de liens).

**Décision (utilisateur, 2026-10-18) : disposition B.** Anneau de sortie de 64 Ko en
`0x01050000`–`0x0105ffff` (banc 3 de la HIGHRAM1) ; HIGHRAM1 `0x01020000`–`0x0104ffff` (192 Ko)
pour les paramètres, l'état, les données des codecs et l'anneau d'entrée ; PMU-SRAM réservée au
journal, aux marqueurs, à l'état de reprise et à la veille profonde. La disposition C reste
possible plus tard pour le FLAC haute résolution.

### 16.5 Étape 5b : flux continu, fréquences, 16 bits (image 0.16)
- **Anneau par défaut** : disposition B (64 Ko, `0x01050000`). Le cœur A tient le rôle du cœur
  B : il écrit une note de 1 kHz (table de 1024 points, accumulateur de phase) en avance sur la
  DMA, jusqu'à 4 Ko de garde ; position de lecture tirée de `SAR`, manques de l'anneau
  (producteur dépassé) et de l'I2S (`INTSR` bit 1, effacé par `INTCR` bit 2) comptés,
  remplissage minimal suivi. La rampe de volume continue d'alimenter l'anneau.
- **Fréquences** (table de RECHO `0x0305fc88`) : paramètres de la PLL de l'ACODEC en `+0x204`…
  `+0x210` (le dernier décalé de 4), `CKR` = `0x33f3f` (MCLK 256 fs) jusqu'à 96 kHz et
  `0x13f3f` (128 fs) à 88,2, 176,4 et 192 kHz ; seul registre des DAC qui change : `0x01000b`
  = indice (1 à 44,1 kHz, 2 à 48, 3, 4, 5, 6 à 192). Le changement refait toute la séquence,
  comme RECHO.
- **Échantillons 16 bits** (test) : I2S `TXCR` = `0x000f` (VDW 16, HWT 0) : un mot de 32 bits par
  trame stéréo, gauche dans la moitié basse ; trame I2S inchangée (64 SCLK), échantillon en tête
  du créneau de 32 bits suivi de zéros, **DAC inchangés**. Anneau de 64 Ko : 371 ms à 44,1 kHz
  (348 ms utiles), au lieu de 186. Destiné aux sources 16 bits (MP3, AAC, CD) ; FLAC, ALAC, DSD
  en 24 bits gardent le format 32 bits.
- Écran de test : MENU fait défiler 44,1 (32 et 16 bits), 48 (32, 16), 88,2, 96, 176,4,
  192 (32, 16) kHz, puis les anneaux PMU-SRAM et HIGHRAM0 à 44,1 ; Power suspend le producteur
  500 ms (manque attendu dès que l'anneau dure moins de 500 ms).
- **Sur l'appareil (0.16, 2026-10-18)** : toutes les configurations fonctionnent ; **16 bits :
  même hauteur, même volume, aucun grésillement à l'écoute** (DAC inchangés) ; anneaux
  HIGHRAM1, PMU-SRAM et **HIGHRAM0** (le DMAC1 la lit aussi) ; 0 erreur DMA, 0 erreur I2C,
  0 manque de l'I2S. Remplissage minimal hors suspension : 148 ms sur 174 à 44,1 kHz 32 bits,
  326 sur 348 en 16 bits, 18 sur 40 à 192 kHz 32 bits (le cœur A perd jusqu'à ~25 ms en
  dessin, journal et touches). La suspension de 500 ms provoque bien des manques là où l'anneau
  dure moins de 500 ms (y compris 16 bits à 44,1 kHz : 348 ms). Le compteur comptait chaque
  passage dans la boucle (des milliers) : 0.17 compte des épisodes et la durée rejouée.

## 17. Étape 5c : cœur B et module WAV d'origine, image 0.18 (2026-10-18)
- **Chargement** (`src/hifi.c`) : le firmware cherche à la racine une image d'origine dont
  l'overlay 76 est un module HiFi (en pratique `STOCK39.IMG`), lit sa table d'overlays
  (en-tête `+0x6c`), copie le code en HIGHRAM0 et les données en HIGHRAM1, cœur B en reset. Les
  modules ne font donc jamais partie de notre image.
- **Démarrage** : PD_HIGH (valeurs de RECHO), bloc de paramètres (`+0x20`, `+0x24` = fréquence
  du cœur), états de la boîte aux lettres effacés, reset relâché, attente du message 1.
- **Lecteur** (`src/player.c`) : descripteur `0x65` en RAM du cœur A, OPEN, réponses aux
  requêtes de fichier (`0x6b` lecture écrite directement dans la RAM du cœur B, `0x69` seek,
  `0x73` fermeture, position renvoyée dans le bloc de requête), puis à chaque message 3 ou 5 le
  tampon PCM 16 bits est copié tel quel dans l'anneau 16 bits et DECODE n'est envoyé qu'ensuite ;
  fin au message 6 une fois l'anneau vidé, puis CLOSE. Lecture à position quelconque :
  `fat_pread()` avec curseur de cluster.
- **Anneau** : le module 76 occupe la HIGHRAM1 jusqu'à `0x0105f2bc`, donc l'anneau va en
  HIGHRAM0 banc 1 (`0x01010000`, libre : le code du module s'arrête à `0x010088cc`), validé par
  la DMA en 0.16.
- **Émulation** : `tools/emu_coreb.py` ajoute à `emu_fw.py` un second processeur (modèle de
  `emu_host.py`) qui partage la HIGHRAM, la SRAM et la boîte aux lettres, et la position de la
  DMA avance avec le temps simulé. Fichier de 3 s (440/660 Hz, 16 bits, 44,1 kHz) : module chargé
  identique à l'image, message 1 en 200 µs, OPEN (44 100 Hz, 2 canaux, 16 bits, 3000 ms),
  56 lectures, 33 tampons (dont le premier de 4096 trames muettes), fin de flux à 2972 ms,
  aucun manque. À noter : 131 072 trames utiles pour 132 300 dans le fichier, le dernier tampon
  partiel n'est pas rendu par le module (28 ms).
- Menu : BACK sur l'écran principal ouvre « Audio tests » (note, et chaque `.WAV` de la racine).

## 18. Étape 5d : MP3 (73) et AAC (75) d'origine, image 0.20 (2026-10-18)
- Même lecteur que le WAV (§17), PCM recopié dans l'anneau (pas encore l'option B de patch du
  runtime). Anneau selon le module : HIGHRAM1 banc 3 (disposition B) pour 73 et 75, HIGHRAM0
  banc 1 pour 76. Menu : `.WAV` (76, codec 3), `.MP3` (73, codec 0), `.M4A` (75, codec 2).
- Accélérateurs du MP3 (TRM) : `hclk_synth` = porte 6·16+14, `hclk_imdct` = porte 7·16+14,
  resets `synth` / `imdct` = `SOFTRST0` bits 15 / 14 : désormais tous réglés à l'allumage de
  PD_HIGH (l'horloge d'IMDCT ne l'était qu'au démarrage de la DMA, après OPEN).
- Émulation : AAC (« Philip Glass – Cloudscape », M4A AAC-LC 44,1 kHz) : ouverture, 208 tampons
  de 1024 trames, 276 lectures et 13 seeks (trois positions dans le MP4), aucun manque. Durée
  annoncée par le module : 279 783 ms pour 209 840 réels (à noter, calcul du module d'origine).
  MP3 : impossible en émulation (IMDCT36/SYNTH non modélisés), test sur l'appareil seulement.
- Fichiers de test copiés à la racine (copies, originaux intacts) : `TEST1.MP3` (Geese – Cobra,
  320 kb/s, 44,1 kHz, pochette ID3 intégrée), `TEST2.M4A`.

### 18.1 Sur l'appareil (0.20) : MP3 et AAC jouent ; réouverture du même module bloquée
- **MP3 d'origine (73) : fonctionne** (Geese – Cobra, 320 kb/s : 49 s jouées, 1 872 tampons,
  0 manque, attente maximale d'un tampon 25 ms) ; les accélérateurs IMDCT36 et SYNTH répondent.
  AAC (75) : 68 s, 0 manque.
- **Défaut** : rouvrir le module qui vient de jouer (WAV puis WAV, AAC puis AAC) échoue : prêt,
  première lecture de 512 octets servie et prise (`A2B st 0`), puis plus rien (« descriptor not
  taken »). Un autre module entre les deux fonctionne.
- **Cause probable** : seul le cœur B était remis en reset ; le mailbox (autorisations
  d'interruption restées à `f`), le DMAC2 et le HIFIACC gardaient l'état laissé par la session
  précédente. Le firmware d'origine, à l'arrêt, remet en reset la matrice PD_HIGH, les deux
  HIGHRAM, le cœur B et le mailbox (§1.4). Bits (TRM) : `SOFTRST1` 14 mailbox ; `SOFTRST2`
  0 matrice, 1 HIGHRAM1, 2 HIGHRAM0, 3 HIFIACC ; `SOFTRST3` 2 cœur B, 7 DMAC2.
- **0.21** : `hifi_stop()` met cœur B, DMAC2, HIFIACC et mailbox en reset ; `hifi_load()` fait
  en plus une impulsion sur la matrice et les HIGHRAM, puis relâche tout sauf le cœur B ;
  `B2A_INTEN` réécrit à `0xf` au démarrage.

### 18.2 0.22 : faute du cœur A en HIGHRAM, échecs aléatoires ; hypothèse de l'accès à la HIGHRAM
- Trace de démarrage (0.22) : séquence normale du module WAV (lecture de 512 octets, `66`, en-tête,
  deux retours au début, 12 Ko, message 3 vers 21 ms) ; le MP3 lit la fin du fichier (ID3v1),
  le début, puis balaie le fichier (positions 1,2 Mo à 5,9 Mo) avant le message 3 (~100 ms).
- **Faute** : HardFault forcée, `CFSR` = `0x400` (erreur de bus imprécise), dans la copie du FIFO
  eMMC vers la mémoire (`data_command`), c'est-à-dire une écriture du cœur A dans un tampon du
  module en HIGHRAM1, pendant l'ouverture du MP3. La même séquence (mêmes positions, mêmes
  adresses) passe au démarrage suivant : l'adresse est valide, l'accès échoue par moments.
- Avec les échecs « descriptor not taken » (cœur B muet après une lecture, aléatoire en 0.21),
  cela évoque des accès à la HIGHRAM hors marge : cœur B à PLL / 2 (200 MHz), accès en un cycle
  (`GRF_INTER_CON0` bits 14 `caliram`, 15 `caldram` à 0). L'origine les active au-delà de 300 MHz
  (`0x0305237a` : `hclk_cal` ≥ 300 MHz → `0x305301c(0xc000, 1)`) ; RECHO démarre le cœur B à
  PLL / 8 et ne l'accélère que selon la charge.
- **0.23** : cœur B à PLL / 4 (100 MHz, bloc de paramètres en conséquence) et accès multi-cycles
  des deux RAM du cœur B activés ; remis à zéro au redémarrage. Si les échecs cessent, on
  rouvrira progressivement (étape 5g, fréquence selon la charge).
- **La PMU-SRAM ne survit pas à l'extinction** (Power 2 s, GPIO2_6 à 0) : le compteur de
  démarrages du journal est reparti de 1 (réponse à la question 3 du §16.4) ; elle survit au
  redémarrage après une faute (« 2368 bytes recovered »).
- AAC (0.22) : 201 épisodes de manque de l'anneau en 6 s : **expliqué**, ce démarrage était en
  repli à 24 MHz (PLL arrêtée, cœur B sur le quartz), trop lent pour l'AAC (constat de
  l'utilisateur, confirmé par la ligne `clock` du journal).
- **0.23 sur l'appareil (session normale à 200 MHz)** : 10 ouvertures (MP3, AAC, WAV, dont des
  réouvertures du même fichier), **0 relance, 0 échec, 0 faute, 0 manque**. L'hypothèse de
  l'accès à la HIGHRAM est confirmée (cœur B à 100 MHz, accès multi-cycles).
- Défauts vus en 0.23, corrigés en **0.24** : le marqueur de repli (`FAST`) n'était jamais
  effacé pendant un démarrage en repli, si bien que tous les démarrages suivants restaient à
  24 MHz (jusqu'à l'extinction) ; en repli, le cœur B tombait à 6 MHz (WAV arrêté à 0,6 s, MP3
  en manque) : la lecture y est désormais refusée ; le journal n'était pas vidé vers le fichier
  pendant la lecture (lignes perdues pendant la trace de démarrage d'un MP3) ; le message 5
  arrivé après BACK n'est plus signalé comme inattendu.

## 19. Étape 5f : notre runtime du cœur B et nos décodeurs (étude, 2026-10-18)
**Décision de l'utilisateur** : pour la diffusion, ne plus dépendre des modules de l'image
d'origine. Pas de réécriture à partir de leurs binaires (œuvre dérivée, même règle que pour le
SDK) : notre propre runtime et des décodeurs libres à licence permissive. **Formats retenus pour
l'instant : WAV, MP3, Vorbis, Opus, FLAC** ; les autres (AAC, ALAC, APE, DSD) attendront. Les
modules d'origine restent un outil de développement et une référence (qualité, charge, usage des
accélérateurs).

### 19.1 Décodeurs
| Format | Décodeur | Licence | Taille, charge | Remarques |
|---|---|---|---|---|
| WAV | le nôtre | BSD | quelques Ko | PCM 16/24/32 bits ; sert à mettre au point le runtime |
| MP3 | PacketVideo (OpenCORE), dans AOSP `frameworks/av/media/libstagefright/codecs/mp3dec` (fichiers `pvmp3_*`) | **Apache-2.0** (« Copyright (C) 1998-2009 PacketVideo », en-tête vérifié) | état ≈ 21 Ko (portage micro-mp3 d'ESPHome) ; en entiers ; ESP32 à 240 MHz : 3,2 fois le temps réel à 128 kb/s | charge sur Cortex-M3 à mesurer (le module d'origine, avec IMDCT36/SYNTH, prend ~31 MHz) ; les accélérateurs sont documentés dans le TRM (chap. 27, 28) et pourront être utilisés ensuite, d'après le TRM |
| Vorbis | Tremor (notre portage, pleine précision) | BSD | 68 Ko de code et tables, ~70–80 Ko de données | validé sur l'appareil (HIFIEC54–58) |
| Opus | libopus 1.5.2 en virgule fixe (notre portage) | BSD | ~75 Ko de code, ~60–80 Ko de données | validé sur l'appareil (HIFIEC46–58) |
| FLAC | libFLAC (Xiph, BSD-3) ou dr_flac (domaine public / MIT-0, un seul fichier, plus petit) | permissives | résidus et sortie ~64 Ko pour des blocs de 4096 | LPC du HIFIACC en option plus tard ; choix entre les deux après essai de taille et de vitesse |

Apache-2.0 est compatible avec une diffusion sous BSD (fichier NOTICE et texte de licence à
fournir).

### 19.2 Runtime du cœur B (le nôtre)
- **Une image par codec** (HIGHRAM0 = 128 Ko de code : Opus et Vorbis ne tiennent pas
  ensemble), toutes liées avec le même runtime : démarrage, vecteurs, boîte aux lettres, anneaux,
  bloc d'état, mesure de charge (méthode de la sonde HIFIEC64).
- **Images rangées dans notre propre firmware** (`HIFIEC70.IMG`), lues par le cœur A dans la zone
  FW1 de l'eMMC : plus besoin de `STOCK39.IMG` ni d'aucun fichier sur le volume.
- **Plan mémoire (disposition B)** : HIGHRAM0 = code du codec ; HIGHRAM1 `0x01020000` bloc de
  contrôle et d'état (4 Ko), puis pile (8 Ko), données du codec jusqu'à `0x01047fff`, **anneau
  d'entrée** `0x01048000`–`0x0104ffff` (32 Ko, données compressées), **anneau de sortie**
  `0x01050000`–`0x0105ffff` (64 Ko, PCM 16 ou 32 bits).

### 19.3 Protocole entre les cœurs (le nôtre, plus simple que celui d'origine)
- **Pas de requêtes de lecture** : le cœur A remplit l'anneau d'entrée d'avance avec le fichier ;
  le cœur B consomme. Pour les formats qui sautent dans le fichier (fin de MP3 pour l'ID3v1,
  recherche de position), le bloc de contrôle porte une demande de repositionnement.
- **Bloc de contrôle** : commandes du cœur A (démarrer, arrêter, pause, aller à, piste
  suivante), positions des deux anneaux (écriture de A, lecture de B ; écriture de B, lecture de
  la DMA publiée par A), **file de pistes** pour le gapless (début de chaque piste dans le flux
  d'entrée, longueur, retard et remplissage d'encodeur), format de sortie choisi par B.
- **Bloc d'état** (§6) : charge du cœur B, erreurs, marges des anneaux, piste entendue.
- **Boîte aux lettres** : seulement des « sonnettes » (de A : données ou commande ; de B :
  besoin de données, changement d'état), plus de protocole requête-réponse.
- Gapless (§13.3) : le cœur B enchaîne les pistes du même format sans vider l'anneau de sortie ;
  changement de format ou de fréquence : on vide, on recharge.

### 19.4 Ordre proposé
1. Runtime + WAV : chargement depuis notre image, démarrage, anneaux, bloc d'état ; émulable
   entièrement (`emu_fw.py` + `emu_coreb.py`).
2. FLAC (dr_flac ou libFLAC).
3. Opus et Vorbis : nos portages, réédités avec le nouveau runtime.
4. MP3 (PacketVideo), mesure de charge ; puis, si besoin, les accélérateurs d'après le TRM.
5. Gapless (file de pistes, retrait du retard d'encodeur).

### 19.5 Étape 5f-1 : runtime + WAV (0.25, 2026-10-18)
- `fw/hifi/rt.c` (runtime : vecteurs, sonnette A2B canal 1 → interruption 0x1a, boucle sur le
  bloc de contrôle, WFI ; fautes rapportées dans le bloc), `fw/hifi/codec_wav.c` (RIFF, PCM
  8/16/24/32 bits, mono/stéréo ; 16 bits → trames compactées de 4 octets), `fw/hifi/link.ld`,
  `fw/src/hifi_proto.h` (protocole commun). Image du codec WAV : 1 436 octets.
- `tools/mkpack.py` : paquet de codecs `NNCP` ; `mkimg.py --pack` le range après le code,
  en-tête +0x148 = {1, offset, longueur}. `hifi_load_own` lit le paquet dans FW1 (sinon FW2)
  par secteurs entiers, directement en HIGHRAM0/HIGHRAM1.
- Cœur A (`player_play_own`) : anneau d'entrée rempli par secteurs entiers (la position
  d'écriture reste alignée), sonnette après chaque apport ou tous les 4 Ko consommés par la
  DMA ; démarrage de la sortie 8 Ko devant la DMA. Après un manque, le cœur B rattrape la
  DMA (`out_space`).
- `emu_fw.py` : le cœur B avance de 5 000 instructions par appel à `ticks()` (50 µs à
  100 MHz) quand il est éveillé ; bloc de contrôle affiché à la fin.
- Émulation : TEST.WAV entier (132 300 trames = 3 000 ms), 146 lectures, 0 manque, 242 réveils
  du cœur B. **Ni STOCK39.IMG ni module d'origine pour le WAV.** MP3 et M4A passent encore par
  les modules d'origine.
- **Sur l'appareil (0.25)** : TEST.WAV 3 000 ms, 0 manque ; TEST3.WAV (69 Mo, Tears For Fears
  « Shout » converti du FLAC) lu en entier : 17 316 994 trames = 392 675 ms (exact), 0 manque,
  aucun défaut à l'écoute ; 35 438 réveils du cœur B. 134 598 lectures (≈ 514 octets en
  moyenne) : à regrouper (attendre 4 Ko de place) pour l'énergie. 0.26 : durée transmise
  (`total_frames`).

### 19.6 FLAC : dr_flac contre libFLAC (mesures, 2026-10-18)
Banc (dans le scratchpad, hors dépôt) : chaque décodeur compilé pour Cortex-M3 (gcc 14,
`-ffunction-sections`, `--gc-sections`), exécuté nu dans Unicorn ; entrée par rappels de lecture
(comme depuis notre anneau), allocateur à pointeur croissant (pic du tas), pile peinte. PCM
comparé à ffmpeg. 20 s de chaque fichier : A = « Shout » original (16/44,1, blocs 4096, pochette
38 Ko), B = extrait réencodé `flac -8`, C = extrait 24 bits / 96 kHz `flac -5`.

| | dr_flac v0.13.4 | libFLAC 1.4.3 (décodeur seul, entiers) |
|---|---|---|
| Licence | domaine public ou MIT-0 (sans attribution) | BSD-3 (notice à fournir) |
| Sources | un en-tête | 10 fichiers + config.h |
| Code `-O2` / `-Os` | 30,8 Ko / 26,4 Ko | 31,5 Ko / 27,5 Ko |
| Tas (pic) | **37 Ko** | **84 Ko** |
| Pile | 4,9 Ko | 3,9 Ko |
| A 16/44,1 (`-O2`) | 8,8 M instr/s d'audio | 8,6 M instr/s |
| B `flac -8` (`-O2` / `-Os`) | 8,9 / 8,3 M | 8,6 / 9,3 M |
| C 24/96 (`-O2`) | **17,0 M** | 19,2 M |
| PCM | identique à ffmpeg (A, B, C) | identique (A, B, C) |
| Métadonnées (pochette…) | sautées par le rappel de déplacement | lues et jetées octet par octet |

Charge estimée sur le cœur B (CPI ~1,3–1,5 avec les accès multi-cycles HIGHRAM) : ~12–15 MHz en
16/44,1, ~25–30 MHz en 24/96, sur 100 MHz. **Choix : dr_flac** (moitié moins de mémoire, vitesse
égale, plus rapide en 24/96, un seul fichier, licence sans contrainte, saut des métadonnées par
déplacement : utile pour les grosses pochettes). Contrepartie : un seul mainteneur, moins éprouvé
que l'implémentation de référence → le tester sur des fichiers corrompus ou tronqués (le cœur B
rapporte déjà ses fautes dans le bloc de contrôle).

### 19.7 Étape 5f-2 : FLAC avec dr_flac (0.27, 2026-10-18)
- `fw/hifi/ext/dr_flac.h` (non modifié, provenance dans `ext/README`), `fw/hifi/codec_flac.c` :
  dr_flac tire ses données par le rappel de lecture, qui attend dans `rt_wait()` ; sorties par
  paquets de 1 024 trames (16 bits) ou 512 (32 bits), attente de place dans l'anneau. CRC des
  trames conservé. Tas du runtime (`rt_alloc`, de la fin du BSS à `HP_DATA_END`), `memcpy`,
  `memmove`, `memset`, `memcmp` dans `rt.c`. Image : 31,9 Ko de code.
- **Saut vers l'avant** (`in_seek`) : le cœur B place `in_rd` au-delà de `in_wr` ; le cœur A
  y voit un saut, reprend la lecture du fichier à cette position (un premier morceau jusqu'au
  secteur suivant, puis des secteurs entiers).
- **Lectures regroupées** : le cœur A attend 8 Ko de place avant de relire (WAV de 3 s :
  65 lectures au lieu de 146).
- Émulation : extrait de 5 s de « Shout » (`flac -5`) avec une pochette de 714 Ko : 220 500
  trames = 5 000 ms, pochette sautée (578 Ko lus sur 1,28 Mo), tas 37 Ko, 0 manque.
- Sur l'appareil : TEST4.FLAC (nom court TEST4~1.FLA), Depeche Mode « Enjoy the Silence »,
  copie inchangée de la bibliothèque (16/44,1, 255 s).
- **Sur l'appareil (0.27)** : TEST4.FLAC lu en entier : 11 251 380 trames = 255 133 ms, 0 manque,
  ouverture en 12 ms (63 904 octets lus avant le format : métadonnées, pochette de 28 Ko
  incluse), 4 076 lectures (≈ 8 Ko chacune), 19 390 réveils du cœur B ; écoute satisfaisante.
  TEST3.WAV (lectures par 8 Ko) : 0 manque. MP3/M4A (modules d'origine) toujours bons.

### 19.8 Étape 5f-3 : Opus et Vorbis (0.28, 2026-10-19)
- **Lecteur Ogg du cœur B** (`fw/hifi/oggpage.c`, préfixe `pg_` : `ogg_packet` est un type de
  Tremor) : pages du premier flux logique, resynchronisation sur « OggS », paquets trop longs
  sautés par `in_seek`, `pg_last_on_page()` pour le granule. **Durée** : le cœur A lit le
  granule de la dernière page (64 derniers Ko du fichier) et le passe dans le bloc de contrôle
  (`end_granule_lo/hi`), seul champ ajouté au protocole.
- **Opus** (`codec_opus.c`) : libopus 1.5.2 variante validée (HIFIEC52–58 : `OPUS_ARM_INLINE_ASM`,
  `ECHO_DECODER_ONLY`), FEC et PLC **conservés** (tout tient). Objets compilés depuis
  `re/opus/opus-1.5.2-var`. 66 Ko de code, 73 Ko de données. Pre-skip et coupe finale sur la
  page EOS.
- **Vorbis** (`codec_vorbis.c`, `heap.c`) : Tremor lowmem + nos patchs (`re/vorbis/tremor-lowmem`),
  pleine précision, `_ARM_ASSEM_` + `ECHO_SSAT` (HIFIEC56). En-tête de commentaires sauté, Tremor
  reçoit un en-tête vide. 56 Ko de code, 106 Ko de données (tas 88 Ko).
- **Émulation, PCM capturé** (`emu_fw.py --pcm`) :
  - Opus 128 kb/s, 5 s : 240 000 trames (pre-skip et fin exacts), écart à `opusdec` (flottant)
    2,4 LSB eff., 11 max, sur un passage à −34 dBFS ; 0 manque ;
  - Vorbis q5, 5 s, commentaire de 200 Ko : 220 500 trames, écart au décodeur flottant de
    ffmpeg 0,7 LSB eff., **2 max** ; tas 30 Ko ; 0 manque.
- Pistes de test pour l'appareil : TEST5.OPUS (New Order « Blue Monday », 451 s, 48 kHz) et
  TEST6.OGG (Philip Glass « Vessels », 486 s, 44,1 kHz), copies inchangées de la bibliothèque.
- **Sur l'appareil (0.28, 2026-10-19)** : TEST5.OPUS (48 kHz) 84 s puis 13 s, TEST6.OGG (44,1 kHz)
  62 s : 0 manque, durées justes (450 800 ms, 485 840 ms), ouverture 25 ms (Opus) / 70 ms
  (Vorbis), écoute bonne. Remplissage minimal 5,2–6,9 Ko (au démarrage : sortie lancée 8 Ko
  devant la DMA) → à relever (démarrer la DMA avec 16–32 Ko prêts).

### 19.9 MP3 : PacketVideo (notre codec) contre Helix (mesures, 2026-10-19)
Banc Unicorn (comme §19.6), Cortex-M3 `-O2`, 20 s. PacketVideo = **notre `codec_mp3.c` complet**
(découpage, ID3v2, Xing/Info/VBRI, tag LAME) sur un runtime simulé ; Helix (libhelix-mp3,
RealNetworks) avec sa propre boucle, un `assembly.h` Cortex-M3 écrit pour le banc
(`smull`/`clz`), polyphase en C. Références : ffmpeg.

| Fichier | PacketVideo | Helix |
|---|---|---|
| Geese « Cobra » 320 kb/s | 18,8 M instr/s, SNR 86,7 dB, err. max 2 | 14,7 M, 87,9 dB, 2 |
| LAME 128 kb/s CBR | 16,9 M, 78,7 dB, 4 | 13,4 M, 81,1 dB, 1 |
| LAME VBR V2 | 18,5 M, 79,7 dB, 4 | 14,4 M, 81,5 dB, 1 |
| Code / état | 34 Ko / 27,8 Ko | 32 Ko / 23,8 Ko |
| Licence | Apache-2.0 | RPSL 1.0 (copyleft par fichier) ou RCSL (commerciale) |

- PacketVideo : sortie **alignée sur ffmpeg (décalage 0)** sur les fichiers LAME (retard 529 du
  décodeur + retard et remplissage de l'encodeur retirés) ; sans tag LAME, seul le retard 529
  est retiré.
- Helix : −22 % d'instructions, SNR un peu meilleur (1 à 2 dB). Charge estimée sur le cœur B :
  ~25 MHz (PacketVideo) contre ~19 MHz (Helix), sur 100 MHz ; le module d'origine (avec IMDCT36
  et SYNTH) mesuré ~31 MHz sur le matériel.
- **Choix : PacketVideo** (licence permissive ; la charge laisse de la marge). Pistes si besoin :
  macros ARM de PacketVideo (`pv_mp3dec_fxd_op_arm_gcc.h`, à vérifier en Thumb-2), accélérateurs.
- 0.29 : le MP3 passe par notre codec (codec 3, 34 Ko de code, 53 Ko de données) ; le cœur A
  donne la taille du fichier dès le départ (`in_end`), pour l'estimation de durée des CBR sans
  en-tête Xing.
- Émulation 0.29 : MP3 LAME 192 kb/s de 5 s avec une pochette ID3v2 de 714 Ko : pochette sautée
  (154 Ko lus sur 852 Ko), 220 500 trames, **décalage 0 et erreur max 3 LSB face à ffmpeg**,
  0 manque.

## 20. Tampon du DAC en SYSRAM (0.30, 2026-10-20)
**Décision de l'utilisateur** : anneau du DAC de 128 Kio en SYSRAM, anneau d'entrée des codecs
de 32 Kio, sortie du cœur B portée à 96 Kio.
- **HIGHRAM1** : données des codecs `0x01023000`–`0x0103ffff` (limite `HP_DATA_END` =
  `0x01040000` ; Vorbis, le plus gros, finit en `0x0103d994`, 10 Kio de marge), entrée
  `0x01040000` (32 Kio), sortie du cœur B `0x01048000`–`0x0105ffff` (96 Kio). Les positions de
  la sortie sont des **décalages dans l'anneau** (96 Kio n'est pas une puissance de 2 : un
  compteur modulo 2³² décalerait l'adresse en rebouclant, après ~7 h).
- **SYSRAM** : `sysram_ring` de 128 Kio (`0x0301a980`), lu par le DMAC1 canal 5 (16 blocs de
  8 Kio). Le cœur A y recopie la sortie du cœur B à chaque tour (`own_service`) et sonne le
  cœur B tous les 4 Kio libérés. Les descripteurs de la DMA étaient déjà en SYSRAM ; la lecture
  de données en SYSRAM par le DMAC1 reste à confirmer sur l'appareil.
- **Démarrage** : PLAY envoyé avant le réglage des DAC (le cœur B décode pendant ce temps) ;
  la sortie commence 8 Ko après la position de la DMA. Temps affiché = trames écrites par le
  cœur B moins ce qui reste dans les deux anneaux.
- **Durées** (16 bits, 44,1 kHz) : 743 ms dans l'anneau du DAC + 557 ms dans la sortie du
  cœur B = 1,30 s ; à 192 kHz en 32 bits : 85 + 64 = 149 ms.
- **Écran de test** : configurations « sram 128K » (44,1 kHz en 16 et 32 bits, 192 kHz) ;
  Power bloque le producteur 500 ms : pas de manque attendu en 16 bits à 44,1 kHz (743 ms),
  un manque attendu en 32 bits (371 ms) et sur « hram1 64K » en 16 bits (371 ms).
- **Émulation** : WAV identique au bit près (132 300 trames), MP3 à 2 LSB de ffmpeg ; anneau du
  DAC plein 279 ms après le démarrage (sortie du cœur B : 94 Ko d'avance), 0 manque.
- **Outils** : le scratchpad de la session a été vidé ; environnement Python, disques et
  médias de test dans `re/newfw/work/` (local, jamais publié). `fw/tools/mkdisk.py` refait un
  disque de test, `fw/tools/pcmcmp.py` compare deux PCM.
- **FLAC 24 bits / 96 kHz, blocs de 8 192** (émulation 0.30) : 288 000 trames **identiques au bit
  près** à ffmpeg (trames de 32 bits), tas de dr_flac 70 Ko (sous la limite de 112 Ko), anneau du
  DAC plein après 232 ms, 0 manque. Fichier de test sur l'appareil : TEST8.FLAC (« Enjoy the
  Silence », 2 min, rééchantillonné en 24/96 par soxr).
- **Sur l'appareil (0.30, 2026-10-20) : le DMAC1 lit l'anneau en SYSRAM** (SAR `0x0301b800`…,
  0 erreur). 22 lectures des 8 formats et fréquences (WAV, FLAC 16/44,1 et **24/96**, MP3 par
  **notre codec** pour la première fois sur l'appareil, Opus 48 kHz, Vorbis ; M4A par le module
  d'origine, anneau HIGHRAM1) : **0 manque**. Au démarrage de la DMA, le cœur B a déjà 32 à 94 Ko
  prêts ; anneau du DAC plein en 104–295 ms ; ensuite le remplissage minimal reste à 120–126 Ko
  sur 128 (l'anneau reste plein). TEST8.FLAC : 96 kHz, 24 bits, trames de 8 octets, 0 manque.
  L'essai de la note (blocage de 500 ms par Power) n'apparaît pas dans le journal.

## 21. Enchaînement des pistes : mesures sur l'appareil (0.31, 2026-10-20)
Mode « chain the CH* files » du menu de tests : fichiers CH* de la racine joués à la suite
(ordre des noms), `PLAY_CHAIN`. Même codec : cœur B remis à zéro sans relecture
(`hifi_restart_own` : données initialisées recopiées, le runtime efface son BSS). Même fréquence
et même taille de trame : DAC et DMA gardés, l'anneau du DAC (719 ms au changement) joue la fin
de la piste précédente. Pistes : segments consécutifs de 3 s de « Shout », coupés à
l'échantillon près.

| Passage | Cœur B | Runtime prêt | Ouverture | DAC | 1er PCM après le début | Manques |
|---|---|---|---|---|---|---|
| → CH1 WAV (départ) | chargé 0,69 ms | 31 µs | 5,0 ms | démarrés 343 ms | 356 ms | 0 |
| CH1 → CH2 WAV | **remis à zéro 33 µs** | 30 µs | 4,4 ms | gardés | **4,6 ms** | 0 |
| CH2 → CH3 FLAC | chargé 2,2 ms | 92 µs | 5,5 ms | gardés | 28,7 ms | 0 |
| CH3 → CH4 FLAC | remis à zéro 42 µs | 92 µs | 5,4 ms | gardés | 27,3 ms | 0 |
| CH4 → CH5 MP3 | chargé 2,3 ms | 653 µs | 6,2 ms | gardés | 21,8 ms | 0 |
| CH5 → CH6 MP3 | remis à zéro 33 µs | 654 µs | 6,1 ms | gardés | 20,2 ms | 0 |
| CH6 → CH7 Vorbis | chargé 3,3 ms | 1,7 ms | **64,8 ms** | gardés | 73,8 ms | 0 |
| CH7 → CH8 Opus 48 kHz | chargé 3,9 ms | 1,1 ms | 15,0 ms | **vidés puis redémarrés 1 003 ms** | 1 029 ms | 0 |
| CH8 → CH9 Opus | remis à zéro 33 µs | 1,1 ms | 10,8 ms | gardés | 28,2 ms | 0 |
| CH9 → CHA FLAC 24/96 | chargé 2,2 ms | 92 µs | 5,6 ms | vidés puis redémarrés 959 ms | 975 ms | 0 |

- **Enchaînement sans coupure à l'écoute** quand la fréquence ne change pas, y compris entre
  formats avec perte (MP3 LAME, Vorbis, Opus) : l'anneau du DAC couvre largement le
  changement (pire cas 74 ms pour 719 ms en réserve). Coupures entendues seulement à CH7 → CH8
  et CH9 → CHA, aux changements de fréquence (DAC redémarrés).
- **Charger un codec coûte 2 à 4 ms** (lecture de l'image en eMMC, ~17 Mo/s), **le remettre à
  zéro 33–42 µs** : l'écart est négligeable devant l'ouverture et le premier décodage. Le temps
  « runtime prêt » est l'effacement du BSS par le cœur B (proportionnel : 106 Ko pour Vorbis).
- **Vorbis** : l'ouverture (65 ms) est le décodage des livres de codes de l'en-tête de setup.
- **Changement de fréquence** : vidage de l'anneau (~720 ms de musique, pas de silence) puis
  réglage complet des DAC (≈ 340 ms de silence, dont 230 ms de séquence I2C et 100 ms
  d'attentes) + 8 Ko d'avance. Piste d'amélioration : ne changer que la PLL de l'ACODEC, le
  CKR de l'I2S et l'indice de fréquence des DAC (`0x01000b`), sans refaire toute la séquence.

## 22. Changement de fréquence allégé (0.32–0.33, 2026-10-20)
- `audio_change_rate` : DAC laissés alimentés et configurés ; muet (I2C), I2S et DMA arrêtés,
  PLL de l'ACODEC, I2S (rapport, format 16/32 bits) et **indice de fréquence des DAC
  `0x01000b`** (seul registre des DAC qui dépend de la fréquence dans la séquence complète),
  DMA relancée, départ dès que la FIFO de l'I2S est remplie, volume rétabli.
- **Sur l'appareil (0.32)** : 10,9 ms (muet 1,1 ; PLL + I2S + DAC 5,5 dont 5 ms d'attente de la
  PLL ; anneau + DMA 3,1), 0 erreur I2C ; état des interruptions des DAC après le changement
  (`10`, `68`) identique à celui d'un démarrage complet. Coupures entendues beaucoup plus
  courtes (≈ 380 ms → ≈ 77 et 43 ms estimées : 4 Ko de silence en fin de piste, 8 Ko d'avance
  au départ).
- **0.33** : fin de piste à la dernière trame (attente que la DMA la dépasse, 1 Ko de silence de
  marge), départ 1 Ko devant la DMA (le cœur B a déjà ~90 Ko prêts), seuls les 8 premiers Ko de
  l'anneau remis à zéro. Silence attendu ≈ 15 ms, plus un éventuel fondu interne des DAC.
- **Sur l'appareil (0.33)** : changement en 7,9 ms (muet 1,1 ; PLL + I2S + DAC 5,5 ; anneau +
  DMA 0,16), 0 erreur I2C, état des DAC inchangé (`68`), 0 manque sur les 10 pistes. Silence
  estimé : 1 Ko de marge en fin de piste (≈ 5–6 ms) + 7,9 ms + 1 Ko d'avance (5,3 ms à 48 kHz
  16 bits, 1,3 ms à 96 kHz 32 bits) ≈ **19 ms (CH7 → CH8) et 15 ms (CH9 → CHA)**.
- **Écoute (0.33)** : un blanc reste audible aux changements de fréquence (silence estimé
  15–19 ms, plus un éventuel fondu interne des DAC). **Jugé satisfaisant par l'utilisateur** :
  un changement de codec et de fréquence au milieu d'un album est le pire cas ; sans changement
  de fréquence, l'enchaînement est continu.

## 23. Tampons réévalués (0.34, 2026-10-20)
Constat (§20–22) : quand le cœur A est occupé, seul l'anneau du DAC fait tenir le son (la
sortie du cœur B n'est vidée que par le cœur A) ; le cœur B décode bien plus vite que le temps
réel (anneau du DAC plein en permanence) ; le pire changement de piste (Vorbis, 74 ms) est
couvert. **Décision de l'utilisateur** :
- **sortie du cœur B 32 Ko** (`0x01058000`), Opus écrit ses paquets par morceaux (un paquet de
  120 ms fait 23 Ko) ;
- **données des codecs 180 Ko** (limite `0x01050000` : marge de Vorbis 10 → 74 Ko, FLAC à blocs
  de 16 384 possible) ; **entrée 32 Ko** en `0x01050000` ;
- **anneau du DAC 192 Ko** en SYSRAM (24 blocs de 8 Ko) : 1,11 s à 44,1/16, 128 ms à
  192 kHz / 32 bits ; fin du BSS du cœur A `0x0304dfc0` (8 Ko sous le code ; 152 + 64 Ko libres
  au-dessus du code).
- Émulation : WAV identique au bit près, anneau du DAC plein après 277 ms, 0 manque.

### 23.1 Ouverture de Vorbis : où passe le temps (banc Unicorn, 2026-10-20)
`work/bench_vorbis/` : notre `codec_vorbis.c` (+ `oggpage.c`, `heap.c`, objets Tremor du build)
sur un runtime simulé, instructions comptées par étape de `codec_open`.
- CH7.OGG : 2,34 M instructions, dont **94 % pour l'en-tête de setup** (42 livres de codes) ;
  « Vessels » (pochette dans les commentaires, 53 Ko) : 2,16 M, dont 90 % setup (44 livres),
  **3,6 % pour lire et sauter les commentaires** (16 Ko copiés, le reste sauté par `in_seek`,
  non lu par le cœur A). Les commentaires ne sont pas analysés (Tremor reçoit un en-tête vide).
- **Sur l'appareil, l'ouverture de CH7 a pris 65 ms → ≈ 2,8 cycles par instruction sur le cœur B**
  (code en HIGHRAM0, accès multi-cycles depuis la 0.23), et non 1,3 comme supposé jusqu'ici. Les
  charges du cœur B estimées depuis les bancs (§19.6, §19.9) sont donc à doubler environ : MP3
  320 kb/s ≈ 53 MHz, FLAC 24/96 ≈ 48 MHz, Opus 2,5 ms 256 kb/s ≈ 160 MHz (> 100 MHz). À mesurer
  sur l'appareil (étape 5g : sonde SysTick, fréquence du cœur B).

### 23.2 0.34 : le chargeur IDB effacé par notre BSS (2026-10-20)
- **Observé** : après l'installation de NEWFW034.IMG, gel, puis écran noir à chaque démarrage.
- **Cause** : le chargeur IDB (stage 2, 16 Ko) **se recopie et s'exécute en `0x0304a000`–
  `0x0304dfff`** (31 pointeurs de fonctions de son binaire dans cette zone ; données et pile en
  `0x03088000`–`0x0308ffff`). Il met à zéro le BSS de l'entrée 0 avant de sauter chez nous. Avec
  l'anneau de 192 Ko, le BSS allait jusqu'à `0x0304dfc0` : le chargeur a effacé son propre code.
  (0.30–0.33 : fin du BSS en `0x0303d108`, sous la zone.) L'émulateur ne pouvait pas le voir : il
  charge l'image lui-même.
- **Correction (0.35)** : anneau du DAC dans une section `.dacring` NOLOAD en `0x03020000`–
  `0x0304ffff` (ni chargée, ni effacée par le chargeur ou notre démarrage ; elle recouvre la copie
  du chargeur, morte une fois chez nous) ; BSS jusqu'à `0x0301dfb0`. Garde-fous : `ASSERT` dans
  `link.ld` (BSS ≤ `0x03020000`), `mkimg.py` refuse une entrée 0 dont données + BSS dépassent
  `0x0304a000`. Émulation : WAV identique au bit près.
- **Règle** : rien de chargé ni d'effacé par le chargeur au-delà de `0x0304a000`, ni dans
  `0x03080000`–`0x0308ffff` ; les grands tampons (écran à venir) en sections NOLOAD.
- Récupération : mode USB du chargeur (VOL− + RST, encore fonctionnel puisque le chargeur est
  dans l'IDB), puis `flash.sh` par l'utilisateur.
- **Sur l'appareil (0.35, 2026-10-20)** : démarre (écrite par `flash.sh` dans FW1), puis
  réinstallée par MENU (FW2 puis FW1, 706 ms, FW2 n'a plus la 0.34). Chaîne CH1–CHA : anneau du
  DAC en `0x03020000` (192 Ko), **0 manque**, 1 091 ms de réserve à chaque changement de piste à
  44,1 kHz, remplissage minimal 170–188 Ko sur 192 ; changements de fréquence 7,9 ms ; Opus
  (paquets écrits par morceaux dans la sortie de 32 Ko) correct.

## 24. Étape 5g : charge du cœur B et sa fréquence (0.36, 2026-10-20)
- **Base de temps** : TIMER1 (`0x400e0020`, TRM chap. 18 ; 64 bits, oscillateur 24 MHz,
  décompte libre depuis 2⁶⁴−1 ; portes `CLKGATE0` bit 12 et `CLKGATE8` bit 14, reset `SOFTRST1`
  bit 5, `CLKSEL2` : 24 MHz, diviseur 1), réglé par le cœur A, lu par les deux cœurs, indépendant
  de leurs horloges (le SysTick du cœur B avait échoué avec l'origine, HIFIEC61–64).
- **Mesure** : le runtime chronomètre ses sommeils (`idle_ticks`, `sleep_since` dans le bloc de
  contrôle) ; le cœur A calcule, par fenêtres de 250 ms, la part du temps éveillé et les MHz
  utilisés (part × fréquence). Résumé par piste : moyenne, maximum sur 250 ms, temps par palier.
- **Fréquence** : PLL (400 MHz) / n, n = 2 à 8 : 200, 133, 100, 80, 67, 57, 50 MHz (`hifi_set_div`,
  `CLKSEL0` bits 14:12, changé en marche ; accès multi-cycles de la HIGHRAM toujours actifs).
- **Régulateur automatique** : palier le plus bas qui garde la charge ≤ 60 % ; un palier de plus
  si l'anneau du DAC est sous la moitié et baisse ; montée immédiate, descente d'un palier après
  1 s ; palier maximal du début de la piste (chargement, ouverture) jusqu'à l'anneau du DAC plein
  (ou 3 s). Modules d'origine (M4A) : 100 MHz fixes.
- **Menu de tests** : MENU choisit automatique, ou 50, 100, 133, 200 MHz fixes (stabilité
  au-delà de 100 MHz jamais essayée depuis la 0.22).
- **Sur l'appareil (0.36, chaîne CH* en automatique)** : TIMER1 à 24 MHz (24 030 ticks en
  1 001 µs), lu par le cœur B sans faute ; cœur B à 133 et 200 MHz pendant 30 s, sans blocage.
  MHz utilisés en moyenne sur 3 s (anneau du DAC plein au début et à la fin : besoin du temps
  réel) : WAV 17,5 (lecture trame par trame : à réécrire), FLAC 16/44,1 24, MP3 192 kb/s 52,
  Vorbis q5 70, Opus 128 kb/s 74, FLAC 24/96 54. Le MP3 confirme ~2,8 cycles par instruction
  (53 MHz estimés). Le régulateur descend d'un palier par seconde : sur 3 s, il n'atteint pas
  son palier stable.
- **0.37** : codec WAV par blocs de 4 Ko (16 bits stéréo recopié tel quel, `out_write`) ;
  entrée « cycle the TEST* files » (pistes longues en boucle, mode d'horloge choisi par MENU,
  BACK arrête).
- Effet de la 0.34 (sortie du cœur B 96 → 32 Ko, données des codecs 116 → 180 Ko) : aucun sur
  la vitesse (ouvertures identiques, réveils du cœur B semblables ; tas de Vorbis fixe à 88 Ko).
- **Pistes longues en automatique (0.36)**, 0 manque partout :

  | Piste | MHz utilisés (moyenne) | Palier stable | Remarque |
  |---|---|---|---|
  | TEST1.MP3 (320 kb/s, 185 s) | 51,4 | 100 MHz (182 s) | |
  | TEST4.FLAC (16/44,1, 255 s) | 21,2 | **50 MHz** (249 s) | |
  | TEST5.OPUS (122 s) | 71,5 | 133 MHz (94 s) + 200 (28 s) | **29 changements** : oscillation 133 ↔ 200 sur les pointes de charge |
  | TEST6.OGG (Vorbis, 47 s) | 65,6 | 133 MHz (46 s) | |

  200 MHz tenu sans blocage (Opus, 28 s cumulées). À corriger : l'oscillation (monter seulement
  si l'anneau du DAC baisse, ou si la charge dépasse un seuil haut plusieurs fenêtres).
- **0.38** : régulateur corrigé : montée seulement si l'anneau du DAC est sous les 3/4 et perd
  plus de 4 Ko en une fenêtre, ou si la charge dépasse 85 % deux fenêtres de suite (l'anneau
  absorbe les pointes) ; descente inchangée. Mise sous tension du cœur B : garde la fréquence
  déjà réglée (la première piste après le démarrage perdait l'accélération à 200 MHz). Page de
  lecture : temps / durée ; anneau du DAC (ms, %, minimum, manques) ; cœur B (fréquence, charge et
  MHz utilisés sur 250 ms, remplissage de sa sortie) ; lignes de lecture FAT retirées.
- **Cycle long (0.37, automatique), un tour complet de 32 min + début du 2ᵉ, 0 manque** :

  | Piste | MHz utilisés (moyenne) | Palier stable |
  |---|---|---|
  | TEST3.WAV (6 min 33) | **0,6** (contre 17,5 avant la copie par blocs) | 50 MHz |
  | TEST4.FLAC 16/44,1 | 21,2 | 50 MHz |
  | TEST1.MP3 320 kb/s | 51,5 | 100 MHz |
  | TEST8.FLAC 24/96 | 55,6 | 100 MHz |
  | TEST6.OGG Vorbis | 64,6 | 133 MHz (483 s, 1 changement) |
  | TEST5.OPUS | 68,9 | 133 MHz (421 s) + 200 (30 s), **35 changements** (ancien régulateur) |

  Le remplissage minimal de l'anneau du DAC reste à 184–193 Ko sur 192 sur toutes les pistes.
- **0.39** : page de lecture : ligne du cœur B coupée en deux (« core B 100 MHz  load 48% (48 MHz) »,
  puis « core B out 25 KiB » : la ligne de la 0.38 dépassait les ~40 caractères de l'écran) ;
  avertissement sur le volume retiré (départ à −40 dB, jugé suffisant par l'utilisateur) ;
  cycle long sans les WAV (demande de l'utilisateur : plus d'information utile).
- **Sur l'appareil (0.38, 2026-10-20)** : chaîne CH* (11 pistes) et un tour complet du cycle long
  en automatique, 0 manque.
  - **Vorbis → Vorbis (CH7 → CH7B, même codec)** : remis à zéro 36 µs, runtime 0,85 ms,
    ouverture 40,5 ms, **premier PCM 43,3 ms** (estimé 42) ; CH7 (codec chargé) : 43,5 ms.
  - **Régulateur corrigé : TEST5.OPUS 1 changement** (contre 35 en 0.37) : 448 s à 133 MHz,
    3 s à 200 (accélération du départ). Paliers stables inchangés pour les autres pistes (MP3 et
    FLAC 24/96 100 MHz, FLAC 16/44,1 et WAV 50 MHz, Vorbis 133 MHz).

## 25. Étape 6 : navigateur, lecture en arrière-plan, file, tags (0.40, 2026-10-20/21)
Demande de l'utilisateur : s'inspirer de l'interface de RECHO (meilleure et plus réactive que
celle d'origine). Repris de RECHO : seulement l'ergonomie décrite sur sa page (pas de code).
- **Touches** (`ui.c`, `key_poll`) : appui court (au relâchement), appui long (600 ms), répétition
  de VOL+/VOL− (450 ms puis toutes les 90 ms).
  - Navigateur : VOL+/VOL− déplacer ; PLAY ouvrir un dossier ou jouer un fichier (file = les
    fichiers jouables du dossier, à partir de lui) ; PLAY long sur un dossier : y entrer et tout
    jouer ; BACK remonter ; MENU écran de lecture ; MENU long : menu principal.
  - Lecture : VOL+/VOL− volume (pas de 1 dB) ; PLAY pause/reprise ; PLAY long piste suivante ;
    BACK piste précédente (ou début de la piste après 3 s) ; BACK long arrêt ; MENU navigateur.
  - Power : appui court écran éteint/allumé ; 2 s extinction (dans tous les écrans).
  - Menu principal : Files, Now playing, USB lecture seule / lecture-écriture, installation,
    tests audio, écran système (l'ancien accueil), mode USB du chargeur. La récupération reste
    VOL− + RST (chargeur).
- **FAT** (`fat_list_dir`) : noms longs VFAT (somme de contrôle, séquence), UTF-16 → UTF-8
  (paires de substitution), sous-dossiers ; ignorés : `.`, `..`, étiquette, cachés, système,
  noms commençant par `.` ; minuscules NT des noms 8.3.
- **Liste** : dossiers d'abord, puis fichiers jouables (wav, flac, mp3, ogg/oga, opus), ordre
  naturel sans casse (« 2 » avant « 10 »), tri de Shell ; jusqu'à 1 000 entrées (`.dirbuf`, 64 Ko
  en `0x03080000`, zone du chargeur morte après le démarrage) ; le lecteur est servi pendant la
  lecture et le tri d'un dossier.
- **File** : jusqu'à 400 pistes (`.queuebuf`, 32 Ko en `0x03078000` ; le code doit finir avant,
  `ASSERT`). Enchaînement sans coupure (`PLAY_CHAIN`) ; saut de piste : sortie gardée, anneau du
  DAC vidé (`audio_flush`), sans redémarrer les DAC ; fin de la file : vidage puis arrêt.
- **Lecteur non bloquant** : `player_open` / `player_service` / `player_close` ; l'ancien
  `player_play_own` (menus de tests) repose dessus. Pause : DAC muets puis `I2S_XFER` = 0 (la
  DMA attend, rien n'est perdu).
- **Police** : 1 504 glyphes extraits de l'image de l'utilisateur à la construction (latin
  jusqu'à Latin Extended-B, grec, cyrillique, ponctuation, flèches, formes, notes, ponctuation CJK
  et kanas) ; glyphe absent = bit 7 de l'octet de largeur à 0 ; espace = largeur 0 (dessiné sur
  5 px). Les idéogrammes CJK et le hangeul (dizaines de milliers) ne sont pas inclus.
  `lcd_text_clip` : UTF-8, coupe par « … ».
- **Tags** (`tags.c`) : ID3v2.2/2.3/2.4 (puis ID3v1) ; commentaires Vorbis du FLAC ; paquet de
  commentaires Ogg Vorbis et Opus lu à travers les pages ; RIFF INFO. Pochettes et grands champs
  sautés par position. Test sur l'hôte (`work/tagtest`, lecture seule) : **1 444 pistes de la
  bibliothèque de l'utilisateur, titre et artiste trouvés pour toutes**.
- **Écran de lecture** : état et position dans la file ; titre (ou nom du fichier) sur deux
  lignes ; artiste ; album (ou dossier) ; temps / durée et barre ; format, fréquence,
  résolution, volume ; en petit : anneau du DAC, manques, fréquence et charge du cœur B.
- Émulation : parcours racine → dossier → lecture, noms longs accentués et kanas, ordre naturel,
  fichiers non jouables cachés, enchaînement de la file.
- À venir : reprise au démarrage (fichier ou PMU-SRAM), lecture récursive d'un dossier, CJK
  (police libre à intégrer, la police d'origine n'étant pas distribuable), recherche dans une
  piste, mode aléatoire et répétition.
- **Compléments de la nuit (0.40)** :
  - **Reprise** : le dernier secteur de `NEWFWLOG.TXT` (hors anneau du journal, ignoré par
    `readlog.py`) garde « NEWFWSTATE 1 » : chemin (clusters, sélection et haut de liste par
    niveau), piste, lecture en cours, volume, mode ; écrit au début de chaque piste, à chaque
    changement de dossier, à la pause et avant l'extinction (seulement s'il a changé). Au
    démarrage : retour dans le dossier (noms retrouvés en relisant les parents), sélection, file,
    et la piste relancée depuis son début si elle jouait. Émulation : deux démarrages de suite.
  - **Lecture récursive** (PLAY long sur un dossier) : fichiers du dossier puis sous-dossiers,
    en profondeur (8 niveaux, 64 sous-dossiers par niveau, 400 pistes).
  - **Modes** (BACK long sur l'écran de lecture) : dans l'ordre, répéter tout, répéter la
    piste, aléatoire (xorshift) ; gardés dans l'état de reprise.
  - **Batterie** en haut à droite de l'écran de lecture (courbe Li-ion sur la tension, lue toutes
    les 5 s ; « USB » si le câble est branché).
  - **Mémoire** : file en `0x03070000` (32 Ko, le code doit finir avant), liste du dossier en
    `0x03078000`–`0x0308ffff` (96 Ko : 1 000 entrées, noms gardés sur 72 octets, 50 places
    réservées aux dossiers). Cette zone est **partagée** avec le tampon USB (64 Ko) et ceux de
    l'installateur (2 × 8 Ko), jamais actifs en même temps (`shared.h`) : 80 Ko de BSS libérés
    (fin du BSS `0x0300b8d8`). Installation vérifiée en émulation depuis le menu principal
    (FW2 puis FW1 identiques à l'image).
  - Affichage : redessin partiel (seules les lignes dont la sélection ou la marque change ;
    rafraîchissement périodique limité à la barre de progression).
  - Grand dossier (1 203 entrées) en émulation : 953 gardées (+ 3 sous-dossiers), « 953+ »,
    tri naturel, défilement circulaire, répétition des touches.

## 26. Premier journal de la 0.40 sur l'appareil, et 0.41 (2026-10-22)

**Journal de la 0.40** (`work/logs/20261022_2007/`, 4 démarrages, dont une session d'environ 11 h) :
- **Enchaînement des albums** en Vorbis (Beast In Black) et en Opus (Daft Punk) : 0 décrochage aux changements naturels de piste. Au changement, l'anneau du DAC est plein (192 Ko), puis rempli à nouveau en 20 ms.
- **Durées** : lecture d'un dossier ≤ 1 ms ; tags de 0,5 à 1,8 ms. La reprise au démarrage retrouve le dossier.
- **Saut manuel** : 1 décrochage d'environ 10 Ko, qui correspond au silence inséré par `audio_flush`. C'est attendu.
- **Charge du cœur B** : moyenne de 63 MHz en Vorbis et de 70 MHz en Opus, la plupart du temps à 133 MHz.
- **Broche de charge `PIN_CHARGING`** : à 0 pendant la charge (USB, de 3,68 à 4,06 V) ; à 1 batterie pleine (USB, 4,20 V) ou sans USB.
- **Pause** : en 9 h de pause (de 4 963 à 37 594 s), la batterie a baissé à peu près au même rythme qu'en lecture (environ 50 mV par 40 min ; de 4,20 à 3,52 V sur toute la session). **Il faut un mode d'économie en pause et au repos.** C'est à faire.
- **OPEN Vorbis** : de 41 à 55 ms et de 95 à 155 Ko lus, contre 17 ms pour l'Opus. `pg_packet` copiait les 16 premiers Ko de l'en-tête de commentaires, puis sautait le reste par pas de 255 octets. Ces sauts restaient presque toujours dans ce que le cœur A avait déjà lu (32 Ko d'avance par `feed`), si bien que la pochette était lue en entier.

**0.41** :
- `hifi/oggpage.c` : `pg_packet(buf, max)` ne lit que les `max` premiers octets. Le reste est sauté par un seul `in_seek` par page, ce qui fait sauter le cœur A au-delà de son anneau. Même méthode que pour le firmware d'origine (`re/REPORT.md` §38).
- En-tête de commentaires : seuls 7 octets sont lus (Vorbis) ou 8 (Opus, le magic `OpusTags` est vérifié dans tous les cas).
- `ogg_end_granule` (cœur A) n'est pas modifié : il part déjà d'une fenêtre de 8 Ko, parcourue en remontant.
- Indicateur de batterie : « CHG » pendant la charge et « USB » une fois chargée (`charging()`, `board.c`).
- Vérification en émulation (Vorbis 173 Ko de pochette, puis Opus 205 Ko, tronqués à environ 245 Ko) : PCM identique octet pour octet à l'ancien lecteur Ogg (277 000 trames), mais **lecture identique elle aussi** : 237 893 octets lus, ouverture inchangée. ffmpeg découpe l'en-tête de commentaires en pages de 4 123 octets, alors que le cœur A lit 8 Ko par lecture et jusqu'à 32 Ko d'avance. Un saut d'une page reste donc dans ce qui est déjà lu. Le gain n'existe que pour des pages plus grandes que cette avance.
- Sur ce firmware, il resterait donc à aller plus loin : soit faire parcourir les en-têtes de page par le cœur A avant le démarrage du cœur B (peu de gain : une petite lecture par page de 4 Ko au lieu de lectures contiguës), soit garder les codebooks décodés d'une piste à l'autre quand l'en-tête de configuration est identique, ce qui est le cas sur tout un album. Les codebooks coûtent environ 2 M d'instructions (QEMU, `re/REPORT.md` §38), soit sans doute la plus grande part des 41 à 55 ms mesurées.

**Journal de la 0.41 sur l'appareil** (`work/logs/20261022_2211/`, 35 min). Installée par le firmware d'origine sous le nom `HIFIEC74.IMG`, puisqu'il ne cherche que `ECHOMINI.IMG` et `HIFIEC00`–`97.IMG` (`re/REPORT.md` §5). Enchaînement des 11 fichiers CH\* sans décrochage, FLAC 24 bits / 96 kHz compris.

| Fichier | OPEN (cœur B) | Lu | Tags (cœur A) |
|---|---|---|---|
| Vorbis aoTuV, pochette 270 Ko (placée **avant** le titre) | 67,6 ms | 279 Ko | 43,3 ms |
| le même sans pochette | 41,2 ms | 4,4 Ko | 0,4 ms |
| Vorbis ffmpeg, pochette 173 Ko (placée après le titre) | 53,2 ms | 180 Ko | 0,5 ms |
| le même sans pochette | 32,3 ms | 5,4 Ko | 0,4 ms |
| Opus, pochette 205 Ko | 42,8 ms | 208 Ko | 0,4 ms |
| le même sans pochette | 18,1 ms | 0,3 Ko | 0,4 ms |

- Les codebooks coûtent de 32 à 41 ms par OPEN Vorbis, et la pochette de 21 à 26 ms au cœur B.
- **`fat_pread` lisait au plus un cluster (4 Ko) par commande eMMC**, et le lecteur de tags 2 Ko non alignés, ce qui donnait une commande par secteur. Chaque commande coûte environ 0,5 ms.

**0.42** :
- **Cache des codebooks Vorbis** (`hifi/codec_vorbis.c`, `hifi/heap.c`, section `.noinit` dans `hifi/link.ld`).
  - Le tas de Tremor n'est plus effacé au redémarrage du cœur B. Après les en-têtes, `heap_keep()` fige un plancher, et les allocations de la piste se font au-dessus.
  - On garde `vorbis_info`, une copie des en-têtes d'identification et de configuration (8 Ko au plus), et une somme FNV de l'ensemble et du tas sous le plancher.
  - À l'OPEN suivant, si les deux en-têtes sont identiques octet pour octet et la somme intacte, `heap_resume()` repart de là, sans décoder les codebooks. Le journal l'indique par « codebooks from the cache ».
  - Toute écriture dans cette zone (autre codec chargé, Tremor qui modifierait sa configuration) fait échouer la somme : les en-têtes sont alors décodés normalement.
  - Émulation : quatre pistes (trois du même album, puis un aoTuV), PCM identique à la 0.41 sur 13 s.
- **Lectures eMMC groupées** : `fat_pread` lit en une commande les clusters contigus qui suivent (jusqu'à 64 secteurs par commande, `emmc_read`).
- **Tags** : lectures alignées sur le secteur, de 2 Ko, ou de 16 Ko pour l'Ogg. Le lecteur parcourt tout l'en-tête de commentaires et en déduit la page et le segment où il se termine (`og_packet_end`). Pour Vorbis, il vérifie que l'en-tête de configuration suit bien.
- **Le cœur B saute l'en-tête de commentaires** à l'endroit indiqué : `comment_end_page` et `comment_end_seg` dans `hp_ctl`, puis `pg_skip_to()`. La pochette n'est donc plus lue qu'une fois, par le cœur A, en grandes lectures. Banc hôte (`work/tagtest`) : fin des commentaires exacte sur les 506 Ogg (comparée à un calcul indépendant), et tags inchangés sur les 1 444 pistes.
- **Cœur A** : `cpu_wait()` (WFI jusqu'au tick SysTick de 1 ms) dans la boucle de l'interface et dans le menu, au lieu d'une boucle active à 200 MHz. L'émulateur remplace cette fonction par un simple retour.
- **Affichage** : au plus une image toutes les 33 ms (répétition des touches comprise), rafraîchissement périodique de 4/s (lecture) ou 2/s (navigateur), et rien n'est dessiné écran éteint. La dalle de type ST7789 balaie d'elle-même à 60 Hz (`FRCTRL2` = 0x0F, variante A) ou à environ 53 Hz (0x13, variante B).
- **Correction** : quand aucune piste ne s'ouvrait jusqu'au bout de la file, l'écran affichait « 4/3 » et un nom faux.

**Journal de la 0.42 sur l'appareil** (`work/logs/20261022_2233/`, 10 min, aucune erreur). Durées en ms, tags (cœur A) + OPEN (cœur B) :

| Fichier | 0.41 | 0.42 |
|---|---|---|
| Vorbis aoTuV, pochette 270 Ko (codebooks décodés) | 43,3 + 67,6 | 17,0 + 36,5 |
| le même sans pochette (codebooks du cache) | 0,4 + 41,2 | 1,2 + 13,1 |
| Vorbis ffmpeg, pochette 173 Ko (codebooks décodés) | 0,5 + 53,2 | 11,0 + 33,2 |
| le même sans pochette (cache) | 0,4 + 32,3 | 1,2 + 8,4 |
| Opus, pochette 205 Ko | 0,4 + 42,8 | 13,0 + 16,8 |
| CH7B.OGG (cache) | — + 40,3 | 1,2 + 12,6 |
| Album Dark Connection (Vorbis, pochettes 146 Ko), pistes 2 à 11 | 0,5 + 48 à 55 (0.40) | 8,9 à 10,1 + 8,6 à 16,1 |
| Album Discovery (Opus) | 0,5 + 17 (0.40) | 4,1 + 10,3 à 11,4 |
| WAV, FLAC, MP3 (CH\*) | 4,3 à 5,5 | 2,6 à 3,6 |

- **Cache des codebooks** : utilisé sur les pistes 2 à 11 de l'album, et sur toute piste qui suit une autre de même configuration. L'OPEN Vorbis passe de 32 à 41 ms à 8 à 16 ms.
- **Pochettes** : elles ne sont plus lues que par le cœur A. Les tags coûtent de 9 à 17 ms quand il y a une pochette (contre 0,5 ms quand le titre la précédait), mais le cœur B ne la relit plus. Au total, on gagne de 10 à 60 ms par piste.
- **Lectures groupées** : l'OPEN des WAV, FLAC et MP3 descend de 4,3–5,5 à 2,6–3,6 ms. Le « runtime » Vorbis passe de 845 à 170 µs, car le tas n'est plus dans le BSS effacé.

**0.43** : cible du régulateur du cœur B passée de 60 à 75 % de charge (`need10 = mhz10 * 4 / 3`). Avec 60 %, le Vorbis (63–66 MHz utilisés) et l'Opus (69–72 MHz) restaient à 133 MHz, avec une charge affichée de 47 à 50 %, car 100 MHz leur aurait donné 63 à 72 %. Ils devraient maintenant tourner à 100 MHz. Le nombre de changements de fréquence reste à surveiller.

**0.44 (autonomie, côté cœur A)** :
- **Écran** : extinction après 30 s sans touche. On coupe le rétroéclairage et on met la dalle en veille (ST7789 `DISPOFF` + `SLPIN` ; au réveil, `SLPOUT`, 120 ms, `DISPON`, dans `lcd_sleep()`). La première touche ne fait que rallumer. La touche Power passe par la même fonction, `sys_screen_set()`. L'écran de faute réveille la dalle sans SysTick (`lcd_wake_fault()`). Le rétroéclairage reste en GPIO à pleine luminosité, car on n'a pas encore trouvé de PWM.
- **Pause longue** : 10 s après la mise en pause, les deux CS43131 mettent en veille leur étage casque et leur port série (`PWDN_CTL` de `a6` à `f6`, GPIO2_0 à 1 comme à l'arrêt). À la reprise, on rejoue la séquence de `dac_setup()` (DXD1 `0x99`, `0x180000` = `45`, effacement de `0x40`, puis de `0x10` avec DXD13 = `20` pendant 10 ms). L'anneau et la position de la DMA sont conservés, si bien que la lecture reprend exactement au même point. Les éventuels bruits de commutation sont à vérifier à l'oreille.

## 27. Polices et affichage : ce que font les deux autres firmwares, et Fusion Pixel (2026-10-22)
**Police d'origine ≠ Fusion Pixel.**
- Comparaison des cellules 16 × 16 de la police 16 px de `HIFIEC39.IMG` avec Fusion Pixel 2026.10.16 (BDF, 10 et 12 px, proportionnelle ou à chasse fixe, les 7 variantes de langue), en cherchant tous les décalages possibles sur 28 caractères (latin, accents, kana, hangeul, idéogrammes) : **aucun glyphe identique**.
- Le « 中 » d'origine occupe 16 lignes, celui de Fusion 12 px 11 lignes. RECHO 0.10.0 embarque, lui, les polices d'origine à l'identique (§7). Fusion Pixel n'est donc la police d'aucun des deux, du moins dans les versions analysées.

**Le firmware d'origine lit chaque glyphe sur l'eMMC à la demande** : confirmé.
- `0x030631dc` (code résident) calcule `*(0x0301d4e0) + code × 33 + 0x20` pour la police 16 px, ou `+ code × 32 + 0x18` pour la police 12 px, et lit 2 octets par `0x0306559c`.
- `0x0306559c` appelle `0x030545e8`, qui appelle `0x03068ddc` avec le périphérique `0x30000`, la zone du firmware. C'est le schéma du SDK (`LcdGetResourceData(FontLogicAddress + code × taille)`, `sdk_ref/Common/Display/COMP_NewJ_12_CJK.c`).
- L'existence d'un cache sous cette lecture n'a pas été vérifiée.

**RECHO 0.10.0** (ses journaux sur l'appareil, `RECHO/ECHOLOG_*.TXT`) :
- « font: bitmap fonts from selected slot, 43 icons » : les polices sont lues dans la copie du firmware sur l'eMMC. Le code contient un « font cache misses %u ».
- « lcd: panel bus VOP », « lcd: push adma », « the DMA push ran stable for 5 s » : l'écran est alimenté par bandes, en DMA asynchrone, avec un repli en PIO.
- Chiffres relevés : « async 362 bands, cpu 18889 us (52 us a band) » en 120 s, « dma 640/0/0 ». (Au §3.1, la même version avait été vue en GPIO pur : le chemin VOP et DMA s'active donc selon ses marqueurs.)
- L'emplacement exact des bandes (SYSRAM ?) n'est pas vérifié.

**Fusion Pixel pour notre firmware** :
- Licence : OFL-1.1, sans nom réservé dans `OFL.txt`. Elle peut accompagner le firmware publié, avec la licence. Le programme qui la construit est sous MIT.
- 12 px proportionnelle : 37 690 glyphes (19 317 idéogrammes, 11 172 syllabes hangeul, kanas), environ 783 Kio de bitmaps.
- 10 px : 25 910 glyphes, environ 409 Kio.
- Les variantes de langue (zh_hans, zh_hant, ja, ko…) ne diffèrent que par la forme de certains idéogrammes.

**0.45 : Fusion Pixel 12 px intégrée** (variante japonaise, `FONT_LANG` dans le Makefile) :
- **Fichier de police** (`tools/mkfusion.py`, format A) : en-tête `FPXF`, table des 4 072 plages (premier code sur 32 bits, index du premier glyphe sur 16 bits), décalage de chaque groupe de 16 fiches, puis les fiches (avance, largeur, hauteur, décalages x et y, bitmap aligné sur l'octet). 37 690 glyphes, 1 000 Kio. Le fichier est placé dans l'image après les codecs, et l'en-tête `+0x80` donne sa position et sa longueur (l'emplacement de la police 16 px d'origine). L'image passe de 1 à 2 Mio.
- **`src/font.c`** : au démarrage, `font_init()` lit les tables dans FW1 (sinon FW2), soit environ 34 Ko de mémoire, par lectures de 4 secteurs. `font_glyph()` cherche la plage par dichotomie. Pour un glyphe absent du cache, une seule commande eMMC lit son groupe de 16 fiches (au plus 3 secteurs), qui entre en entier dans un cache de 256 fiches. Le journal minute compte ces lectures (« font reads »).
- **ASCII imprimable compilé dans le code** (`build/font_ascii.c`, 1,2 Kio) : il sert avant `font_init()`, sur l'écran de faute (sans eMMC), et si la police est illisible.
- **Lignes** de 16 px (ascendante 13, descendante 3), comme avant. Les glyphes plus hauts (diacritiques empilés) sont rognés à la ligne.
- **Fin de la dépendance à l'image d'origine** pour la police : `tools/mkfont.py` est supprimé. Le code passe de 104 à 58 Ko, et le BSS finit en `0x0301c2f4`.
- **Émulation** : noms de fichiers en japonais (kanji et kana), chinois, coréen, russe, grec, français et en symboles (★ ♪ ← …), tous affichés dans le navigateur et sur l'écran de lecture.

**0.46** : sur l'écran de lecture, le titre passe en **taille double** (chaque pixel de Fusion devient un carré de 2 × 2, sur des lignes de 32 px), sur deux lignes. La coupure se fait entre deux mots quand une espace tombe dans la seconde moitié de la première ligne, et un « … » termine ce qui dépasse (`lcd_text_clip_x`, `lcd_text_width_x`). La ligne de mesures (anneau, décrochages, fréquence et charge du cœur B) est retirée, car ces informations sont dans le journal. La ligne d'aide des touches l'est aussi, faute de place. Disposition, de haut en bas : état (0), titre (18 et 50), artiste (86), album ou dossier (102), temps (122), barre (140), format et volume (150).

**0.47** :
- **Défilement horizontal** des lignes artiste et album trop longues pour l'écran de lecture (`marquee_set`, `marquee_tick`, `lcd_text_scroll`). 1,5 s d'immobilité au début, puis 1 pixel toutes les 33 ms, en boucle, avec un blanc de 32 pixels. Rien n'est dessiné écran éteint.
- `draw_glyph_cols()` ne dessine que les colonnes d'une bande, ce qui permet les glyphes coupés en bord de ligne.
- **Tags** : artiste et album passent à 128 octets. Une coupe juste après l'octet de tête d'un caractère UTF-8 est maintenant retirée. Avant, elle s'affichait en caractère inconnu.

## 28. Horloge temps réel : un circuit externe sur l'I2C (analyse statique, 2026-10-23)
- **Pas de RTC dans le SoC** : le TRM et la fiche technique du RKNanoD n'en décrivent aucune.
- **Une horloge dans le firmware d'origine** : ses menus proposent « Date and Time », « Time Display » et « Clock Setting ».
- Au démarrage (`0x03062b8c`), il appelle `0x03062106`. Celle-ci, par `0x3060c54(5, 0xa2, 0xc8)` puis `0x3061310`, **lit 9 octets à partir du registre 2 du composant I2C d'adresse 0xA2** (0x51 en adresse sur 7 bits). C'est le plan de registres d'un PCF8563 ou d'un HYM8563 : secondes (`0x02`) à année (`0x08`), suivies des alarmes.
- La date et l'heure, rangées en `0x0301d042` et `0x0301d03e`, sont ensuite vérifiées (mois ≤ 12, jour ≤ 31, heure ≤ 23, minutes et secondes ≤ 59). Si elles sont invalides, une nouvelle lecture est faite, puis une valeur par défaut est mise : 2026-01-01 à 12 h (`0x7ea`).
- `0x030679c0` fait avancer secondes, minutes et heures en mémoire entre deux lectures.
- **Confirmé par RECHO** (ses journaux sur l'appareil, `work/logs/20261022_2007/ECHOLOG.TXT`) :
  - « RTC 0x51 on I2C1 C » : le composant est sur l'I2C1, broches C (GPIO2_12 SDA, GPIO2_13 SCL, fonction 3 ; `GRF_IOMUX_CON` [3:2] = 2 ; I2C1 en `0x40050000`, porte 8·16 + 4, reset 1·16 + 11).
  - RECHO lit l'horloge au démarrage et à l'entrée en USB (« rtc (boot): 2025-05-07 04:37:22 wd 3 VL »), et propose un écran « Clock » (« clock set », « the clock does not answer »).
  - **L'heure avance appareil éteint** : extinction vers 04:48:44, démarrage suivant à 04:48:54 ; extinction vers 04:49:03, démarrage suivant à 04:57:12.
  - Elle n'a jamais été réglée (mai 2025 en octobre 2026, bit VL levé).

**0.48 : horloge** (`src/rtc.c`, `i2c1_*` dans `src/i2c.c`) :
- **Lecture** au démarrage (« rtc (boot): … », avec « not set (VL) » si le bit VL est levé), puis toutes les 10 minutes. Entre deux lectures, l'heure suit `millis()`. Elle figure aussi dans le journal minute, avec la charge du cœur A (temps hors `WFI`, mesuré par TIMER1).
- **Réglage manuel** : menu principal → « Date and time ». VOL+/VOL− change le champ, PLAY passe au suivant (le dernier enregistre), BACK revient au précédent (sur le premier, il annule), MENU annule. L'écriture efface le bit VL.
- **Réglage automatique** après une session USB en lecture-écriture : on prend la date d'écriture la plus récente des entrées de la racine (`fat_dirent.mtime`, heure locale du PC). L'horloge est réglée sur cette date si elle n'était pas réglée, ou si elle a plus d'une minute de retard (`rtc_from_fat`).
- **Affichage** : « HH:MM » dans l'en-tête du navigateur et dans la barre d'état de l'écran de lecture, une fois l'horloge réglée.
- **Vérifications** : l'émulateur simule un PCF8563 sur l'I2C1, démarrant à 2025-05-07 04:37:22 avec VL. Lecture, réglage par le menu, écriture (`RTC set 2027-01-01T12:00:00`) et affichage sont validés. Banc hôte (`work/rtctest`) : conversions de dates exactes de 2000 à 2099 (jour de la semaine compris), réglage depuis une date FAT validé.

## 29. Recherche dans une piste et reprise au point exact (0.48, 2026-10-23, nuit)
**Protocole** : `hp_ctl.seek_ms` (posé par le cœur A avant l'OPEN, par `player_seek_hint()`). Le cœur B lit les en-têtes, puis saute **en avant** dans l'entrée. La reprise à l'endroit voulu se fait à l'intérieur de chaque codec, sans calcul du cœur A ; `ctl->frames` reçoit la position atteinte, ce qui fait afficher le bon temps par le cœur A.
- **WAV** : saut exact (début des données + trames × taille de trame).
- **FLAC** : octet proportionnel une seconde avant la cible, au moins après ce qui est déjà lu. `drflac__seek_to_approximate_flac_frame_to_byte()` y trouve et décode la trame suivante, d'où sa position exacte, puis `drflac__seek_forward_by_pcm_frames()` va jusqu'à la cible.
- **MP3** : octet d'après la table de 100 points de l'en-tête Xing (s'il y en a une), sinon en proportion (débit constant). Le décodeur se resynchronise sur la trame suivante, sans délai à retirer. Les premières trames, privées de leur réservoir de bits, sortent en silence (environ une trame). La position est celle d'une frontière de trame proche de la cible.
- **Opus** : saut proportionnel une seconde avant, jusqu'à la page suivante (`pg_jump`), puis `OPUS_RESET_STATE`. Les paquets sont décodés et jetés jusqu'au premier granule de page, qui donne la position, puis jusqu'à la cible. La seconde de marge sert au décodeur à converger.
- **Vorbis** : de même, avec `vorbis_dsp_restart()` et une rupture de numérotation des paquets (`packetno += 2`). La position est déduite du premier granule, puis les trames sont jetées jusqu'à la cible.
- **Reprise au démarrage** : l'état de reprise (« NEWFWSTATE 2 ») garde la position (`pos_ms`). Il est enregistré au début de chaque piste, à la pause, à l'extinction, et chaque minute pendant la lecture. Au démarrage, la piste reprend 2 s avant la position enregistrée.
- **Touches** : non attribuées. C'est à l'utilisateur de choisir.
- **Banc** : `work/seek/` contient des extraits de 40 s de vraie musique dans les 5 formats. `inject.py` écrit dans le disque de test un état de reprise à 22 s, avec la même somme de contrôle que le firmware. `xcmp.py` retrouve où commence le PCM capturé dans le décodage de référence de ffmpeg, par corrélation FFT, et donne le RSB.

### 29.1 Résultats, et le décalage de Vorbis (2026-10-23, matin)
**Résultats émulés, reprise à 22 s (départ à 20 s)** : WAV et FLAC exacts au bit près ; MP3 +9,8 ms (frontière de trame) ; Opus exact (RSB 67 dB : le décodeur converge pendant la seconde de marge). La dichotomie du cœur A (`ogg_page_before()`) lit maintenant jusqu'à 72 Ko au-delà du point médian : les pages de ffmpeg font environ 20 Ko, et l'ancienne fenêtre de 4 Ko retombait toujours sur la même page.

**Vorbis : un décalage de 358 ms, pas une faute.**
- La faute du cœur B vue dans la nuit (`decode_packed_entry_number`, lecture hors de la mémoire) ne s'est pas reproduite avec la dichotomie corrigée, ni dans l'émulateur, ni sous QEMU.
- **Banc QEMU** (`work/qbench/`, Cortex-M3 `mps2-an385`, le `codec_vorbis.c` du firmware avec un runtime de substitution) : il bouclait sans fin, car le bloc de contrôle est à `0x01020000`, qui n'existe pas sur cette carte (lu à 0, `in_rd` restait nul). Le banc force maintenant un `hifi_proto.h` qui le place en RAM (`-include`), et `run.sh` a un moniteur QEMU (`pcs.py` y lit le PC).
- Sous QEMU, la recherche commençait 14 776 trames (335 ms) plus loin que le décodage complet, et 358 ms plus loin que ffmpeg.
- **Cause** : le fichier de test (encodé par ffmpeg) déclare un granule de 28 232 sur sa première page audio, dont les 44 paquets donnent 44 032 trames. La spécification Vorbis (§A.2) dit alors de **couper le début** (15 800 trames), pour que la trame n soit au granule n. Tremor seul ne coupe que dans le paquet qui porte le granule (1 024 trames au plus) : la lecture normale sortait donc 14 776 trames de trop au début. ffmpeg, lui, ne coupe rien. Après une recherche, notre codec suit exactement les granules : les deux échelles différaient.
- **Correctif** (`start_trim()` dans `codec_vorbis.c`, comme vorbisfile) : avant la première trame, le cœur B lit la première page audio dans l'anneau d'entrée **sans la consommer** (`in_peek_ahead()`, nouveau dans `rt.c`, qui attend que le cœur A l'ait écrite ; l'anneau en garde au moins 24 Ko). Il additionne les trames de ses paquets d'après le numéro de mode de chacun (premier octet), et jette l'excédent sur le granule. Tremor reçoit des granules décalés d'autant (`gshift`), pour ne pas couper une seconde fois.
- **Vérification sous QEMU** : le décodage complet donne exactement 1 764 552 trames, soit le dernier granule ; la recherche à 22 s tombe exactement sur la trame 970 200, identique au bit près au décodage complet. Un fichier libvorbis (T1.ogg, rien à couper) donne aussi exactement son dernier granule.
- La référence de ffmpeg (`ref_OGG.raw`) a 15 800 trames de plus au début. `xcmp.py` montre donc +358 ms pour ce fichier : c'est ffmpeg qui ne suit pas la spécification.
- **Le défaut était dans l'émulateur** : sous QEMU, la recherche Opus est propre (22 LSB au plus d'écart avec ffmpeg). La sortie de l'émulateur lui est identique, sauf un échantillon (trame 17 761, canal droit à 32 767 au lieu de 1 832). L'erreur est déterministe et dépend de la disposition du code. `emu_host.py` délivrait les interruptions du cœur B (la sonnette du cœur A) en sauvant et rendant `R0`–`LR`, **mais pas `xPSR`**. Arrêté entre une comparaison et son branchement conditionnel, le cœur B repartait avec les drapeaux laissés par le gestionnaire. Le matériel, lui, empile `xPSR`. Les drapeaux (`APSR`) sont maintenant rendus aussi. C'est très probablement aussi la « faute Vorbis » de la nuit (`decode_packed_entry_number` suit un arbre selon des bits lus, et un mauvais branchement donne un index hors de la table).

## 30. Économie d'énergie en pause (0.49, 2026-10-23)
**Mesure (0.46, pause longue de 8,2 h, écran et DAC éteints)** : 4086 → 3715 mV, soit 45,5 mV/h. Sur la même plage (4086 → 3759 mV), la lecture continue d'Opus en 0.40 (écran allumé) a pris 4,05 h, la pause 6,90 h. La pause consommait donc environ 59 % du courant de lecture : il restait le cœur A à 200 MHz (WFI seulement), le cœur B allumé à 50 MHz (29 378 s à 50 MHz dans le journal), la PLL et les bus.

**Trois mesures (`ui.c` `power_service()`, appelée par la boucle de l'interface)** :
1. **Mise de côté (`PARK_MS`, 60 s de pause)** : la position (`player.time_ms`) est gardée, la piste arrêtée (`play_stop(STOP_NOW)`), les DAC mis en veille (`audio_stop()` les met maintenant toujours en veille, `audio_start()` les reprend de zéro), puis `hifi_sleep()` : cœur B en reset, portes d'horloge de `hclk_cal_core`, des accélérateurs, des HIGHRAM et du bus de PD_HIGH fermées (`CLKGATE3` `0x01ff`, `CLKGATE4` `0x170f`, les mêmes bits que `hifi_power()` ouvre). Le codec est rechargé à la reprise (`hifi_own_forget()`). PLAY reprend la piste à la position exacte (`player_seek_hint`), depuis l'écran de lecture ou depuis le navigateur sur la même piste. L'état de reprise garde la piste et la position d'une piste mise de côté : après une extinction, PLAY repart de là.
   - Un arrêt depuis la pause ne passe plus par `audio_pause(0)`, qui rallumait les DAC et enlevait la sourdine juste avant l'arrêt (risque de claquement).
   - PD_HIGH n'est pas coupé (`PMU_PWRDN_CON` bit 1) : le TRM demande une requête d'inactivité au bus avant, non étudiée.
2. **Horloges sur le quartz** (`clock_low()` dans `board.c`) quand rien ne joue et que l'écran est éteint, hors USB : cœur et bus à 24 MHz (diviseurs à 1, celui du cœur B gardé), **PLL éteinte** (`PLL_CON1` bit 14), SysTick recalé. Retour par `clock_low(0)` : PLL, attente du verrouillage, diviseurs d'avant, `MODE_CON`. L'allumage de l'écran (`sys_screen_set(1)`) repasse d'abord sur la PLL. Les périphériques gardent leurs diviseurs. L'eMMC et le SARADC prennent leur horloge « PLL ou 24 MHz » (`CLKSEL7`) après le sélecteur de `MODE_CON` : le chargeur et notre mode de secours à 24 MHz tournent ainsi, le journal reste donc écrit chaque minute (plus lentement). Pas en mode de secours (`cpu_hz` à 24 MHz).
3. **Extinction automatique (`AUTO_OFF_MS`, 10 min)** sans touche, rien ne jouant, écran éteint, sans alimentation USB : `sys_power_off()` (état enregistré, journal vidé, maintien d'alimentation relâché). S'il est toujours alimenté, nouvel essai seulement 10 min plus tard.

**Essais émulés** (délais réduits par `-DPARK_MS=3000u -DAUTO_OFF_MS=15000u -DSCREEN_TIMEOUT_US=5000000u`) : pause à 12,1 s, mise de côté à 15,1 s (position 3 971 ms, DAC en veille, cœur B endormi), écran éteint et quartz à 17,1 s, écran rallumé à 20,2 s, PLAY à 21,1 s : PD_HIGH rallumé, codec rechargé, départ à 3 971 ms ; seconde mise de côté à 26,2 s ; extinction à 38,9 s, puis un essai toutes les 15 s (l'émulateur ne coupe pas l'alimentation).
- Deux défauts trouvés ainsi : le passage sur le quartz était retenté à chaque tour quand l'horloge y était déjà (le journal se remplissait) ; `off_tried = millis() | 1` pouvait tomber 1 ms dans le futur, et la soustraction suivante débordait (essais d'extinction en rafale).
- **Émulateur** : `millis()` n'avançait pas (pas de SysTick ; tous les temps du journal étaient à 0) ; elle lit maintenant le compteur simulé, par un bout de code placé à côté de lui en PMU-SRAM.
- **À vérifier sur l'appareil** : la pente de la batterie en pause (journal minute), la reprise après la mise de côté (sans claquement), le réveil depuis le quartz (touches, écran), l'extinction après 10 min.

## 31. Recherche par la barre de progression (0.50, 2026-10-23)
**Choix (proposition A, validée par l'utilisateur)** : un mode « position » sur l'écran de lecture, proche de ce que les chaînes de RECHO laissent voir (`play: bar back %u -> %u ms of %u`, réglage « step », `keys: chord vol+- (%s first, adc %u)`). Le fonctionnement de RECHO n'est déduit que de ces chaînes ; aucun code n'en vient.

**Entrée : VOL+ et VOL− ensemble (l'accord)**. VOL− met l'échelle de touches à la masse (médiane 1), donc la paire se lit comme VOL− : l'accord n'est visible que par le passage direct d'une touche de volume à l'autre, sans relâchement entre les deux (`key_poll`, `EV_CHORD`).
- VOL+ d'abord : vu dès l'appui de VOL−.
- VOL− d'abord : vu seulement quand VOL− est relâché avant VOL+.
- Un relâchement de moins de 30 ms (l'anti-rebond de `key_scan`) passerait pour un accord. Sans conséquence : sans déplacement, le mode se referme sans rien faire.
- Le pas de volume donné par la première touche est annulé (`lvl_key`).
- Le journal note `keys: chord vol+- (vol+ first, adc N)`. **À mesurer sur l'appareil** : la valeur réelle de l'échelle pendant l'accord.

**Dans le mode** (`seek_begin`/`seek_move`/`seek_end`, `ui.c`) :
- VOL+ et VOL− déplacent une cible de 10 s par appui. Maintenus, ils accélèrent : 10 s, puis 30 s après 1,5 s, puis 1 min après 4 s, avec au plus 4 pas par seconde. La cible est bornée à [0, durée − 1 s].
- L'écran montre `go to 2:35 / 4:10   +1:05` en orange, la barre en bleu atténué avec une marque orange sur la cible, et l'aide `VOL+/- move  PLAY go  BACK cancel`.
- PLAY ou MENU : aller à la cible. BACK : annuler. 2 s sans touche (`SEEK_IDLE_MS`) : aller à la cible. MENU maintenu : annuler, puis ouvrir le menu principal.
- La musique continue pendant le choix. Il n'y a **qu'une réouverture à la fin** (`player_seek_hint` + `play_index`, DAC gardés par `STOP_SKIP`), quel que soit le nombre de pas. Un écart de moins de 0,5 s ne déclenche rien.
- Depuis une pause, la piste repart à la cible. Une piste mise de côté (§30) ne fait que changer la position gardée (`parked_ms`, enregistrée) : PLAY repartira de là.
- Le mode se referme sans rien faire si la piste change entre-temps (fin de piste, enchaînement).

**Essai émulé** (S.WAV de 40 s, `work/seek/d_K.img`) :
- Accord VOL+ d'abord, puis deux appuis sur VOL+ et PLAY : saut de 3,0 à 23,0 s (`ui: seek 5469 -> 23009 ms of 40000`, la position de départ étant celle de l'entrée dans le mode).
- Accord VOL− d'abord (vu au relâchement de VOL−), puis VOL− maintenu 2 s : la cible va à 0 avec l'accélération, et le saut se fait seul 2 s après le relâchement.
- Accord, puis BACK : annulé, aucun saut.
- Le volume reste à −40 dB après les trois accords.
- MENU ouvre ensuite le navigateur normalement.
- Chaque saut compte une sous-alimentation du DAC au moment de la réouverture (3 à 7 Ko, dans la ligne `player: finished`).
- **À vérifier sur l'appareil** : la détection de l'accord dans les deux ordres, la lisibilité de l'écran, et la coupure audible au saut.
- Pendant ce travail, un remplacement scripté a effacé par erreur une partie de `ui.c` (noms, dossiers, file). Elle a été restaurée depuis `fwbase/src/ui.c`, qu'aucune modification n'avait touché dans cette zone depuis. Les tailles des fonctions dans le binaire sont identiques à celles de la build 0.49 de test.
- **0.50** : `work/NEWFW050.IMG`, CRC `0x25c451c8`.

## 32. L'écran suit ce qu'on entend, pas ce que le cœur B décode (0.51, 2026-10-23)
**Constat (signalé sur l'appareil)** : la barre de progression semblait arriver au bout alors que la musique jouait encore environ 1 s.
- La position elle-même était juste : `player_service()` déduit déjà le contenu des deux tampons (cœur B, et SYSRAM du DAC). Exemple du journal : « 01. Beast In », durée 247 967 ms, position 246 877 ms à la fin du décodage, soit 1 090 ms dans le tampon de 192 Ko.
- La cause : une piste est « finie » dès que le cœur B a tout livré au tampon du DAC, qui joue encore jusqu'à 1,1 s (1,02 s à 48 kHz, environ 0,26 s à 96 kHz 24 bits). L'interface ouvrait alors tout de suite la suivante (enchaînement sans blanc) et l'affichait, titre et barre vide compris, pendant que l'ancienne finissait. La barre de l'ancienne restait à « durée − 1,1 s » (99,5 % sur 4 min).
- En fin de file, `player_chain_end()` vidait le tampon par une boucle bloquante, avec l'écran figé environ 1,1 s.
- Dans l'émulateur (deux WAV de 40 s enchaînés) : la barre de A1 était figée à 97 % à 7,25 s, A2 était affiché à 7,5 s, alors que A1 s'entendait jusqu'à environ 8,47 s.
- RECHO semble faire la même distinction (chaînes `player: now hearing track %u`, `(the last one has drained)`). C'est une déduction faite seulement d'après les chaînes.

**Correctif** :
1. **Piste entendue et piste décodée séparées.**
   - `player.c` : une « queue » (`tail`) est notée à la fermeture d'une piste finie en enchaînement. Elle contient la position du tampon du DAC où finissent ses trames (`audio.wr`, toutes y sont déjà) et son nombre de trames.
   - `player_heard` (fréquence, bits, durée, position, compteur `gen`) décrit ce qu'on entend. Pendant la queue, la position est calculée d'après ce qui reste à jouer de l'ancienne piste.
   - Au passage du DMA (`audio.rd`) sur la fin de l'ancienne piste, la nouvelle est publiée : `gen` augmente et le journal note `player: now hearing …`. Le journal note aussi l'ouverture en avance (`player: X queued behind N ms of the last track`).
   - `ui.c` : `q_cur` reste la piste entendue, pour l'écran, les touches, la reprise, la recherche et la mise de côté. `q_dec` est la piste décodée, pour l'enchaînement. Les tags sont lus dans `tg_next`, et `heard_sync()` les recopie dans `tg` au passage.
   - La reprise est enregistrée au moment où l'on entend la nouvelle piste. BACK, PLAY maintenu et la recherche portent sur la piste entendue.
2. **Vidage non bloquant en fin de file** : `player_drain_service()` est appelée par `ui_background()` (état `draining`).
   - Elle écrit le même 1 Ko de silence, puis rend la main jusqu'à ce que le DMA dépasse la dernière trame.
   - La pause suspend le délai maximal de 2 s.
   - Un saut ou un arrêt (`play_stop`, `player_drop_tail()`) l'abandonne.

Le tampon n'est toujours jamais vidé entre deux pistes de même format. Seul le changement de fréquence garde son vidage bloquant (dans `player_open`, non traité).

**Essais émulés** :
- **Enchaînement** : `player: A2.WAV queued behind 1088 ms of the last track` à 7,392 s, `now hearing A2.WAV` à 8,454 s. La barre de A1 continue d'avancer jusqu'au passage (602, 606, 614, 618 pixels sur 624 de 7,25 à 8,3 s), puis A2 s'affiche avec une barre vide.
- **Fin de file** : « playing 0:39 / 0:40 » à 8,0 s, puis « stopped 0:40 / 0:40 » avec la barre pleine. Avant, l'écran restait figé à 0:38.
- **Non-régression** : la sortie PCM de l'enchaînement est identique octet pour octet à celle de la 0.50, et le scénario de recherche du §31 donne le même journal.
- **0.51** : `work/NEWFW051.IMG`, CRC `0x0dd9110c`.

## 33. Changement de fréquence entre deux pistes sans bloquer (0.52, 2026-10-23)
**Avant** : quand la piste suivante avait une autre fréquence ou une autre taille de trame (44,1 → 48 kHz, par exemple), `player_open()` appelait `player_chain_end()`. Ce vidage bloquant du tampon SYSRAM durait jusqu'à 1,1 s, puis venait `audio_change_rate()`. L'écran et les touches restaient figés pendant ce temps.

**Maintenant** :
- `player_open()` ouvre le codec, lance le décodage (`HP_CMD_PLAY`) et marque l'attente (`dac_switch`), puis rend la main. Le journal note `player: X at 48000 Hz waits for N ms of the last track at 44100 Hz`.
- Pendant l'attente :
  - `own_service()` ne recopie rien du cœur B dans le tampon du DAC. Le cœur B remplit son propre tampon de 32 Ko puis attend, et l'entrée continue d'être lue.
  - `dac.rate`/`dac.fb` gardent les valeurs de l'ancienne piste, ce qui reste juste si la piste est fermée pendant l'attente.
  - L'écran suit la queue de l'ancienne piste (§32).
- `player_service()` suit le vidage par `drain_step()`, partagé avec la fin de file :
  - du silence est écrit derrière la dernière trame à mesure que le DMA libère de la place, pour qu'une boucle principale en retard ne fasse jamais rejouer d'anciennes données ;
  - une pause suspend le délai maximal de 2 s.
- Une fois l'ancienne piste finie, `audio_change_rate()` (ou `audio_start()` si le changement échoue) est appelée, puis la nouvelle piste est publiée (`player: now hearing`).
- Le journal `chain:` donne la durée du changement lui-même, sans l'attente.
- Fermer la piste pendant l'attente (saut, arrêt) l'annule.

**Émulateur** : `emu_fw.py --dac FILE` enregistre ce que le DMA lit dans le tampon, à chaque lecture de sa position : c'est ce qu'on entend. La capture est fiable, car le micrologiciel n'écrit jamais au-delà de la dernière position lue. Les fichiers `--pcm` et `--dac` sont maintenant ouverts sans tampon : l'émulateur se termine sans les vider, et la fin des captures était perdue.

**Essais émulés** (captures côté DAC) :
- **44,1 kHz (WAV) → 48 kHz (Opus)** :
  - A1 est entendue en entier et sans trou, suivie de 564 trames de silence (environ 12 ms : la marge et le changement), puis A2 complète.
  - Le changement a lieu à 8,466 s, soit 1,1 s après la fin du décodage de A1. L'écran montre A1 (0:39 / 0:40, 44,1 kHz) jusque-là, puis A2 (Opus 48 kHz, avec ses tags).
- **Saut pendant l'attente** (PLAY maintenu) : A1 est coupée à l'appui, suivie d'environ 60 ms de silence sans données parasites, puis A2 repart du début à 48 kHz.
- **Même fréquence** : A1 puis A2 sans blanc, à la trame près.
- **Fin de file** : A1 entière, puis uniquement du silence jusqu'à l'arrêt du DAC.
- **0.52** : `work/NEWFW052.IMG`, CRC `0xe831d4c7`.

## 34. Touches fiables, tags après un saut, fin de liste (0.53, 2026-10-23)
**Constats sur l'appareil (0.52, journal du 23/10)** :
- La piste en cours se relançait parfois au lieu de passer à la suivante (24/25 → 24/25, 25/25 → 25/25, 13/13 → 13/13), parfois juste après une pause non voulue. L'utilisateur l'attribue à un faux contact de PLAY.
- Mécanisme : l'échelle de touches est filtrée par un RC. Quand le contact de PLAY (270) lâche, la tension remonte vers « rien » (1023) en traversant MENU (384) et BACK (515). Si elle reste 30 ms dans la plage de BACK, `key_scan` y voit un appui sur BACK : la piste est relancée (lue depuis plus de 3 s) ou la précédente est ouverte. Une coupure brève donnait aussi un appui court (pause) au milieu d'un appui long.
- Accords VOL+/VOL− non voulus : un contact douteux de VOL+ seule peut descendre jusqu'à la plage de VOL− (la masse), ce qui se lit comme l'accord (« adc 1 » dans le journal).
- Tags décalés : après un saut vers une piste d'une autre fréquence, l'écran gardait les tags de la piste précédente (le nom du fichier quand elle n'en avait pas). `play_index` changeait le numéro tout de suite et l'écran se redessinait ; les tags n'arrivaient qu'à la publication de la piste entendue, après le changement de fréquence (§33), et rien ne redessinait alors l'écran en entier. Reproduit dans l'émulateur (B.OPUS affichée avec les tags de A.WAV, C.WAV avec ceux de B.OPUS).

**Changements** :
- `key_scan` (`main.c`), délai selon la transition :
  - de « rien » vers une touche : 30 ms, comme avant ;
  - relâchement d'une touche tenue : 60 ms ;
  - VOL+ ↔ VOL− (l'accord) : 100 ms ;
  - d'une touche tenue vers une autre : jamais, seul un relâchement compte.
  - Une lecture écartée de plus de 15 ms est notée : `keys: play held: back for 60 ms ignored`.
- `key_poll` (`ui.c`) :
  - chaque événement est noté avec la valeur de l'échelle prise pour la touche : `keys: back (adc 515, 180 ms)`, `keys: play long (adc 270)`, `keys: vol+ held (adc 126)` ;
  - VOL+/VOL− en appui court agissent au relâchement : un accord qui suit n'a plus de pas de volume à annuler. Maintenues, elles répètent à partir de 450 ms, sans pas de plus au relâchement. `lvl_key` (le volume avant la touche) est relevé tant qu'aucune touche n'est tenue.
  - Limite : VOL− met l'échelle à la masse et cache VOL+. Un accord commencé par VOL− et relâché d'un coup se lit comme un appui sur VOL−.
- Fin de liste : en mode dans l'ordre, PLAY maintenu sur la dernière piste arrête la lecture (`ui: end of the queue (next on the last track)`). L'écran affiche « stopped » à 0:00, et PLAY rejoue la piste depuis le début.
- Tags : `play_index` publie aussitôt la piste choisie (`q_cur`, `tg`), et l'écran se redessine en entier dès que la piste entendue change (`heard_gen`), pas seulement quand le numéro change.

**Essais émulés** :
- Sauts WAV 44,1 → Opus 48 → WAV 44,1 : « Nightvision / Daft Punk » sur B.OPUS, le nom du fichier sur C.WAV.
- Dernière piste : « stopped 3/3, 0:00 / 0:40 », puis PLAY la rejoue depuis le début.
- Touches : VOL+ court à −40 dB tant qu'elle est tenue, −39 dB au relâchement ; accord VOL+ puis VOL− détecté 100 ms après, sans changement de volume ; PLAY maintenu avec 60 ms de BACK puis 40 ms de « rien » au milieu : les deux sont écartés et notés, l'appui long passe à la piste suivante.
- **0.53** : `work/NEWFW053.IMG`, CRC `0x0efdc9a2`.

**0.54 (2026-10-23), d'après l'essai de la 0.53 sur l'appareil** :
- Constat : les faux contacts de PLAY ont disparu sur 19 min (15 appuis longs, aucune relance ni pause parasite), la fin de liste et la recherche fonctionnent. Mais des clics rapides sur VOL+/VOL− laissent 50 à 60 ms de relâchement entre eux : avec le délai de 60 ms, deux clics n'en faisaient qu'un (appuis notés à 387-413 ms), et une fois la fusion a passé 450 ms et lancé la répétition (7 fois `vol+ held: none for 51-60 ms ignored`).
- Changement : le relâchement de VOL+/VOL− compte de nouveau après 30 ms ; PLAY, MENU et BACK gardent 60 ms.
- Valeurs de l'échelle relevées sur l'appareil pendant cet essai : VOL− 1, VOL+ 123-124, PLAY 246, MENU 372, BACK 494-497 (plus bas que l'étalonnage de 270/384/515 ; marges d'au moins 48 points jusqu'aux seuils).
- Émulateur : trois clics de VOL+ séparés de 50 ms donnent trois pas (−40 → −37 dB) ; une coupure de 40 ms pendant PLAY maintenu reste écartée.
- **0.54** : `work/NEWFW054.IMG`, CRC `0x72d1c160`.

## 35. ReplayGain (0.55, 2026-10-23)
**Choix de l'utilisateur** : mode album par défaut, préampli 0 dB, 0 dB pour une piste sans étiquette.

**Lecture des étiquettes** (`tags.c`) :
- FLAC, Ogg Vorbis : commentaires `REPLAYGAIN_TRACK_GAIN`, `REPLAYGAIN_ALBUM_GAIN`, `REPLAYGAIN_TRACK_PEAK`, `REPLAYGAIN_ALBUM_PEAK`, sans tenir compte de la casse. Les commentaires Ogg de moins de 512 octets sont maintenant tous lus : avant, la lecture s'arrêtait une fois titre, artiste, album et numéro trouvés.
- Opus : `R128_TRACK_GAIN` / `R128_ALBUM_GAIN`, entiers signés en 1/256 dB rapportés à −23 LUFS, auxquels on ajoute 5 dB pour la référence ReplayGain (−18 LUFS). Ils l'emportent sur d'éventuels `REPLAYGAIN_*`. Le gain d'en-tête Opus est déjà appliqué par le décodeur (`OPUS_SET_GAIN`, `codec_opus.c`).
- MP3 : trames ID3v2 `TXXX` portant les mêmes noms (Latin-1, UTF-16 ou UTF-8).
- WAV : rien.
- Stockage dans `struct tags` : gains en 1/100 dB (`RG_NONE` si absents), pics en Q16.

**Application** (`ui.c`) :
- Par le volume numérique des CS43131 (pas de 0,5 dB), sans calcul sur les échantillons : atténuation = volume choisi − gain (`out_level()`), bornée à 0 dB.
- Mode album : gain d'album, sinon gain de piste. Pic : celui du gain choisi, sinon l'autre. Le gain est limité pour que le pic reste sous 0 dBFS (marge comptée par pas de 0,5 dB, en Q16 : ×10^(0,5/20) = 69419/65536).
- Appliqué quand la piste **s'entend** (`heard_sync`, §32) ou dès un saut demandé, avec la rampe courte de `audio_volume`. En pause, seul le niveau rendu par `audio_pause(0)` change.
- Menu principal : « ReplayGain: album / track / off » ; PLAY fait défiler sur place. Le mode est rangé dans l'état de reprise (bits 8-15 de `mode`, 0 = album, ce qui garde les états de la 0.54 valides).
- Écran de lecture : `RG +6.0` (le gain appliqué, après limitation) ou `RG none`.
- Journal : `ui: replaygain album: tag -400/100 dB, peak 62259/65536: -4.0 dB applied`, à chaque changement de valeur.
- Le menu principal défile désormais : avec cette entrée, il a 10 lignes pour 9 visibles.

**Essais émulés** (fichiers étiquetés de `work/rg/`, mode album puis piste) :

| Fichier | Étiquettes | Appliqué |
|---|---|---|
| A.FLAC | album −4,00 dB, pic 0,95 | −4,0 dB |
| B.OGG | piste +8,20 dB, pic 0,5 (pas d'album) | +6,0 dB (limité par le pic) |
| C.OPUS | R128 album 256 (+1 dB) | +6,0 dB |
| D.MP3 | TXXX album −2,00 / piste −3,21 dB | −2,0 dB, puis −3,0 dB en mode piste |
| E.WAV | aucune | 0 dB (`RG none`) |

En enchaînement D → E, le gain passe à 0 dB 1,1 s après l'ouverture de E, au moment où E s'entend.
- **0.55** : `work/NEWFW055.IMG`, CRC `0xb0869078`.

## 36. Carte microSD : matériel et pilote (0.56, 2026-10-23)
**Choix de l'utilisateur** : la carte reste en exFAT ; l'accès USB à la carte n'est pas nécessaire dans les premières versions. Étapes : S1 rétro-ingénierie, S2 pilote, S3 lecture exFAT, S4 interface, S5 (plus tard) USB.

**S1, relevé dans le firmware d'origine** (fonctions de la pile SD du SDK, adaptées par FiiO) :
- `0x030603a6(port, …)` ouvre un port (0 = carte SD en `0x60020000`, 1 = eMMC en `0x60030000`). Il appelle `0x0306048c` (horloges, reset, broches), puis `0x03060308` (contrôleur), puis `0x03057b0c` (présence de la carte).
- `0x0306048c`, port 0 :
  - portes d'horloge 24 (`clk_sdmmc`, CLKGATE1 bit 8) et 107 (`hclk_sdmmc`, CLKGATE6 bit 11) ouvertes, reset logiciel 17 relâché ;
  - `CRU_CLKSEL7` (`+0x30`) = `0x009f009f` : source 24 MHz (bit 7), diviseur 32 pour démarrer ;
  - `GRF_GPIO1A_IOMUX` (`+0x20`) = `0xfc005400` et `GRF_GPIO1B_IOMUX` (`+0x24`) = `0x003f0015` : GPIO1A5 CMD, 1A6 CLK, 1A7 D0, 1B0 à 1B2 D1 à D3, toutes en fonction 1 (table d'iomux du TRM).
  - Pour comparaison, le port 1 (eMMC) met GPIO0A en fonction 1, GPIO0B0 à 0B2, et GPIO1B3 (`emmc_rstn`).
- `0x03060308` : CTRL = 7 (reset), FIFOTH `0x300f0010`, CTYPE 0, DEBNCE 25 ms à 24 MHz, TMOUT `0xffffff40`, INTMASK `0x283c`, puis CTRL = `0x10` (interruptions). Mêmes valeurs que pour l'eMMC.
- `0x0305f36c(port, marche)` : porte d'horloge (CLKGATE1 bit 8 pour la carte, bit 9 pour l'eMMC).
- `0x03057b0c` : lit GPIO0_11 ; 0 = carte présente. Confirme la ligne du §3.2 (le SDK de référence décrit la même broche, « GPIO0 port B pin 3 »).
- Aucune broche d'alimentation de la carte n'est pilotée : elle est alimentée en permanence.

**S2, pilote** :
- `mshc.c` : les commandes, les transferts par la FIFO et l'horloge de carte (`mshc_clock`, avec la commande « update clock »), tirés de `emmc.c` et paramétrés par l'adresse du contrôleur. `emmc.c` les utilise sans changement de comportement.
- `sd.c` :
  - mise en route comme le firmware d'origine, mais l'horloge du contrôleur vient du quartz 24 MHz sans division (`CRU_CLKSEL7` = `0x009f0080`) : elle ne dépend pas de la PLL, que le firmware change en pause ;
  - séquence SD 2.0 (spécification simplifiée de la couche physique, publique) : CMD0, CMD8 (`0x1aa`), ACMD41 jusqu'à « prête » (1 s au plus), CMD2, CMD3, CMD9 (taille par le CSD 1.0 ou 2.0), CMD7, ACMD6 (4 bits) ;
  - identification à 400 kHz (24 MHz / 60), puis 24 MHz sur 4 bits (vitesse par défaut, 12 Mo/s au plus) ;
  - lectures par CMD17/CMD18 avec CMD12 automatique, 64 secteurs par commande. Aucune écriture.
- Au démarrage (`sd_probe`, provisoire) : la carte est mise en route et son secteur 0 lu. Le journal note `sd: SDHC/SDXC card, N MiB, OCR …` et le type et la position de la première partition.
- Émulateur : `--sd IMAGE` ajoute une carte SDHC derrière le second contrôleur (identification et lectures). GPIO0_11 vaut 0 avec `--sd`, 1 sinon.
- Essai émulé (carte de 257 Mio, partition exFAT à 2048) : `sd: SDHC/SDXC card, 257 MiB, OCR c0ff8000`, puis `partition 1 type 7 at 2048, 524288 sectors`, en 4 ms. Le volume interne et le journal fonctionnent comme avant.
- **0.56** : `work/NEWFW056.IMG`, CRC `0xdb372e4c`. À essayer sur l'appareil : les deux lignes `sd:` du journal avec la carte de l'utilisateur (environ 244 016 Mio, partition à 65 536 selon RECHO).

**Essai de la 0.55 sur l'appareil (2026-10-23)** : 55 changements de gain en 19 min, tous conformes aux étiquettes (Ogg Vorbis −8,30 → −8,5 dB, Opus `REPLAYGAIN_TRACK_GAIN` −12,80 → −13,0 dB, −18,50 dB) ; modes album, piste et off essayés ; 0 dB sans étiquette. À l'écoute : niveaux constants, pas de bruit au changement. La bibliothèque de l'utilisateur ne porte que des gains de piste, sans gain d'album ni pic : le mode album y retombe sur le gain de piste.

**Essai de la 0.56 sur l'appareil (2026-10-23)** : `sd: SDHC/SDXC card, 244016 MiB, OCR c0ff8000, RCA aaaa`, puis `ready in 224 ms; sector 0: signature aa55, partition 1 type 7 at 65536, 499679232 sectors`. Mêmes valeurs que RECHO (§3.4). Les 224 ms viennent surtout de la boucle ACMD41 (mise sous tension de la carte) et retardent le démarrage : la mise en route devra se faire plus tard ou en arrière-plan (S4).

## 37. Lecture exFAT et second volume (0.57, 2026-10-23)
Étape S3 du §36. Écrit de zéro d'après la spécification exFAT publique de Microsoft (2019).

**`fat.c` à deux volumes** :
- `vol` (interne, eMMC, FAT32) et `sdvol` (carte) ; chaque volume connaît son périphérique (`emmc_read` ou `sd_read`) et son type. Le cache de secteur de FAT est propre à chaque volume.
- `struct fat_file` et `struct fat_dirent` portent leur volume (`FAT_VOL_INT`, `FAT_VOL_SD`) et l'indicateur exFAT « pas de chaîne FAT » (`contig`) : les clusters se suivent, la FAT n'est pas lue. `fat_pread`, `fat_read` et `fat_extent` suivent l'un ou l'autre.
- `struct fat_dir` (cluster, taille, volume, `contig`) désigne un dossier des deux volumes : `fat_list()`, `fat_dir_of()`, `fat_root()`. `fat_list_dir(cluster)` reste pour le volume interne.
- `fat_mount_sd()` : un volume au secteur 0 (sans table de partitions), sinon la première partition FAT32 (`0x0b`, `0x0c`) ou exFAT (`0x07`) du MBR.

**exFAT** :
- Secteur de démarrage : signature `EXFAT   `, secteurs de 512 octets, somme de contrôle des secteurs 0 à 10 (octets 106, 107 et 112 exclus) comparée au secteur 11. Décalages de la FAT et du tas de clusters, nombre de clusters, premier cluster de la racine ; la seconde FAT si elle est l'active.
- FAT à entrées de 32 bits ; fin de chaîne à partir de `0xfffffff7`.
- Dossiers : un ensemble par fichier (`0x85` attributs et dates, `0xc0` drapeaux, longueur du nom, premier cluster et tailles, `0xc1` 15 caractères UTF-16 chacun), suivi entrée par entrée à travers les secteurs et les clusters. Type sous `0x80` : entrée libre ; `0` : fin du dossier. Un sous-dossier contigu s'arrête à sa taille.
- Taille d'un fichier : sa longueur de données valides. Les fichiers de 4 Gio ou plus, cachés ou système, et les noms commençant par « . » sont écartés.
- Pas de nom 8.3 : `name` et `sname` reçoivent le début du nom long, coupé sur un caractère.

**Essais émulés** :
- `fw/tools/exfat_add.py` ajoute dossiers et fichiers à une image faite par `mkfs.exfat` (ensembles d'entrées avec leurs sommes de contrôle et hachages de noms, carte d'allocation, fichiers contigus ou fragmentés). `fsck.exfat -n` la déclare propre (4 dossiers, 66 fichiers).
- Variante de test (`make EXTRA=-DSD_WALK`) : chaque dossier lu jusqu'à la profondeur 3, chaque fichier lu par `fat_pread` et haché. Les quatre empreintes sont identiques à celles des fichiers d'origine : fichiers contigus, fragmenté (chaîne FAT), noms longs accentués et japonais, dossier de 61 entrées sur trois clusters, sous-dossier, fichier vide ; le fichier caché est écarté.
- Même essai sur une carte FAT32 (MBR `0x0c`, noms longs) : mêmes empreintes.
- Lecture sur le volume interne inchangée (Opus puis WAV, changement de fréquence).
- Au démarrage, la 0.57 monte la carte et note sa racine (12 entrées au plus) dans le journal : `sd: ready …; exFAT volume at …`, puis `sd:   nom/, taille, cluster`.
- **0.57** : `work/NEWFW057.IMG`, CRC `0x360c75d0`.

**Essai de la 0.57 sur l'appareil (2026-10-23)** : carte prête en 39 ms (carte déjà alimentée), volume exFAT à 65 536, 975 808 clusters de 512 secteurs (256 Kio), racine au cluster 4, montage en 5 ms ; les 13 dossiers de la racine lus en 1 ms avec leurs noms (dont un nom commençant par une espace), tous contigus.

## 38. La carte dans l'interface (0.58, 2026-10-23)
Étape S4 du §36.

- **Niveau supérieur** (« Storage ») : « Internal », puis « SD card » quand une carte est présente. `path[0]` est ce niveau (`VOL_TOP`), `path[1]` la racine d'un volume ; chaque niveau garde un `struct fat_dir` (cluster, taille, volume, contigu) au lieu d'un simple cluster. Sans état sauvegardé, le navigateur s'ouvre sur la racine interne ; BACK y mène au niveau supérieur.
- **Entrées** : les drapeaux de `struct entry` portent `F_SD`, `F_CONTIG` et `F_VOLROOT` à côté du codec (`F_CODEC`), sans agrandir les tableaux. Une piste se reconnaît par son cluster et son volume (`same_file`). `q_from_tree` garde des `struct fat_dir` (la file passe de 24 à 20 Kio de noms pour rester dans ses 32 Kio).
- **Mise en route de la carte** à la première entrée dans « SD card » ou à une reprise sur la carte (`sd_open` : `sd_init` puis `fat_mount_sd`). En cas d'échec, l'entrée devient « SD card (not readable) ». Le démarrage ne touche plus la carte autrement (`sd_probe` ne reste que dans la variante de test `SD_WALK`).
- **Insertion et retrait** (`sd_watch`) : GPIO0_11 lue toutes les 250 ms, un changement retenu après deux lectures égales. Au retrait : la carte est démontée ; si la file venait de la carte, la lecture s'arrête et la file est vidée ; si le navigateur était sur la carte, il revient au niveau supérieur, et l'écran du navigateur s'affiche.
- **Reprise** : état `NEWFWSTATE 3` ; `depth` porte le volume dans ses bits 8 à 15, les tableaux commencent au niveau supérieur. Un état 2 (avant 0.58) est relu comme celui du volume interne, décalé d'un niveau. Si la carte d'une reprise manque ou ne se monte pas, le navigateur s'ouvre au niveau supérieur.
- **Correction** : `copy_name` pouvait garder l'octet de tête d'un caractère UTF-8 coupé (« 03 Fragment\xc3 » dans le journal) ; la coupe se fait maintenant avant le caractère.
- **Émulateur** : `--sd-out MS` retire la carte à l'instant donné.

**Essais émulés** (carte exFAT de 128 Mio : « Album é » avec FLAC, Opus et un WAV fragmenté ; « Other ») :
- racine interne, BACK : « Storage » avec « Internal » et « SD card » ; entrée sur la carte : montage en 13 ms ; « Album é » ; lecture FLAC, puis saut vers l'Opus (48 kHz, tags et ReplayGain lus sur la carte), puis vers le WAV fragmenté (chaîne FAT) ;
- retrait de la carte pendant la lecture : `ui: card removed`, arrêt, navigateur au niveau supérieur ;
- redémarrage avec la carte : `ui: resume: card, depth 2`, la piste reprend à sa position ;
- état sauvegardé par la 0.57 : reprise sur le volume interne comme avant.
- **0.58** : `work/NEWFW058.IMG`, CRC `0x5be43d0e`.

**Essai de la 0.58 sur l'appareil (2026-10-23)** : carte montée en 15 ms à la première entrée ; 9 dossiers de 77 à 356 entrées lus en 13 à 41 ms ; une soixantaine de MP3 lus depuis la carte avec leurs tags et le ReplayGain ; PLAY maintenu sur « SD card » : 400 pistes (le maximum de la file) en 70 ms. Aucune erreur de lecture. Restent à essayer : l'enchaînement naturel, la reprise après extinction, le retrait de la carte.

**0.59 (2026-10-23), demande de l'utilisateur** : sur l'écran de lecture, l'indication ReplayGain est sur fond jaune (`RG_TRACK_BG`, texte noir) quand le gain appliqué est celui de la piste ; sur fond noir quand il vient de l'album ou qu'il n'y en a pas (`RG none`). `rg_src` retient la source ; le journal la note (`ui: replaygain album: track tag …`). Émulateur : FLAC à gain d'album sur fond noir, Ogg à gain de piste seul sur fond jaune, WAV sans étiquette sur fond noir. `work/NEWFW059.IMG`, CRC `0xdacbe244`.

## 39. Affichage sans scintillement (0.60, 2026-10-23)
**Constat de l'utilisateur** : des scintillements au rafraîchissement, surtout sur le fond jaune du ReplayGain (0.59).

**Causes** (le texte lui-même est écrit en une passe, fond et lettre ensemble ; le problème vient des zones écrites deux fois) :
- la ligne du bas, redessinée 4 fois par seconde : `lcd_text_clip` remplissait le reste de la ligne en noir jusqu'au bord, par-dessus le cadre jaune, aussitôt redessiné ;
- chaque touche sur l'écran de lecture demandait un dessin complet, qui commençait par `lcd_fill` de tout l'écran ; de même à chaque changement de piste ;
- l'heure, le pourcentage de batterie, le compteur d'entrées et l'en-tête du navigateur étaient effacés avant d'être réécrits ; la bande du bas du navigateur aussi, sous la barre de progression.

**Changements** (`ui.c`) :
- Règle : chaque pixel n'est écrit qu'une fois par dessin. Chaque bande remplace son contenu en recouvrant l'ancien ; un dessin complet peint aussi, une fois, les intervalles et marges entre les bandes (rien n'est effacé d'abord). Seul l'écran « nothing plays » est encore effacé en entier.
- `text_right()` : texte aligné à droite, la partie gauche de sa zone remplie de fond, sans effacement préalable (heure, batterie, compteur).
- Ligne du bas : texte, puis cadre ReplayGain, puis le reste de la ligne, de gauche à droite.
- Les touches de l'écran de lecture ne demandent plus qu'un dessin partiel (`quick`) : le titre en double taille n'est redessiné qu'à un changement de piste, et le défilement de l'artiste et de l'album n'est plus remis à zéro par une touche.
- Mesure (`draw_stats`) : toutes les minutes, le journal note pour chaque écran le nombre de dessins partiels et complets, leur durée moyenne et maximale : `ui: draw play screen: N partial, mean … us, max … us; N full, …`. Dans l'émulateur, l'écran n'a pas de coût d'écriture : seules les mesures sur l'appareil comptent pour décider d'un affichage par DMA (§39, point c proposé, non retenu pour l'instant).
- Émulateur : passage navigateur → lecture, recherche, saut de piste, retour au navigateur ; aucun reste de l'écran précédent (marges comprises).
- **0.60** : `work/NEWFW060.IMG`, CRC `0xf060560e`.

**Essais complets de la carte sur l'appareil (0.59, 2026-10-23), jugés concluants par l'utilisateur** :
- reprise sur la carte au démarrage : `ui: resume: card, depth 2`, carte montée en 15 ms ;
- enchaînement naturel depuis la carte : `queued behind 1070 ms of the last track`, puis `now hearing` 1,06 s plus tard, deux fois ; changements de fréquence entre albums Opus et Ogg de la carte ;
- pause de plus de 60 s sur une piste de la carte (`parked`), reprise ensuite ;
- retrait pendant la lecture : `ui: card removed`, arrêt immédiat, aucune erreur ; remise en place 5,6 s plus tard : `ui: card inserted`, montage en 230 ms à la première entrée (carte qui vient d'être alimentée), puis lecture normale.
La carte microSD (S1 à S4) est validée ; S5 (accès USB à la carte) reste pour plus tard.

**Essai de la 0.60 sur l'appareil (2026-10-24)** : plus de scintillement selon l'utilisateur. Mesures (`ui: draw`, 3 minutes) :
- écran de lecture : rafraîchissement partiel 3,8 à 3,9 ms en moyenne (4,3 ms au plus), 4 fois par seconde, soit environ 1,5 % du temps du cœur A ; dessin complet 10,7 à 11,5 ms (13,9 ms au plus) ;
- navigateur : partiel 1,3 à 2,5 ms en moyenne (12,3 ms au plus, changement de page) ; complet 12,9 à 15,0 ms (17,1 ms au plus).
Un écran complet est envoyé en moins de 20 ms, sous la durée d'une trame à 30 images/s : l'affichage par DMA (point c) n'apporterait pas de gain visible et n'est pas retenu.

## 40. Abandon des modules du firmware d'origine (0.61, 2026-10-24)
Demande de l'utilisateur : les modules de codec d'origine sont abandonnés au profit des codecs libres, et la page « Audio tests » n'est plus utile.
- Retirés : le lecteur des modules d'origine et leur protocole par la boîte aux lettres (`player_play`, `serve`, `wait_for`, descripteur de fichier) ; le chargement d'un module depuis une image d'origine sur le volume (`hifi_find_image`, `hifi_load`, `hifi_start` et son bloc de paramètres, `hifi_poll`, `hifi_dump`) ; les anneaux du DAC en HIGHRAM1, HIGHRAM0 et PMU-SRAM (`RING_*`, `select_ring`, `ring_check`) ; le générateur de son de test (table de sinus) ; la page « Audio tests » (essai du son, lectures de fichiers et enchaînements `CH*`/`TEST*`, lecture M4A par le module 75) et sa forme bloquante `player_play_own`.
- Il reste : nos codecs, l'anneau du DAC en SYSRAM (`audio_start(rate, bits16)`), la sonnette du cœur B (`hifi_ring`, seule écriture dans la boîte aux lettres). Pendant une rampe de volume sans lecteur, le tampon est rempli de silence (`fill_silence`).
- L'écran « System screen » n'affiche plus les touches de l'ancien écran principal : « any key: back ».
- Taille : code de 73,4 à 62,9 Kio ; `.bss` de 102,7 à 96,7 Kio (marge de 15,3 Kio).
- Non-régression (émulateur) : même scénario sur la 0.60 (recompilée depuis le dépôt) et la 0.61 (WAV, saut vers Opus 48 kHz, enchaînement vers Ogg 44,1 kHz, accord de recherche) : sortie du DAC identique octet pour octet (9,4 Mo), mêmes événements dans le journal.
- **0.61** : `work/NEWFW061.IMG`, CRC `0x27e8263c`.

## 41. Contrôle qualité : code mort, doublons, dépôt autonome (0.62, 2026-10-24)
Points 1, 2 et 4 de la passe de contrôle demandée par l'utilisateur ; le découpage de `ui.c` (point 3) reste à faire.
- **Code mort retiré** : `audio_dma_err()` ; `audio_i2s_fifo()` devenue interne ; `lcd_char()` (`lcd_text()` passe maintenant par `utf8_next()` comme les autres fonctions de texte) ; `audio.external`, jamais mis à 1 ; les champs `buffers`, `seeks`, `wait_us_max` et `b_out` de `struct player_state`, écrits mais jamais lus ; l'état `ST_DRAIN` ; `hifi_code_size`. Le code de test de la carte (`SD_WALK`) quitte `main.c` pour `sdtest.c`, compilé seulement avec `make EXTRA=-DSD_WALK`.
- **Doublons supprimés** : `dir_add()` sert à `collect()` et `top_entry()` ; `q_add()` à `q_from_dir()` et `q_from_tree()` (la piste choisie n'est plus indiquée si le pool de noms est plein avant elle) ; `text.h` regroupe `le16()`, `le32()` et `utf8_put()`, utilisés par `fat.c` et `tags.c` ; une seule table de noms de touches (`key_name()`).
- **Dépôt autonome** : `fw/tools/rkcrc.py` remplace l'import de `rknanofw.py`, extérieur au dépôt (`mkimg.py`) ; la vérification des modules d'origine de `emu_coreb.py` est retirée ; `mkdisk.py` prend le dump de la zone de démarrage par `--boot` ou `$ECHO_BOOTDUMP` (plus de chemin propre à la machine) ; `fw/README.md` décrit l'état actuel. Une copie des seuls fichiers du dépôt se compile sans rien d'autre et donne la même image.
- Taille : code 62,8 Kio ; `.bss` 96,7 Kio.
- **Non-régression** (émulateur) : même scénario que pour la 0.61 (WAV, saut vers Opus 48 kHz, enchaînement vers Ogg 44,1 kHz, accord) : sortie du DAC identique octet pour octet, mêmes événements ; variante `SD_WALK` sur l'image exFAT de test : les mêmes empreintes de fichiers qu'au §37 ; navigation sur la carte avec noms accentués.
- **0.62** : `work/NEWFW062.IMG`, CRC `0xcbe53e80`.

## 42. Découpage de l'interface, événements des touches et des prises (0.63, 2026-10-24)
Point 3 du contrôle qualité, avec la suggestion de l'utilisateur : un module qui transforme les touches et leurs combinaisons en événements simples pour les écrans ; puis, à sa demande, les prises (casque, câble USB) dans la même liste d'événements.

**`keys.c` / `keys.h`** : le lissage de l'échelle (`key_scan`, venu de `main.c`) et la fabrique d'événements (venue de `ui.c`). `key_poll(struct key_ev *)` rend `{touche, type}` : `EV_PRESS` (au relâchement), `EV_LONG` (600 ms), `EV_REPEAT` (VOL+/VOL− tenues), `EV_CHORD` (VOL+ et VOL−), et maintenant `EV_PLUG` / `EV_UNPLUG` pour `SRC_JACK35` (GPIO0_13), `SRC_JACK44` (GPIO2_8) et `SRC_USB` (VBUS). Une prise change d'état après 100 ms stables (lecture toutes les 10 ms) ; la première lecture fixe l'état sans événement ; le journal note `keys: jack 3.5 mm in/out`, `keys: usb cable in/out`. Aucune action n'y est encore attachée (pistes : pause au débranchement du casque, écran de montage USB de l'eMMC ou de la carte). `key_edge()` sert les écrans simples (système, installation).

**`ui.c` découpé** (1 751 lignes en huit fichiers, état partagé dans `ui_priv.h`) :

| Fichier | Contenu |
|---|---|
| `ui.c` | la boucle : un événement va à l'écran affiché, le menu principal, le dessin, l'économie d'énergie |
| `browser.c` | niveau supérieur, listes de dossiers et tri, chemin, carte (mise en route, détection), écran et touches du navigateur (`browser_key`) |
| `queue.c` | file de lecture et modes |
| `playback.c` | ouverture des pistes, tags, ReplayGain, enchaînement, sauts, arrêt, rangement en pause, volume, recherche |
| `playscreen.c` | écran de lecture, ses touches (`play_key`) et celles de la recherche (`seek_key`) |
| `menu.c` | menu principal, date et heure |
| `resume.c` | état de reprise |
| `uidraw.c` | aides au dessin (temps, barre, texte à droite, heure) |

Chaque écran a sa fonction de touches, qui rend à la boucle ce qu'elle doit faire (`UI_FULL`, `UI_QUICK`, `UI_TO_PLAYING`) ; la longue chaîne de conditions de `ui_main` disparaît.

**Non-régression** (émulateur), la 0.62 recompilée avec le même numéro de version :
- scénario complet (navigateur, carte, lecture, sauts, recherche, volume, menu principal, retour) : mêmes événements dans le journal, aux millisecondes près ; 13 captures d'écran sur 14 identiques au pixel près (la 14e ne diffère que d'un pixel de barre de progression) ;
- scénario du §40 : même audio ; le saut arrive 0,8 ms plus tard (la boucle n'a plus exactement le même temps de parcours), le reste est identique octet pour octet ;
- prise 3,5 mm simulée (`emu_fw.py --gpio-at MS:BANC:BROCHE:NIVEAU`) : `keys: jack 3.5 mm in` puis `out`.
- **0.63** : `work/NEWFW063.IMG`, CRC `0x9f6c2701`.

## 43. Batterie faible (0.64, 2026-10-24)
Priorité de sécurité du `ROADMAP.md`. Avant la 0.64, aucune action : l'appareil tournait jusqu'à la coupure par la batterie, éventuellement pendant une écriture.

**Choix validés par l'utilisateur** : seuils du firmware d'origine et de RECHO ; moyenne sur 10 s ; bandeau de 3 s et, pendant la lecture, trois bips mêlés à la musique, puis un bip toutes les 5 min ; 30 s sous 3 300 mV avant l'extinction, annoncée par deux bips graves ; une annonce vocale plus tard (`ROADMAP.md`).

**Fonctionnement** :
- `battery_service()` (`ui.c`), appelée par la boucle de l'interface seulement, donc jamais au milieu d'une écriture (journal, état de reprise, installation, disque USB) : une lecture par seconde sans câble USB, décision sur la moyenne des 10 dernières.
  - Sous 3 400 mV : `battery: low (N mV)`, bandeau plein écran « Low battery / plug the USB cable » pendant 3 s (l'écran s'allume), pourcentage en rouge sur l'écran de lecture ; pendant la lecture, trois bips de 1 kHz (120 ms, séparés de 120 ms), puis un bip toutes les 5 min ; si rien ne joue, les trois bips sont donnés à la reprise de la lecture. Réarmé au-dessus de 3 500 mV.
  - Sous 3 300 mV pendant 30 s : `battery: empty`, deux bips de 500 Hz (200 ms), attente de 1,8 s (le tampon du DAC les joue), pause avec rampe, « Battery empty » 2 s, puis `sys_power_off("battery empty")` : état de reprise enregistré en pause à la position atteinte, journal vidé, alimentation coupée.
- Au démarrage (`battery_check_boot`, `main.c`) : sous 3 350 mV sans câble (moyenne de 8 lectures), « Battery empty / plug the USB cable » 2 s, une ligne de journal et extinction, sans passer par `sys_power_off()`, qui aurait écrasé l'état de reprise enregistré avant le chargement (défaut trouvé à l'essai émulé).
- Bips (`audio_beep()`, `audio.c`) : générés par le cœur A au moment où il recopie le PCM dans le tampon du DAC ; la musique est abaissée de 12 dB pendant le motif et une sinusoïde à −12 dBFS est ajoutée (fondus de 5 ms), en trames de 16 ou de 32 bits ; le volume du DAC (ReplayGain compris) s'applique aux deux. Un saut ou un redémarrage de la sortie abandonne un bip en attente.

**Essais émulés** (`emu_fw.py --adc0-at MS:VALEUR` fait varier la lecture de la batterie ; variante de test avec 5 s au lieu de 30 et un rappel à 12 s, `make EXTRA="-DBAT_OFF_MS=5000u -DBAT_REMIND_MS=12000u"`) :
- 4 003 mV puis 3 378 mV : `battery: low` 9 s plus tard (moyenne), bandeau, puis dans la sortie du DAC trois bips de 1 kHz de 100 ms environ, et le rappel 12 s après ;
- 3 291 mV : `battery: empty` une fois la moyenne sous 3 300 mV et le délai écoulé, deux bips de 500 Hz dans la sortie du DAC, `audio: paused`, `power off (battery empty), battery 3291 mV` (l'émulateur ne coupe pas l'alimentation : le firmware note `still powered (USB)` et continue) ;
- 3 334 mV au démarrage : message, `power off (battery empty at start)`, et l'état de reprise reste intact ;
- batterie normale : la lecture du §40 est inchangée (le saut tombe quelques millisecondes plus tôt, la boucle ayant un peu plus de travail).
- **0.64** : `work/NEWFW064.IMG`, CRC `0x8239e945`.

## 44. Fichiers vides ou abîmés, message d'échec (0.65, 2026-10-24)
**Constat sur l'appareil** (journal de la nuit en 0.61, puis essais en 0.64) : sur la carte, des pistes de plus de 208 s s'arrêtaient à 208 s, puis à 103 s, et le fichier était lu jusqu'au bout sans son. La cause est dans les fichiers eux-mêmes, vérifiés sur le PC en lecture seule. Dans le dossier « Redacted top 2025 », 61 fichiers ne contiennent que des zéros après 8 Mio, 44 après 4 Mio, et 53 sont vides (0 octet). Cela ressemble à une copie interrompue, ou à une carte retirée avant la fin de l'écriture. Le firmware lisait correctement. Il n'avait en revanche aucune réponse adaptée : silence pendant toute la lecture des zéros, puis arrêt de la file sur les fichiers vides. En 0.64, l'écran de lecture restait ensuite figé, sans message, sur un album dont tous les MP3 étaient vides.

**Choix validés par l'utilisateur** : les trois propositions et le texte du message.
- **Message d'échec** : quand rien ne peut être joué, un bandeau de 3 s s'affiche, « Cannot play: », suivi du nom du fichier et de la raison, puis le navigateur. Les raisons possibles sont :
  - `empty file (0 bytes)` ;
  - `damaged file (no audio data)` ;
  - `unreadable file (format)` ;
  - `read error` ;
  - `no codec for it in the firmware` ;
  - `core B did not answer`.

  `play_failed()` (`playback.c`) note le nom et la raison ; la boucle (`ui.c`) affiche le bandeau. Il apparaît quand `play_index` ne trouve aucune piste à ouvrir, et quand la file s'arrête sur un échec pendant l'enchaînement. Une piste ratée suivie d'une piste lue ne donne pas de bandeau.
- **Fichiers vides** : ils apparaissent en gris dans le navigateur. Ils ne sont mis ni dans la file d'un dossier ni dans celle d'une arborescence. PLAY sur l'un d'eux donne le bandeau et laisse la file en cours intacte. Si le fichier de départ est vide, la file part du suivant.
- **Fichier abîmé** (`player_service`, `player.c`) : 5 s sans aucune trame décodée, alors que le cœur B a lu plus de 1 Mio d'entrée, provoquent l'erreur `PLAYER_ENOAUDIO`. Le journal note « damaged file », puis l'enchaînement passe à la piste suivante. La condition sur l'entrée évite les faux positifs en pause et pendant l'attente d'un changement de fréquence, quand le cœur B ne lit plus. Un saut dans l'en-tête (ID3) est instantané, et des trames suivent aussitôt.
- **Compteur d'échecs** : `q_errors` revient à zéro quand une piste s'est terminée normalement, ou a joué plus de 10 s. Seuls 9 échecs de suite arrêtent la file. Avant, le compteur s'accumulait sur toute la file.

**Essais émulés** :
- **File** : `1A` normal, `2E` vide, `3Z` (300 Ko de MP3 puis 8 Mio de zéros), `4B` normal. La file compte 3 pistes. `3Z` joue 7,4 s, puis `no audio decoded for 5014 ms over 6029312 bytes of input: damaged file`, et `4B` est joué.
- **PLAY sur `2E`** : bandeau « empty file (0 bytes) », puis le navigateur, où `2E` est en gris.
- **PLAY sur `5G`** (octets aléatoires) : bandeau « unreadable file (format) ».

**Limite** : un Ogg dont l'en-tête de commentaires est énorme (pochette de plus de 10 Mo) et qui serait lu sans l'indication de `tags.c` pourrait être pris pour un fichier abîmé.

- **0.65** : `work/NEWFW065.IMG`, CRC `0x8cda5d10`.

## 45. Carte réinitialisée après le mode USB (0.66, 2026-10-24)
**Constat** (journal 0.64, 12:57) : en sortant d'une session USB de 814 s, « SD card » affichait un dossier vide (`folder sd:0: 0 entries`, deux fois). Il a fallu retirer puis remettre la carte. Pendant les écrans bloquants (disque USB, installation, écran système, chargeur), `sd_watch` ne tourne plus. La carte avait été sortie puis remise entre-temps, et le firmware gardait le montage de la précédente : la nouvelle carte n'était pas initialisée et les lectures échouaient. `dir_load` donnait alors une liste vide, sans message.

**Correction validée par l'utilisateur** :
- **`sd_forget()`** (`browser.c`) : le montage et l'état du pilote sont oubliés après chacun de ces écrans (`menu_open`, `ui.c`). Le journal note `ui: card to be set up again (after the ...)`, et la carte est réinitialisée au prochain accès, en 240 ms environ sur l'appareil. `sd_watch` utilise la même fonction.
- **Erreur de lecture d'un dossier de la carte** : le journal note `ui: card read failed`, la carte est oubliée et marquée illisible, puis le navigateur revient au niveau du haut. On y voit « SD card (not readable) », et une nouvelle tentative a lieu à l'entrée suivante.
- **Pistes de la carte** : `q_open` remonte d'abord la carte si elle a été oubliée. Si le montage échoue, le bandeau affiche « read error ».

**Essai émulé** : carte exFAT de test montée, puis menu → System → retour. Le journal montre `card to be set up again (after the system screen)`. À l'entrée suivante dans « SD card », la carte est réinitialisée et montée, avec ses 2 entrées. Le cas de l'erreur de lecture n'est pas simulé : l'émulateur ne sait pas faire échouer une lecture de la carte.

- **0.66** : `work/NEWFW066.IMG`, CRC `0x2f75c369`.

## 46. Fichiers abîmés : essais sur l'appareil et corrections (0.67, 2026-10-24)
**Base de test** : `work/damage.py` crée, à partir des fichiers `CH*` (3 s chacun), des copies abîmées de chaque format (WAV, FLAC, MP3, Ogg Vorbis, Opus). Les originaux restent à côté. Les octets aléatoires viennent d'une graine fixe, donc le script recrée toujours les mêmes fichiers. Les préfixes :
- `E_` : fichier vide ;
- `T_` : coupé à la moitié ;
- `H_` : 4 premiers Kio aléatoires ;
- `M_` : 16 Kio aléatoires au milieu ;
- `Z_` : fichier complet suivi de 3 Mio de zéros ;
- `R_` : fichier complet suivi de 3 Mio d'octets aléatoires ;
- `ZZ_` : 3 Mio de zéros seulement ;
- `RR_` : 3 Mio d'octets aléatoires seulement.

**Essai sur l'appareil en 0.66** (journal `work/hwlog_1024_066/`) : la file de 46 pistes va jusqu'au bout, sans blocage, et se termine sur le bandeau « Cannot play ». La détection des fichiers abîmés (§44) se déclenche sur `R_CH7.OGG` et `Z_CH7.OGG`. MP3 et Opus se resynchronisent après une zone abîmée. L'essai a aussi révélé cinq défauts :
1. BACK vers une piste qui ne s'ouvre pas : `play_index` passait à la suivante, donc on revenait sur la piste de départ, et il était impossible de reculer au-delà.
2. FLAC abîmé au milieu : arrêt à la zone abîmée. dr_flac rend 0 trame sur une erreur de décodage qui n'est pas une erreur de CRC, et le codec prenait cela pour la fin du flux.
3. Vorbis suivi de zéros ou d'octets aléatoires : il ne s'arrêtait pas à la page de fin (drapeau 4), d'où 5 s de silence avant la détection. L'Opus, lui, s'arrêtait déjà.
4. Ogg ou Opus sans aucune page : 3 s avant le refus (expiration de l'ouverture), car le cœur B cherchait « OggS » octet par octet dans tout le fichier.
5. FLAC suivi de zéros : tout le reste était lu, d'où un trou d'environ 1 s avant la piste suivante.

**Corrections validées par l'utilisateur** :
1. `play_step(i, dir)` (`playback.c`) : une piste qui ne s'ouvre pas est sautée dans le sens demandé, en arrière pour BACK (`playscreen.c`). `play_index(i)` correspond à `play_step(i, 1)`.
2. `codec_flac.c` : quand aucune trame n'est rendue et que l'entrée n'est pas finie, le codec rend « du travail fait ». L'appel suivant cherche alors le prochain code de synchronisation de trame.
3. `codec_vorbis.c` : après le paquet de la dernière page (`e_o_s`), plus rien n'est lu.
4. `oggpage.c` : sans aucune page dans les 64 premiers Kio (`PG_FIRST_MAX`), l'entrée n'est pas de l'Ogg.
5. `codec_flac.c` : le flux s'arrête au nombre de trames de STREAMINFO. La demande à dr_flac est bornée aux trames restantes : sans cette borne, dr_flac cherchait la trame suivante dans les zéros, dans le même appel.

**Essais émulés** (10 fichiers de la base, sur la mémoire interne) :
- `M_CH3.FLAC` : 123 084 trames sur 132 300 (64 512 avant) ;
- `Z_CH3.FLAC` : 425 984 octets lus au lieu de 3,5 Mo, sans sous-alimentation de la sortie ;
- `R_CH7.OGG` et `Z_CH7.OGG` : fin propre après 98 304 octets, sans attente ;
- `RR_RANDOM.OGG` et `ZZ_ZEROS.OPUS` : refus en 160 ms (3 s avant) ;
- BACK sur `T_CH3.FLAC` : `RR_RANDOM.OGG` échoue, puis `R_CH7.OGG` est joué.

**Limites** :
- Un Ogg suivi de données inutiles affiche une durée de 0 ms : la recherche de la dernière page, côté cœur A, ne lit que la fin du fichier.
- Le MP3 parcourt encore tout un fichier sans trame valide avant de le refuser (1,6 s pour 3 Mio).

**Essai sur l'appareil en 0.67** (journal `work/hwlog_1024_067b/`, même dossier de 46 pistes) : les résultats sont ceux de l'émulateur.
- `M_CH3.FLAC` : 123 084 trames ;
- `R_CH3.FLAC` et `Z_CH3.FLAC` : 425 984 octets lus, sans sous-alimentation de la sortie ;
- `R_CH7.OGG` et `Z_CH7.OGG` : fin après 98 304 octets, sans « damaged file » ;
- Ogg et Opus sans contenu : refus en 150 ms (3 s en 0.66) ;
- la file entière dure 89 s au lieu de 107 s.

Le seul trou restant vient du MP3 suivi de données inutiles : 116 772 octets de sortie manquants (environ 0,66 s), le temps de parcourir les 3 Mio.

**Série A** (journal `work/hwlog_1024_067c/`) :
- **BACK** : depuis `T_CH1.WAV`, les cinq `RR_RANDOM.*` échouent l'un après l'autre en reculant, puis `R_CH8.OPUS` est joué. Depuis `M_CH1.WAV`, `H_CH8` et `H_CH7` échouent, puis `H_CH5.MP3` est joué. Depuis `H_CH5.MP3`, `H_CH3` et `H_CH1` échouent, puis `CHA.FLAC` est joué.
- **PLAY sur les `E_*`** : bandeau « empty file (0 bytes) » pour chacun.
- **Non-régression** :
  - FLAC et Ogg de la carte : avance rapide correcte (Arkona en FLAC et en Ogg, Claws) ;
  - « Road to Burn.flac » (528 s) joué jusqu'au bout, puis enchaîné ;
  - `CHA.FLAC` (24 bits, 96 kHz) joué entier ;
  - `OPEN_aotuv`, `OPEN_bib` et `OPEN_glow` (pochettes) et leurs versions `_nocov` : ouverts et joués jusqu'au bout.

  Le patch 4 n'a donc pas d'effet sur les Ogg à grosse pochette.
- Une avance rapide loin dans un Ogg de la carte (Claws, de 15 à 298 s) donne environ 0,8 s de silence. C'était déjà le cas en 0.66 (« Loose »).
- Sur la carte, le dossier d'Arkona contient un FLAC vide, « 01 Izrechenie. Nachalo.flac » : c'est le fichier, pas le firmware.

- **0.67** : `work/NEWFW067.IMG`, CRC `0x2bf2016f`.

## 47. Essais sur l'appareil : carte après le mode USB, prises, branchement appareil éteint (0.67, 2026-10-24)
Journal `work/hwlog_1024_067d/`. L'utilisateur confirme que tous les essais de la série B passent.

- **Carte après le mode USB** : six sessions USB, en lecture et écriture ou en lecture seule, quittées par l'éjection ou par BACK. Après chacune, le journal note `card to be set up again (after the USB session)`, puis la carte est remontée au premier accès, en 236 à 251 ms, ou en 43-44 ms quand la carte était restée prête. Le contenu est toujours correct.
- **Carte retirée puis remise pendant le mode USB** : détectée à la sortie (`card removed`, puis `card inserted`), et contenu correct à l'entrée suivante.
- **Carte retirée pendant la lecture d'une de ses pistes** (« Road to Burn ») : la lecture s'arrête aussitôt, sans blocage. Pendant la remise, le journal montre plusieurs `card removed` et `card inserted` toutes les 0,5 s, le temps que la carte soit bien en place. Ensuite, la carte est montée en 251 ms et la piste est relancée.
- **Prise 3,5 mm** : branchements et débranchements journalisés (`jack 3.5 mm in` / `out`), sans effet sur la lecture. La prise 4,4 mm n'a pas été essayée.
- **Câble USB branché sur l'appareil éteint** : l'appareil démarre normalement, avec reprise sur la carte, et se recharge. Il n'y a pas d'écran de charge à part. Le branchement du câble suffit à l'allumer.

## 48. Écran de charge (0.68, 2026-10-25)
**Constat** (§47) : brancher le câble USB sur l'appareil éteint suffit à l'allumer, et le firmware démarrait alors normalement, avec la reprise de la lecture.

**Choix validés par l'utilisateur** : toutes les propositions, avec une seule durée d'extinction de l'écran, 30 s (`SCREEN_TIMEOUT_US`, déplacé de `ui.c` dans `sys.h`).
- **Quand il apparaît** (`charge.c`) :
  - au démarrage, si le câble est branché et que la touche Power n'est pas enfoncée, c'est le câble qui a allumé l'appareil. Le journal de démarrage note maintenant l'état de la touche (`power key`) ;
  - après Power 2 s avec le câble branché, l'écran de charge remplace « power off: unplug the USB cable ». L'état de reprise est enregistré avant, comme pour une extinction, et la lecture ainsi que le cœur B sont arrêtés.
- **L'écran** :
  - une batterie de 10 segments, remplie selon la tension (courbe Li-ion commune avec la barre d'état, `battery_pct()` dans `board.c`, moyenne glissante), dont le dernier segment clignote pendant la charge ;
  - dessous, « Charging NN % » tant que le chargeur tire sa broche au niveau bas, puis « Charged 100 % » ;
  - une ligne d'aide : « PLAY or Power held: start ».

  Le passage à « Charged » vient du chargeur et reste donc fiable. La tension, elle, est un peu gonflée pendant la charge (environ +40 mV).
- **Touches** :
  - un appui court sur n'importe quelle touche, Power comprise, rallume l'écran ;
  - PLAY tenu (0,6 s) ou Power tenu (1 s) donne le démarrage normal.

  Depuis Power 2 s, le démarrage normal passe par un redémarrage. Un mot gardé dans la mémoire PMU-SRAM (`log_boot_note()`, en-tête de l'anneau du journal, nouvelle signature « NLG2 ») évite alors de revenir sur l'écran de charge, PLAY pouvant déjà être relâché.
- **Câble débranché** pendant 0,5 s : extinction.
- **Écran éteint** au bout de 30 s : l'horloge passe sur le quartz, et `sys_screen_set(1)` remet la PLL.
- **Journal** : `charge: screen (...)`, `charge: charged`, `charge: start (PLAY held)` ou `(Power held)`, `charge: power off (cable out)`.

**Essais émulés** (variante `make EXTRA=-DSCREEN_TIMEOUT_US=5000000u` : écran éteint au bout de 5 s) :
- **Allumage par le câble** :
  - écran de charge à 80 % (captures) ;
  - écran éteint à 6,0 s, rallumé par PLAY à 9,3 s ;
  - broche du chargeur au niveau haut à 12 s : « Charged 100 % » ;
  - PLAY tenu à 15,6 s : navigateur.
- **Allumage par Power, câble branché** : démarrage normal.
- **Power 2 s avec le câble branché** : `power off (Power 2 s)`, puis `still powered (USB)`, puis l'écran de charge ; PLAY tenu donne `charge: start`.

L'émulateur ne modélise ni le redémarrage qui suit, ni le débranchement du câble : ces deux points sont à vérifier sur l'appareil.

- **0.68** : `work/NEWFW068.IMG`, CRC `0x1c7438de`.

**Essai sur l'appareil en 0.68** (journal `work/hwlog_1025_068/`) :
- Fonctionnent :
  - allumage par le câble (5 fois, `power key 0`) ;
  - écran éteint au bout de 30 s, puis rallumé par PLAY ;
  - « charged » à la fin de la charge ;
  - depuis l'écran de charge ouvert au démarrage, PLAY tenu et Power tenu donnent le démarrage normal ;
  - câble débranché : extinction en 0,4 s ;
  - allumage par Power, câble branché (`power key 1`) : démarrage normal.
- **Défaut, après Power 2 s avec le câble branché** : `power_off()` avait mis à 0 le rétroéclairage et le maintien d'alimentation (`PIN_POWER_HOLD`). L'écran restait noir, et `sys_screen_set(1)` ne le rallumait pas, puisqu'il le croyait allumé. Un appui sur Power relançait l'alimentation, d'où un redémarrage complet sans `charge: start`, puis, Power étant relâché, l'écran de charge comme après un allumage par le câble.

**Correction (0.69, validée par l'utilisateur)** : à l'entrée de l'écran de charge par Power 2 s, `PIN_POWER_HOLD` est remis à 1 et le rétroéclairage rallumé. Le débranchement du câble repasse par `power_off()`. Dans l'émulateur, `emu_fw.py` affiche maintenant en fin de course la sortie de GPIO2 : `0x50` sur l'écran de charge, maintien et rétroéclairage à 1.

**Essai sur l'appareil en 0.69** (journal `work/hwlog_1025_069b/`) : trois passages par Power 2 s avec le câble branché, tous corrects.
- **Sortie par PLAY tenu** : redémarrage, puis démarrage normal avec `power key 0`. Le mot gardé en PMU-SRAM a bien évité l'écran de charge.
- **Sortie par Power tenu** (deux fois) : redémarrage, puis démarrage normal.
- **Allumage par le câble** : l'écran de charge apparaît, et Power tenu donne le démarrage normal.

L'écran de charge est validé.

- **0.69** : `work/NEWFW069.IMG`, CRC `0x0502d1d0`.

## 49. MP3 : l'assembleur de PacketVideo en Thumb-2 (0.70, 2026-10-25)
**Contexte** : notre décodeur MP3 (PacketVideo, en C) consomme environ 50 MHz du cœur B en 320 kb/s, contre environ 31 MHz pour le module d'origine, qui utilisait les accélérateurs IMDCT36 et SYNTH. L'évaluation de ces accélérateurs (registres non documentés, formats à déterminer sur l'appareil, gain modeste) a conduit à essayer d'abord l'optimisation en assembleur, à la demande de l'utilisateur.

**Banc** : `re/mp3/bench/`, local, sur le même principe que les bancs Vorbis et Opus : QEMU `mps2-an385`, `-icount shift=0`, `PROF=1` pour un profil par fonction. Les fonctions `mem*` sont celles de `fw/hifi/rt.c` (`mkrtmem.sh`), pour que le profil corresponde au firmware. Les mesures portent sur 20 s d'un MP3 à 320 kb/s (`work/long/L.mp3`).

**Profil de la version C** :

| Fonction | Part |
|---|---|
| `pvmp3_polyphase_filter_window` | 31,1 % |
| `pvmp3_mdct_18` | 12,5 % |
| `pvmp3_dct_16` | 6,8 % |
| `pvmp3_dct_9` | 6,3 % |
| déquantification | 5,0 % |
| décodage de Huffman et lecture des bits | environ 10 % |

Les quatre premières fonctions font 57 % du temps, et l'AOSP fournit pour chacune une version en assembleur ARM (`third_party/pvmp3dec/src/asm/*_gcc.s`). Celles-ci sont en mode ARM, que le Cortex-M3 n'exécute pas. Elles n'utilisent cependant que des instructions de base et `smull` / `smlal`, sans DSP ni `smmul`.

**Portage** (`fw/hifi/mp3asm/*_thumb.S`, Apache-2.0, en-tête conservé, changements décrits dans le fichier) :
- syntaxe unifiée et fonctions Thumb ;
- `mov rd, rm, lsl rs` devient `lsl rd, rm, rs`, le décalage par registre n'existant pas comme opérande en Thumb-2 ;
- blocs IT ajoutés par l'assembleur (`-mimplicit-it=always`).

Le calcul est inchangé. Le C est compilé avec `-DPV_ARM_GCC_V5`, qui retire les versions C de ces quatre fonctions et prend les opérations en virgule fixe ARM de PacketVideo. `third_party/` reste intact.

**Résultats sur le banc** (millions d'instructions par seconde d'audio, 320 kb/s) :

| Version | M instr/s | Écart avec la version C | Écart avec ffmpeg |
|---|---|---|---|
| C | 15,96 | — | 88,3 dB, 1 bit de poids faible au plus |
| Thumb-2 | 14,69 (−8 %) | 87,1 dB, 1 bit au plus, 90,7 % d'échantillons identiques | **90,5 dB**, 1 bit au plus |

En 128 kb/s, la version C fait 16,0 M instr/s.

Le gain en instructions est faible. La synthèse polyphase approche son plancher : 16 multiplications-accumulations et deux chargements par échantillon de sortie, soit environ 4,2 M instr/s en stéréo à 44,1 kHz. GCC en `-O2` produisait déjà un code proche. Le gain en cycles peut toutefois différer, sur un cœur B qui fait environ 2,8 cycles par instruction (code en HIGHRAM, accès multi-cycles). Seule la mesure sur l'appareil le dira.

**Émulateur** : le firmware complet décode `l320s` (25 s) sans erreur. La sortie du DAC est identique à celle du banc : 90,5 dB par rapport à ffmpeg, 1 bit au plus.

**Pistes suivantes, selon la mesure** :
- `memset` par mots dans `rt.c` (1,3 %) ;
- l'égaliseur de PacketVideo, appelé même à plat (1,4 %) ;
- l'accès mono-cycle à la HIGHRAM quand le cœur B tourne lentement : le réglage multi-cycles n'était nécessaire qu'à 200 MHz. Ce point concernerait tous les codecs, et c'est sans doute le gain le plus important, à vérifier.

- **0.70** : `work/NEWFW070.IMG`, CRC `0xe33cf19b`.

**Essai sur l'appareil en 0.70** (journal `work/hwlog_1025_070/`) : le son des MP3 est haché et inintelligible. Le cœur B ne fournit que 12 MHz environ, avec 96 coupures du DAC en 21 s, et le dessin de l'écran prend 20 ms au lieu de 4 ms. La cause n'est pas le codec : ce démarrage était en **repli à 24 MHz** (`clock: 24 MHz (fallback: 24 MHz)`), voir ci-dessous.

**Essai sur l'appareil en 0.70, à 200 MHz** (après un redémarrage, journal `work/hwlog_1025_070b/`) : le son est normal. `MP3BENCH.MP3` (copie de `work/long/L.mp3`, 239 s, 320 kb/s) est joué en entier sans manque, avec une **charge moyenne de 49,7 MHz** et un palier de 67 MHz (232 s sur 239). Les albums MP3 en 320 kb/s de Daft Punk donnent 49,5 et 43,8 MHz.

Ces valeurs ne se distinguent pas des mesures précédentes en C : environ 50 MHz (« 04 Within », 0.64), 51,4 MHz (TEST1.MP3, 0.36). Les 8 % d'instructions en moins ne se voient pas sur la charge réelle, qui reste dominée par les accès à la mémoire. Il manque toutefois une comparaison exacte, `MP3BENCH.MP3` n'ayant pas été joué en 0.69.

## 50. Repli à 24 MHz après une installation rapide (0.71, 2026-10-25)
**Constat** : la 0.69 a été démarrée, l'installeur ouvert 4,6 s plus tard, puis la 0.70 installée.
- Le repère de la tentative à 200 MHz (PMU-SRAM, `clock_init`) n'est effacé qu'au bout de 5 s passées dans `sys_idle()`.
- La boucle de l'installeur (`firmware_menu`) n'appelle que `wdt_kick()`, donc le repère était toujours posé au redémarrage qui suit l'installation.
- Le démarrage suivant l'a pris pour un échec à 200 MHz et est resté sur le quartz.

Le repli s'efface tout seul après 5 s, et le redémarrage suivant revient à 200 MHz. Le défaut existe depuis la 0.23. Il n'apparaît que si l'on installe moins de 5 s après le démarrage.

**Correction validée par l'utilisateur** : `clock_fast_ok()` juste avant les deux redémarrages volontaires, après une installation réussie et en sortie de l'écran de charge ouvert par Power 2 s. Ces redémarrages prouvent que le fonctionnement à 200 MHz était stable. Le redémarrage qui suit une faute garde le repère, car c'est le cas que la sécurité doit couvrir.

- **0.71** : `work/NEWFW071.IMG`, CRC `0xd38df64d`.

## 51. Cycle d'attente des mémoires du cœur B : essai A/B (étape 1, 2026-10-25)
**Le réglage** (TRM chap. 12 et `GRF_INTER_CON0`) : les SRAM internes répondent en un cycle jusqu'à environ 150 MHz. Au-delà, il faut activer « one more response cycle », par mémoire : bits 12 et 13 pour le cœur A ; 14 `caliram` (HIGHRAM0, code et tables des codecs) et 15 `caldram` (HIGHRAM1, données, pile, tampons) pour le cœur B. Le firmware d'origine ne les active qu'au-delà de 300 MHz. Nous les gardons actifs depuis la 0.23, à tous les paliers, pour corriger les fautes et les blocages des versions 0.20 à 0.22, quand le cœur B tournait à 200 MHz en un cycle (§18.2).

**Pourquoi le revoir** : le MP3 en 320 kb/s fait 14,7 M instructions/s sur le banc, pour 49,7 MHz sur l'appareil, soit environ 3,4 cycles par instruction. Supprimer l'attente aux paliers ≤ 100 MHz pourrait réduire la charge de 25 à 35 % (estimation).

**Proposition validée par l'utilisateur** :
1. Une image de test A/B.
2. Si elle est concluante, l'attente selon le palier : sans attente jusqu'à 100 MHz, avec attente à 133 et 200 MHz. On active l'attente avant de monter, et on la retire après être descendu ; le bit n'est basculé que pendant un sommeil du cœur B, et ce changement en marche passe un essai d'endurance.
3. Une nuit de lecture pour mesurer l'autonomie.

**Étape 1, image `make EXTRA=-DHIFI_WAIT_AB`** (`work/NEWFWAB1.IMG`, CRC `0xfe6073a2`, base 0.71) :
- cœur B fixe à 67 MHz (`player_clock = 6`) ;
- l'attente retirée pour une piste et remise pour la suivante, basculée seulement dans `hifi_start_own()`, cœur B en reset ;
- journal : `hifi: HIGHRAM wait off|on (A/B test)` avant chaque piste, puis la ligne `clock:` de la piste.

Dans l'émulateur, les pistes alternent `off`, puis `on`, à 66 MHz, palier fixe, sans manque. L'émulateur ne modélise pas les cycles de la mémoire : l'effet ne se mesure que sur l'appareil. La version normale reconstruite reste identique : CRC `0xd38df64d`.

**Résultat de l'étape 1** (journal `work/hwlog_1025_ab1/`, 19 min de lecture, cœur B fixe à 67 MHz) :

| Piste | Attente active | Sans attente | Écart |
|---|---|---|---|
| `MP3BENCH.MP3` (MP3 320 kb/s, 239 s entières, deux lectures de suite) | 49,7 MHz | **38,8 MHz**, 0 manque | **−22 %** |
| `OPEN_aotuv.ogg` (Vorbis) | 61,3 MHz (76 s) | 47,2 MHz (30 s) | −23 % |
| « 01 Human After All » / « 02 The Prime » (MP3) | 49,9 MHz (318 s) | 40,0 et 44,2 MHz (14 s chacune) | −11 à −20 % |
| « Daft Punk » Discovery (Opus) | 65,4 MHz, **78 manques** (67 MHz trop lent) | **46,3 MHz, 0 manque** | |

Les pistes courtes, sautées après quelques secondes, ne se comparent pas.

Aucune faute, aucun « descriptor not taken », aucun dépassement de délai, aucune erreur dans tout l'essai. L'Opus, qui ne tenait pas à 67 MHz avec l'attente, y tient sans elle. Le gain, environ −22 %, entre dans la fourchette estimée.

Les pistes sans attente ne totalisent qu'environ 6 min. L'endurance d'une heure reste à faire, sur l'image de l'étape 2.

**Étape 2** (0.72, `work/NEWFW072.IMG`, CRC `0xcde23882`) : l'attente suit le palier dans `hifi_set_div()`.
- Elle est retirée jusqu'à 100 MHz et activée à 133 et 200 MHz.
- En montant, l'attente est activée avant le changement de fréquence ; en descendant, elle n'est retirée qu'après.
- Le changement se fait pendant un sommeil du cœur B s'il en vient un dans les 2 ms, sinon tout de suite.
- La ligne `clock:` de chaque piste compte les basculements, et ceux faits pendant que le cœur B travaillait.

L'image d'endurance `make EXTRA=-DHIFI_WAIT_STRESS` (`work/NEWFWST1.IMG`, CRC `0x853f0941`) alterne 100 et 133 MHz toutes les 250 ms. Dans l'émulateur, les deux images décodent sans manque et donnent le même son, mais le régulateur n'y tourne pas (TIMER1 non modélisé) : le basculement en marche ne s'essaie que sur l'appareil.

**Pourquoi l'attente à 133 et 200 MHz : une marge, pas une exigence.** Le TRM donne environ 150 MHz sans le cycle supplémentaire, et ne réclame ce cycle que vers 300 MHz. Le firmware d'origine ne l'active qu'à partir de 300 MHz de `hclk_cal`, donc il tourne en un cycle à 200 MHz. Notre seul argument est la 0.22 : blocages et fautes à 200 MHz en un cycle. Or la 0.23 a changé deux choses à la fois, le cœur B passé de 200 à 100 MHz et l'attente activée, et l'on ne sait pas laquelle a corrigé ces défauts.

**Essais ajoutés au plan (validés par l'utilisateur)**, après une endurance propre de la 0.72 :
- **Étape 2b, 133 MHz sans attente** (sous la limite du TRM) : le seuil passe à 133 MHz, puis un essai d'endurance alterne 100 et 133 MHz, et enfin 133 et 200 MHz pour éprouver le basculement autour du nouveau seuil.
- **Étape 2c, 200 MHz sans attente** (comme le firmware d'origine) : une image A/B à 200 MHz fixes, comme l'étape 1, avec une longue lecture et des lectures de fichiers pendant le décodage.

Si les deux passent, l'attente ne sert plus sur nos paliers, qui ne dépassent jamais 200 MHz, et on la retire partout.

**Étape 3** : une nuit de lecture pour mesurer l'autonomie, sur la version retenue.

**Résultat de l'endurance NEWFWST1** (journal `work/hwlog_1025_st1/`, 2026-10-25 de 02:10 à 10:23, 8 h 13, 177 pistes) : **échec**.

| Codec | Lecture | Fautes du cœur B |
|---|---|---|
| WAV | 7,5 min | 0 |
| FLAC | 2,4 min | 0 |
| MP3 | 27 min | 1 |
| Vorbis | 101 min | **43** (une toutes les 2 à 3 min) |
| Opus | 259 min | 1 |

- Aucun journal antérieur ne contient de `core B error` : ni les 0.6x (régulateur de 50 à 200 MHz, attente toujours active), ni l'essai A/B (67 MHz fixes).
- Les fautes Vorbis sont des sauts vers des adresses fausses : `pc` dans le tas (`0x0102xxxx`), en `0xfffxxxxx` ou ailleurs, CFSR `0x20000` (INVSTATE) ou `0x1` (IACCVIOL). Dans la plupart, `lr` pointe dans `mdct_butterfly_generic` ou `presymmetry` (`mdct.c` de Tremor). Une donnée lue fausse en mémoire (adresse de retour, pointeur) en est l'explication la plus simple.
- Chaque faute fait échouer la piste : un manque, puis la piste suivante.
- Les 13 « open failed » tombent tous sur des fichiers abîmés de la base d'essai : c'est attendu.
- La fin de la ligne `clock:` est coupée par le tampon de 200 octets de `log_printf()`. Le nombre de basculements faits pendant que le cœur B travaillait est donc perdu.

L'essai ne sépare pas deux causes : 100 MHz sans attente, ou le basculement de l'attente pendant le décodage. Dans l'essai A/B, le Vorbis sans attente n'a tourné que 30 s : cela ne prouve rien. **La 0.72 ne doit pas servir à l'usage courant** ; la 0.71 reste la version de référence.

**Diagnostic (validé par l'utilisateur)** : deux images pour séparer les causes, plus deux corrections du journal, qui valent pour toutes les images.
- Le tampon de `log_printf()` passe à 256 octets : la ligne `clock:` tient en entier.
- Une faute du cœur B ajoute le palier, l'état de l'attente et le temps écoulé depuis le dernier basculement (`hifi_wait_t`).
- **NEWFWST2** (`make EXTRA=-DHIFI_WAIT_FIXED100`, CRC `0xe7231488`) : cœur B fixe à 100 MHz (`player_clock = 4`), sans attente et sans aucun basculement. Si elle faute, 100 MHz sans attente n'est pas sûr.
- **NEWFWST3** (`make EXTRA="-DHIFI_WAIT_STRESS -DHIFI_WAIT_ALWAYS"`, CRC `0x88ecc15c`) : alternance de 100 et 133 MHz toutes les 250 ms comme la ST1, mais l'attente reste toujours active (`WAIT_DIV 9`). Si elle faute, c'est le changement de fréquence lui-même qui est en cause.
- Si aucune des deux ne faute, la cause est le basculement de l'attente pendant le décodage.

Dans l'émulateur, les deux images décodent `CH7.OGG` sans manque. La ST2 démarre à 100 MHz sans attente, la ST3 à 200 MHz avec. Chacune doit tourner environ 30 min en Vorbis seul sur l'appareil.

**Résultat de la ST2** (journal `work/hwlog_1025_st2/`, 2026-10-25 à partir de 10:41, 20 min de Vorbis, Beast In Black, 6 pistes) : **aucune faute**, aucun manque.
- Charge moyenne à 100 MHz sans attente : environ 48 MHz par piste, contre 53 à 62 MHz dans la ST1, où l'attente était active la moitié du temps.
- Au taux de la ST1 (43 fautes en 101 min de Vorbis), on en attendait au moins 8 en 20 min. Si toutes venaient des moments sans attente, on en attendait environ 17. La probabilité de n'en voir aucune est négligeable.
- Conclusion : 100 MHz sans attente, en régime stable, n'est pas en cause. Le suspect est le basculement en marche ; la ST3 dira si c'est celui de l'attente ou celui de la fréquence.

**Résultat de la ST3** (journal `work/hwlog_1025_st3/`, 2026-10-25 à partir de 11:05, 16 min de Vorbis, 3 pistes) : **aucune faute**, aucun manque.
- 3 972 changements de fréquence entre 100 et 133 MHz, l'attente toujours active, jamais basculée.
- Au taux de la ST1, on attendait environ 7 fautes.

**Conclusion des essais** :

| Image | Fréquence | Attente | Vorbis | Fautes |
|---|---|---|---|---|
| ST1 | 100 ↔ 133 MHz | basculée en marche | 101 min | 43 |
| ST2 | 100 MHz fixes | jamais active | 20 min | 0 |
| ST3 | 100 ↔ 133 MHz | toujours active | 16 min | 0 |
| AB1 | 67 MHz fixes | basculée cœur B en reset | 19 min (tous codecs) | 0 |

Chaque mode de fonctionnement est sûr ; seul le basculement de l'attente pendant que le cœur B tourne cause les fautes. Attendre un sommeil du cœur B pendant 2 ms ne suffit pas, ou bien le sommeil n'est pas toujours arrivé à temps. On ne sait pas lequel des deux : le compteur était coupé dans le journal de la ST1.

**Étape 2c, image NEWFWST4** (`make EXTRA="-DHIFI_WAIT_STRESS -DHIFI_WAIT_NEVER"`, CRC `0x0ef485e9`) : l'attente n'est jamais active (`WAIT_DIV 1`), à aucun palier, et n'est jamais basculée. Le cœur B alterne 133 et 200 MHz toutes les 250 ms : c'est la tenue à 200 MHz en un cycle, comme dans le firmware d'origine. Dans l'émulateur, Vorbis et Opus décodent sans manque, avec `HIGHRAM wait off` à 200 MHz. Sur l'appareil : lecture de tout le répertoire racine de l'eMMC, fichiers abîmés compris.

**Résultat de la ST4** (journal `work/hwlog_1025_st4/`, 2026-10-25 à partir de 11:30, 33 min, lecture aléatoire depuis la racine, 13 pistes) : **aucune faute**.
- 19 min de Vorbis, 14 min d'Opus, des WAV et MP3 courts, deux fichiers abîmés (`RR_RANDOM.MP3`, `ZZ_ZEROS.OGG`) refusés comme attendu.
- La moitié du temps à 200 MHz sans attente ; aucun basculement. Au taux de la ST1, on attendait environ 8 fautes en Vorbis.
- Trois manques brefs, de 27 540, 7 936 et 2 616 octets (le compteur est cumulé sur les pistes enchaînées). Ils tombent sur `R_CH5.MP3` (MP3 suivi de 3 Mio d'octets aléatoires), sur le `CH7.OGG` de 1,9 s joué juste après, et au refus de `RR_RANDOM.MP3`. Ce sont les manques connus des fichiers abîmés, déjà présents en 0.67 : pas une régression.
- Non couverts : le FLAC (aucune piste tirée) et un MP3 long.

Les essais ST2 à ST4 montrent que le cœur B tourne sans attente de 67 à 200 MHz, à fréquence fixe comme en alternance. Le défaut de la 0.72 ne venait que du basculement de l'attente en marche. Pour la 0.22, l'explication la plus probable est donc autre chose que la fréquence de 200 MHz en un cycle.

## 52. Mémoires du cœur B sans attente (0.73, 2026-10-25)

Conclusion des essais du §51 : l'attente est retirée partout, et n'est plus jamais basculée.
- `clock_init()` écrit `GRF_INTER_CON0 = 0xf0403040` : les bits 14 et 15 sont mis à 0 au démarrage, cœur B éteint. Avant, cette écriture n'y touchait pas, et c'est `wait_set()` qui les fixait. `clock_slow()` les met déjà à 0.
- `hifi_set_div()` ne change plus que le diviseur. `wait_set()`, l'attente d'un sommeil du cœur B (2 ms), `hifi_wait` et les compteurs de basculements disparaissent, ainsi que la fin « RAM wait switched » de la ligne `clock:`.
- Les variantes d'essai `HIFI_WAIT_STRESS`, `HIFI_WAIT_ALWAYS`, `HIFI_WAIT_NEVER` et `HIFI_WAIT_FIXED100` sont retirées. Les images ST1 à ST4 restent dans `work/`.
- Ce qui reste des essais :
  - le tampon de `log_printf()` à 256 octets ;
  - une faute du cœur B donne aussi la fréquence du moment.

Image `work/NEWFW073.IMG`, CRC `0x80647abf`. Dans l'émulateur, les cinq formats (WAV, FLAC, MP3, Vorbis, Opus) décodent sans manque, enchaînés avec un changement de fréquence d'échantillonnage. On y voit l'écriture `0xf0403040` au démarrage.

À vérifier sur l'appareil :
- du FLAC, dont `Epic_Trailer_Theme.flac` : le FLAC n'a été tiré dans aucun essai sans attente ;
- `MP3BENCH.MP3` au régulateur normal, avec un palier attendu autour de 50 ou 57 MHz ;
- l'absence de faute sur une longue lecture ;
- puis la nuit de mesure d'autonomie, à comparer aux 70 mV/h de la nuit du 24 octobre.

**Essai sur l'appareil** (journal `work/hwlog_1025_073/`, 2026-10-25 à partir de 12:12, 19 min, 12 pistes) : **aucune faute**. Démarrage à 200 MHz, sans repli.

| Piste | Charge moyenne du cœur B | Palier principal | 0.70/0.71 (avec attente) |
|---|---|---|---|
| `MP3BENCH.MP3` (239 s entières) | **38,9 MHz** | 57 MHz (233 s) | 49,7 MHz |
| `Epic_Trailer_Theme.flac`, `TEST4.FLAC` | 15,0 à 15,7 MHz | 50 MHz | — |
| `TEST5.OPUS` | 51,9 MHz | 67 et 80 MHz | — |

- Le MP3 au régulateur normal retrouve le −22 % de l'essai A/B, et le FLAC reste au palier le plus bas.
- **Manques** :
  - 2 manques (3 652 octets, 20 ms) au lancement de `MP3BENCH.MP3` depuis le navigateur : l'anneau du DAC est vidé au changement de piste (2 304 octets « at the switch »), c'est le blanc normal d'un saut de piste ;
  - 1 manque d'environ 0,65 s à chacun des deux déplacements dans `TEST5.OPUS` : l'anneau est vidé, puis la réouverture Opus et le premier PCM prennent 0,6 à 0,66 s. Les journaux des 0.64 à 0.66 montrent déjà le même délai (0,6 à 1 s avant le premier PCM après un déplacement), mais leur ligne `chain:` était coupée avant le compteur : celui-ci n'apparaît qu'avec le tampon de 256 octets. Ce n'est pas une régression.
- Pas de Vorbis dans cet essai ; la ST4 l'a couvert sans attente, à 133 et 200 MHz.

Reste : la nuit de mesure d'autonomie.

## 53. AIFF (0.74, 2026-10-25)

Choix validé : pas de nouveau codec pour le cœur B, mais le codec WAV étendu en codec PCM (`fw/hifi/codec_pcm.c`, ex-`codec_wav.c`, 3,4 Ko de code contre 2,9 Ko). Mutualiser les gros codecs dans une même image n'est ni possible (zone de code de 128 Ko, Opus 69 Ko + Vorbis 60 Ko) ni utile (un chargement coûte 1 à 4 ms, l'enchaînement entre formats différents garde déjà le DAC). Le partage se fait au niveau des sources, comme `oggpage.c`.

**Cœur B** : le conteneur se reconnaît à ses 4 premiers octets (`RIFF`/`WAVE`, `FORM`/`AIFF`, `FORM`/`AIFC`).
- `COMM` : canaux, nombre de trames (la durée), taille d'échantillon arrondie à l'octet (les échantillons AIFF sont cadrés à gauche), fréquence en flottant étendu 80 bits (exposant et 32 bits hauts de la mantisse).
- AIFF-C : compression `NONE` ou `twos` (gros-boutiste), `sowt` (petit-boutiste, le chemin WAV) ; le reste est refusé (`HP_ERR_UNSUPPORTED`).
- `SSND` : décalage, taille de bloc, puis les données ; la durée est le plus petit des deux, `COMM` ou la taille de `SSND`. Le déplacement est exact, comme en WAV.
- 16 bits stéréo gros-boutiste : copie directe après inversion des octets de chaque échantillon ; les autres formats passent par la conversion trame par trame. L'AIFF 8 bits est signé, le WAV 8 bits ne l'est pas.
- `COMM` doit précéder `SSND` (l'entrée ne fait qu'avancer) : c'est l'ordre de tous les encodeurs connus.
- Non lus : les flottants (`fl32`, `fl64`, WAV IEEE float ; le cœur B est un Cortex-M3 sans FPU), `ulaw`, `alaw`.

**Cœur A** :
- `HP_CODEC_AIFF` (6) : identifiant propre au cœur A (extension `aif`, `aiff`, `aifc` ; « AIFF » à l'écran ; lecture des tags). `player_open()` charge pour lui l'image `HP_CODEC_WAV` : WAV et AIFF s'enchaînent sur la même image, simplement redémarrée.
- Tags : `id3v2()` prend un décalage. L'AIFF porte un chunk `ID3 ` (ffmpeg, iTunes), sinon `NAME` et `AUTH`. Le WAV lit `LIST INFO`, et désormais `id3 `/`ID3 `. Les deux lecteurs sautent les échantillons : la fenêtre de 512 Kio (`SCAN_MAX`) repart après eux (`rd.lim`), car ffmpeg écrit le chunk `ID3 ` après `SSND`. Avant, le lecteur WAV s'arrêtait à `data` et manquait les `LIST INFO` placés après.

**Essais dans l'émulateur** (extrait de 3 s de `work/long/L.mp3`, fichiers produits par ffmpeg) :
- AIFF 16 bits, 24 bits/48 kHz, 24 bits/96 kHz, 32 bits, 8 bits mono, 16 bits mono, AIFF-C `sowt`, AIFF avec `ID3 ` seul (sans `NAME`), WAV avec `LIST INFO` après `data` ;
- tous enchaînés sans manque. La sortie du cœur B est identique à celle de ffmpeg pour les fichiers stéréo, et identique aux échantillons du fichier pour les mono (ffmpeg change le niveau quand il duplique un canal) ;
- les tags sont trouvés dans tous les cas, y compris après les données ;
- fichiers abîmés (variantes de `damage.py` sur un AIFF) : en-tête détruit, que des zéros, que de l'aléatoire → refusés à l'ouverture ; aléatoire ou zéros ajoutés à la fin → arrêt à la fin de `SSND` ; tronqué → arrêt à la coupure ; vide → grisé.
- Gros fichiers (tirés de « Discovery », bibliothèque beets) : la lecture des tags placés à la fin prend 50 ms (37 Mo), 74 ms (57 Mo) et 157 ms (122 Mo), le temps de suivre la chaîne de la FAT. En enchaînement, ce délai est absorbé par l'ouverture 1,1 s à l'avance.

Image `work/NEWFW074.IMG`, CRC `0xf52a2342`. Fichiers pour l'appareil dans `work/aiff_test/` : « Discovery » en AIFF 16 bits, AIFF-C `sowt`, 24/48, 24/96, et un WAV avec ses tags après les données.

## 54. AAC-LC : comparatif PacketVideo et Helix sous QEMU (2026-10-25)

Choix de conception validé (le lecteur MP4 sur le cœur A, des tables lues dans le fichier comme le firmware d'origine, un flux de paquets préfixés par leur longueur vers le cœur B, partagé avec l'ALAC). Reste à choisir le décodeur. Le firmware d'origine utilise Helix AAC sans SBR (REPORT §33).

**Banc** (`re/aac/bench/`, local, sur le modèle du banc MP3 : Cortex-M3 nu sous QEMU `mps2-an385`, `-icount shift=0`, `memcpy`/`memset` du runtime du cœur B) :
- PacketVideo `aacdec` (AOSP `android-4.0.4_r2.1`, Apache-2.0), sans `AAC_PLUS`, appelé à travers une petite couche C (`pvshim.cpp`) ;
- Helix AAC (copie durcie d'ESP8266Audio, `e1f2a1a`, 2026-10-07), sans SBR ; chemin ARM de `assembly.h` activé (`smull`, `smlal`, `ssat`, `clz`, comme la compilation Rockchip), `abs()` mis en ligne (appelée en fonction, elle coûtait 6,6 % du temps) ;
- entrée : trames brutes et AudioSpecificConfig (`mkstream.py`, depuis un ADTS), comme le cœur A les fournira depuis un MP4 ; référence : décodage de ffmpeg, SNR par `snr.py` ;
- extraits de 20 s : t1 Rage Against the Machine « Testify » (M4A iTunes, 125 kb/s), t2 The Doors (M4A, 128 kb/s), t3 Daft Punk « One More Time » en AAC ffmpeg 256 kb/s sans PNS, t4 le même en 128 kb/s avec PNS (réglage par défaut), t5 le même en 256 kb/s à 48 kHz.

**Instructions par seconde d'audio** (millions) :

| Extrait | PV -O2 | PV -O3 | Helix -O2 | Helix -O3 | Helix -O3, `abs` en ligne | Écart Helix / PV -O3 |
|---|---|---|---|---|---|---|
| t1 iTunes 125k | 15,02 | 13,68 | 13,20 | 13,25 | **12,17** | −11 % |
| t2 Doors 128k | 14,92 | 13,54 | 11,66 | 11,64 | **11,10** | −18 % |
| t3 256k | 16,68 | 15,80 | 15,84 | 15,87 | **14,72** | −7 % |
| t4 128k PNS | 15,16 | 13,92 | 12,76 | 12,79 | **11,92** | −14 % |
| t5 256k 48 kHz | 17,80 | 16,66 | 16,31 | 16,34 | **15,22** | −9 % |

**Précision face à ffmpeg** (identique pour toutes les options de compilation d'un même décodeur) :

| Extrait | PacketVideo | Helix |
|---|---|---|
| t1 | 76,7 dB, écart max 6 LSB | 87,3 dB, 2 LSB |
| t2 | 74,9 dB, 5 LSB | 82,3 dB, 1 LSB |
| t3 | 75,6 dB, 7 LSB | 82,9 dB, 1 LSB |
| t5 | 75,6 dB, 6 LSB | 82,9 dB, 1 LSB |
| t4 (PNS) | 26,3 dB | 26,3 dB |

Le PNS fait générer du bruit au décodeur : t4 ne se compare pas, comme déjà constaté (REPORT §34). Aucune erreur de décodage, pas de décalage (lag 0) pour les deux.

**Mémoire et code** : état du décodeur 48,7 Ko (PV) contre 20,7 Ko (Helix) ; code et tables environ 59 Ko (PV -O3) contre 49 Ko (Helix). Les deux tiennent dans la zone de code de 128 Ko ; PV prend un quart de la zone de données (180 Ko), Helix un neuvième.

**Profil** (t1, 5 s) : PV passe 25 % dans `fft_rx4_long`, 14 % dans `trans4m_freq_2_time_fxp_2`, 9 % dans `inv_long_complex_rot` ; Helix 19 % dans `R4Core`, 12 % dans `raac_DCT4`, 10 % dans `raac_IMDCT`, 10 % dans `raac_StereoProcess`.

**Charge estimée sur l'appareil** : le MP3 PacketVideo fait 14,7 M instructions par seconde au banc et 38,9 MHz sur l'appareil sans attente mémoire (§52), soit environ 2,65 cycles par instruction. Par la même règle : AAC PacketVideo de 36 à 44 MHz, Helix de 29 à 40 MHz. Les deux tiennent au palier de 57 MHz, au pire à 67 MHz.

**Bilan** : Helix est meilleur sur tous les critères mesurés : environ 12 % d'instructions en moins (de 7 à 18 %), 7 à 11 dB de précision en plus (1 à 2 LSB au lieu de 5 à 7), 28 Ko de mémoire en moins. Les écarts de PV restent inaudibles. Le seul argument de PV est la licence : Apache-2.0, contre RPSL/RCSL pour Helix (licence à réciprocité par fichier : les fichiers Helix modifiés et distribués restent sous RPSL avec leurs sources, à côté de notre code). C'est la même situation que pour le MP3 (§19.9 : Helix −22 %, PV retenu pour la licence).

**Décision (utilisateur, 2026-10-25) : PacketVideo**, pour la licence (Apache-2.0), comme pour le MP3. Un portage Thumb-2 de sa FFT pourra réduire l'écart plus tard.

## 55. AAC-LC en M4A (0.75, 2026-10-25)

Mise en œuvre de la conception du §54 (décodeur PacketVideo).

**Cœur B** (`fw/hifi/codec_aac.c`, image `HP_CODEC_AAC` = 7) : PacketVideo `aacdec` (`third_party/pvaacdec`, Apache-2.0, non modifié), compilé en C++ sans exceptions ni RTTI, sans `AAC_PLUS`, en -O3 ; C portable pour l'instant (pas encore d'assembleur Thumb-2). 59 Ko de code, 65 Ko de BSS (dont 52 Ko de mémoire du décodeur). Il lit le flux de paquets, décode 1024 trames par paquet (stéréo entrelacée 16 bits, mono dupliqué par le décodeur lui-même), jette `skip` trames au début et s'arrête à `length`. Une trame illisible donne 1024 trames de silence (la durée est gardée) et compte dans `error_info`.

**Flux de paquets** (`hifi_proto.h`, `struct hp_pkt_header`, magie « HP4P ») : fréquence, canaux, `skip`, `length`, `start_frame`, taille de la configuration ; puis l'AudioSpecificConfig ; puis chaque trame précédée de sa longueur (u16). Prévu pour servir aussi à l'ALAC.

**Cœur A** (`fw/src/mp4.c`) :
- boîtes lues à travers un cache d'un secteur (sans lui, l'ouverture d'un M4A d'iTunes avec sa pochette prenait 166 ms dans l'émulateur ; 16 ms avec) ;
- première piste son (`hdlr` = `soun`) ; entrée `mp4a` (versions QuickTime 1 et 2 comprises, `esds` éventuellement dans `wave`) ; configuration lue dans `esds` ; AAC-LC seul (type d'objet 2), mono ou stéréo, trames de 1024 : le reste est refusé avec un message (« no codec for it in the firmware ») ; `alac` reconnu et refusé pour l'instant ;
- tables `stsz`, `stco`/`co64`, `stsc` jamais chargées : une fenêtre de 512 octets par table (plus 12 octets pour une entrée à cheval sur la fin du secteur, défaut trouvé dans l'émulateur), comme le firmware d'origine ;
- données : une fenêtre de 4 Kio, si bien que les trames d'un même bloc (chunk), contiguës, coûtent une lecture ; le flux est écrit par 4 Kio au moins ;
- amorce, remplissage et longueur : `iTunSMPB` (iTunes ; sa valeur fait 124 octets), sinon la liste d'édition (`elst`, ffmpeg), sinon toute la piste ;
- déplacement : la trame cible est trouvée en parcourant les séries de `stsc`, puis les tailles du bloc ; le flux repart **une trame avant** la cible (le recouvrement de la transformée) et le cœur B jette ce qui précède la cible ;
- fin de fichier atteinte au milieu d'une trame (fichier tronqué) : le flux s'arrête à la dernière trame complète. Défaut trouvé dans l'émulateur et corrigé : la même fenêtre était relue sans fin (568 587 commandes eMMC).

**Tags** (`tags.c`) : `moov/udta/meta/ilst` : `©nam`, `©ART`, `©alb`, `trkn`, et les `----` nommés `replaygain_*` (beets, foobar2000). Si `moov` suit les données (ffmpeg), la fenêtre de lecture repart après `mdat`.

**Navigateur** : extensions `m4a`, `m4b`, `mp4` ; « AAC » à l'écran.

**Essais dans l'émulateur** :
- RATM « Testify » (M4A iTunes complet, 3,4 Mo, `moov` en tête, `iTunSMPB`, pochette) : tags lus en 12 ms ; amorce 2112, remplissage 960 ; sortie alignée à l'échantillon près sur ffmpeg ;
- déplacement de 4,8 s à 32,6 s : premier PCM 45 ms après la commande ; dès le premier échantillon, à 3 LSB de ffmpeg au plus (76,6 dB ensuite) ;
- extrait recopié par ffmpeg (`moov` après `mdat`, `elst` : amorce 56, remplissage 31) : 76,7 dB face à ffmpeg, 265 129 trames comme la liste d'édition (ffmpeg en sort 31 de plus, le remplissage) ;
- mono 48 kHz (encodeur AAC de ffmpeg, PNS actif) : L = R, aligné (décalage 0) ; 46 dB face à ffmpeg à cause du PNS, comme au §54 ;
- enchaînement sans blanc entre M4A, et d'un M4A à 44,1 kHz vers un M4A à 48 kHz (changement de fréquence) ; ALAC refusé proprement ;
- fichiers abîmés (deux variantes, `moov` en fin et en tête) : en-tête détruit, zéros, aléatoire, `moov` absent (tronqué) → refusés ; tronqué avec `moov` en tête → arrêt à la trame 124 ; milieu abîmé ou octets ajoutés → lecture complète.

Image `work/NEWFW075.IMG`, CRC `0xad161731`. Pour l'appareil : `work/aac_test/` (l'album « The Battle of Los Angeles » d'iTunes copié depuis la bibliothèque beets, `CHC.M4A` pour `damage.py`, un ALAC qui doit être refusé). À mesurer : la charge du cœur B (estimée de 36 à 44 MHz au §54).

**Mesure d'autonomie de la 0.73** (journal `work/hwlog_1025_bat073/`, 2026-10-25 de 13:15 à 14:53) : l'essai a duré **1 h 37 seulement**, et il a commencé juste après 43 min de charge en mode USB (4 179 mV).
- Lecture aléatoire depuis la mémoire interne : 65 min de Vorbis, 29 min d'Opus, 3 min de MP3 ; écran éteint presque tout le temps.
- **Aucune faute du cœur B, aucun manque** sur les 34 pistes. Charge moyenne du cœur B : 44 MHz ; paliers : 67 MHz pendant 4 233 s, 80 MHz pendant 1 312 s, presque jamais au-delà.
- Batterie : 4 179 → 4 028 mV ; pente 87,5 mV/h sur toute la durée, 73,5 mV/h sur la dernière demi-heure.
- **Pas comparable** aux 70 mV/h de la nuit du 24 octobre (0.61) : la durée est trop courte, l'essai démarre juste après une charge (le haut de la courbe descend plus vite), et les formats diffèrent (Vorbis et Opus, plus lourds que le MP3 320 kb/s lu depuis la carte). La comparaison demande une nuit dans les mêmes conditions.

**Essai sur l'appareil de la 0.75** (journal `work/hwlog_1025_075/`, 2026-10-25 de 15:04 à 16:04 ; AIFF de la 0.74 compris) : **aucune faute**, démarrage à 200 MHz.

| Contenu | Pistes | Charge moyenne du cœur B | Palier |
|---|---|---|---|
| AAC, RATM « The Battle of Los Angeles » (iTunes, ~125 kb/s), album entier | 13 | **25,0 à 25,3 MHz** | 50 MHz (le plus bas) |
| AIFF 16 bits 44,1 kHz (« One More Time », AIFF-C `sowt`) | — | 0,5 à 1,5 MHz | 50 MHz |
| AIFF 24 bits / 48 kHz | — | 7,8 MHz | 50 MHz |
| AIFF 24 bits / 96 kHz | — | 15,7 à 16,4 MHz | 50 MHz |

- **L'AAC coûte 25 MHz, bien moins que les 36 à 44 MHz estimés** au §54 depuis le MP3. L'assembleur Thumb-2 n'est pas nécessaire pour l'instant.
- **Album AAC enchaîné sans blanc** : les 13 pistes de bout en bout, 0 manque ; les tags sont lus (3,4 ms).
- **Déplacements** : 14 en AAC, chacun avec un seul blanc d'environ 16 ms (2,6 à 3,8 Ko vidés de l'anneau) ; en AIFF 24/96, de 54 à 72 Ko (60 à 80 ms) ; en AIFF 16 bits, 5 à 7 Ko.
- **ALAC** : refusé, « Cannot play … no codec for it in the firmware ».
- **Fichiers abîmés** (lecture aléatoire de `test_damanged_files/`, 62 fichiers) : `H_`, `RR_`, `ZZ_` et `T_CHC.M4A` refusés ; `M_`, `R_`, `Z_` lus en entier ; `T_CHB.AIFF` s'arrête à la coupure.
- **Défaut de journal relevé** (pas propre à l'AAC) : après trois ouvertures refusées de suite (`ZZ_ZEROS.MP3` deux fois, 1,15 s chacune, et `H_CHB.AIFF`), la fin de la piste précédente avait fini de jouer. La ligne « queued behind 24346666 ms of the last track » affiche alors une durée négative lue comme non signée, et la piste suivante compte un manque de 217 Ko : le blanc réel pendant les refus. À corriger dans le calcul de `tail` (considérer la fin comme jouée quand `tail.end` est dépassé).

## 56. ALAC en M4A (0.76, 2026-10-25)

Choix validés : le décodeur de référence d'Apple, sur le lecteur MP4 et le flux de paquets de la 0.75, en logiciel (le firmware d'origine passait par le prédicteur matériel HIFIACC, comme pour son FLAC).

**Cœur B** (`fw/hifi/codec_alac.cpp`, image `HP_CODEC_ALAC` = 8) : décodeur d'Apple (`third_party/alac`, dépôt macosforge, commit `c38887c`, Apache-2.0, fichiers de décodage non modifiés), compilé en C++ sans exceptions ni RTTI, avec `TARGET_RT_LITTLE_ENDIAN=1` (sans cette option, le code d'Apple prend l'ARM pour une machine gros-boutiste). 9 Ko de code seulement. Ses `calloc` passent par le tas du runtime (`rt_alloc`, libéré en bloc à l'ouverture suivante) ; l'objet `ALACDecoder` est construit par placement (aucun constructeur global n'est appelé au démarrage du cœur B). Mémoire pour des paquets de 4096 trames : environ 48 Ko pour le décodeur, plus un paquet (`maxFrameBytes` de la configuration) et sa sortie.
- 16 bits : trames de 16 bits empaquetées (stéréo copiée telle quelle) ; 20 et 24 bits : trames de 32 bits, données en haut ; mono dupliqué.
- **Défaut trouvé dans l'émulateur et corrigé** : 4096 trames de 24 bits occupent 32 Kio, toute la taille de l'anneau de sortie, qui ne peut jamais être plein : le cœur B attendait sans fin. La sortie s'écrit désormais par morceaux, à mesure que la place se libère.
- Un paquet illisible donne un paquet de silence (la durée est gardée).

**Cœur A** (`mp4.c`) : l'entrée `alac` est lue (la boîte `alac` qu'elle contient : version, puis l'ALACSpecificConfig de 24 octets, envoyé tel quel au cœur B) ; 16, 20 ou 24 bits, mono ou stéréo, sinon refus. Taille de paquet lue dans la configuration (4096 en général). Pas d'amorce : la longueur est la somme des durées de `stts` (le dernier paquet est plus court). Les paquets sont indépendants : un déplacement repart du paquet de la cible, sans pré-décodage. Le navigateur donne « AAC » à tout M4A ; c'est le conteneur qui décide, et la file d'attente retient ensuite le vrai codec (« ALAC » à l'écran, tags lus de la même façon).

**Essais dans l'émulateur** (FLAC de la bibliothèque beets, Pink Floyd « Echoes », convertis par ffmpeg) :
- 16 bits 44,1 kHz, 20 s : sortie **identique au bit près** au décodage de ffmpeg, y compris après un déplacement de 4 s à 12,6 s (premier échantillon exactement à la cible) ;
- 24 bits / 96 kHz (rééchantillonné) et mono 16 bits : **identiques au bit près** ;
- tags et ReplayGain (écrits avec mutagen comme le fait beets : `----:com.apple.iTunes:replaygain_*`) lus sur un fichier entier ;
- fichiers abîmés : milieu aléatoire et aléatoire ajouté → lus jusqu'au bout sans faute ; tronqué (`moov` en fin) → refusé ;
- pas de régression de l'AAC (76,7 dB, comme en 0.75).
- Remarque : ffmpeg avec `-movflags use_metadata_tags` écrit les tags au format QuickTime « mdta » (clés numérotées), qui n'est ni celui d'iTunes ni celui de beets ; ces fichiers n'affichent pas de tags.

Image `work/NEWFW076.IMG`, CRC `0x712826f0`. Pour l'appareil : `work/alac_test/` (quatre pistes enchaînées de « Echoes » en ALAC 16 bits, dont le passage sans blanc entre « The Happiest Days of Our Lives » et « Another Brick in the Wall (Part 2) », une piste en 24/96, une en mono, `CHE.M4A` pour `damage.py`). À mesurer : la charge du cœur B.

**Essai sur l'appareil de la 0.76** (journal `work/hwlog_1025_076/`, 2026-10-25 à partir de 17:40) : **aucune faute**, démarrage à 200 MHz.

| Contenu | Charge moyenne du cœur B | Palier principal |
|---|---|---|
| ALAC 16 bits 44,1 kHz stéréo (« Echoes », pistes 01 à 04) | **40 à 42 MHz** | 57 MHz |
| ALAC 16 bits mono (« Money ») | 23 à 28 MHz | 50 MHz |
| ALAC 24 bits / 96 kHz (« The Great Gig in the Sky », rééchantillonné) | **108 à 110 MHz** | 200 MHz en permanence |
| pour mémoire : FLAC 16 bits (§52), AAC 125 kb/s (§55) | 15 MHz, 25 MHz | 50 MHz |

- **Enchaînement sans blanc** des pistes 01 à 04 (dont 03 → 04, enchaînées dans le disque) : le DAC est gardé, l'anneau plein au changement, 0 manque.
- Tags et ReplayGain lus ; « ALAC » à l'écran.
- Déplacements : un seul blanc de 2 à 2,5 Ko en mono, de 44 Ko (~0,1 s) en 24/96.
- Fichiers abîmés (`CHE.M4A` et ses variantes, au milieu de la série complète) : comme dans l'émulateur.
- **Le décodeur d'Apple est lent** : 40 MHz pour de l'ALAC 16 bits, presque trois fois notre FLAC (15 MHz) pour un travail comparable, et le 24/96 tient le cœur B à 200 MHz. C'est son code de référence en C portable (décodage de Rice bit par bit, `dyn_decomp`, et prédicteur générique), sans aucune optimisation pour l'ARM. Il tient, sans manque, mais coûte de l'autonomie : une optimisation (ou le prédicteur matériel HIFIACC, comme le firmware d'origine) serait à étudier.

**Le décodeur ALAC d'origine (module 79)** : d'après la référence locale du SDK (`HIFI/alac/`, lue seulement), c'est le décodeur ALAC de **FFmpeg** (`alac.h` porte l'en-tête « This file is part of FFmpeg », licence LGPL-2.1), adapté par Rockchip dans une bibliothèque fermée (`RkNanoD_BHALAC_20151223.lib` puis `_20160926.lib`). Le symbole `HIFI_ACC` et la fonction `Alac_Decode_Lpc` envoient le prédicteur à l'accélérateur HIFIACC (registres LPC, mémoire `0x01069000`, §1.5). Le module a son propre lecteur MP4 (`hifi_alac_MovFile.h`), copie de celui de l'AAC. Le décodage de Rice reste en logiciel. Ni ce code ni celui de FFmpeg (LGPL, peu compatible avec une image statique) ne peuvent servir ici ; l'accélérateur, lui, reste une piste pour notre propre prédicteur.

## 57. ALAC : profil et décodeur optimisé (0.77, 2026-10-25)

Étapes validées par l'utilisateur : 1. profiler le décodeur d'Apple ; 2. écrire notre décodeur en C optimisé ; le HIFIACC selon les résultats.

**Banc** (`re/alac/bench/`, local, comme ceux du MP3 et de l'AAC : Cortex-M3 sous QEMU) : paquets extraits du M4A par `mkstream.py`, sortie au format de l'anneau du cœur B, comparée à ffmpeg. Fichiers : E1 (16 bits 44,1 kHz stéréo, 20 s), E2 (24 bits 96 kHz, 5 s), E3 (16 bits mono, 3 s), encodés par ffmpeg depuis des FLAC de la bibliothèque (ordres de prédiction 4 à 6, surtout 5 et 6 ; l'encodeur d'Apple utilise plutôt 4 et 8).

**Profil du décodeur d'Apple** (E1, 1 s) : **59 % dans le décodage de Rice adaptatif** (`dyn_decomp`), **35 % dans le prédicteur** (`unpc_block`), le reste dans la sortie. Le décodage de Rice compte les zéros de tête par une boucle sur 32 bits (`lead()`, deux fois par échantillon) et lit l'entrée octet par octet ; le prédicteur n'a de version déroulée que pour les ordres 4 et 8.

**Notre décodeur** (`fw/hifi/alacdec.c`, dérivé de celui d'Apple, Apache-2.0, avis conservé) :
- `clz` matériel au lieu de `lead()` ; lecture de l'entrée par mots (`ldr` non aligné, puis `rev`) ;
- prédicteur déroulé par le compilateur pour tous les ordres de 1 à 8 (un seul modèle, `pred_fixed`), coefficients en 16 bits comme la référence (débordement compris) ;
- sortie écrite directement au format de l'anneau (16 bits empaquetés, ou un mot par échantillon de 20 et 24 bits), sans les échantillons de 3 octets de la référence ;
- même arithmétique, dans le même ordre : sortie **identique au bit près** à celle d'Apple sur E1, E2 et E3.
- Un piège de compilation : avec `-ffreestanding` (implique `-fno-builtin`), `memcpy(&v, p, 4)` reste un appel de fonction (9,6 % du temps) ; `__builtin_memcpy` est mis en ligne en un `ldr`.

| Instructions par seconde d'audio (millions) | Apple | nôtre -O2 | nôtre -O3 | -O3 face à Apple |
|---|---|---|---|---|
| E1, 16 bits 44,1 kHz stéréo | 31,0 | 17,8 | **15,4** | **−50 %** |
| E2, 24 bits 96 kHz stéréo | 73,2 | 34,0 | **29,8** | **−59 %** |
| E3, 16 bits mono | 16,0 | 9,1 | — | −43 % (-O2) |

Profil de notre décodeur (-O2, E1) : prédicteur 63 %, Rice 30 % (5,7 M au lieu de 20,4 M : −72 %). Le prédicteur, inchangé dans son principe, devient la part dominante : c'est la partie que le HIFIACC pourrait prendre (étape suivante éventuelle), ou un assembleur Thumb-2 (le code produit par GCC déborde des registres : environ 180 accès à la pile dans `unpc_block`).

**Firmware 0.77** : `hifi/codec_alac.c` (C) appelle `alacdec.c`, compilé en -O3 ; la bibliothèque d'Apple n'est plus compilée (`third_party/alac` reste comme source et pour sa licence). Image ALAC : 10 Ko de code. Image `work/NEWFW077.IMG`, CRC `0x54df0b8e`.

Charge attendue sur l'appareil, au prorata des instructions (0.76 : 41 MHz en 16 bits stéréo, 108 MHz en 24/96) : environ 20 MHz (palier 50 MHz) et 45 MHz (palier 57 ou 67 MHz au lieu de 200).

## 58. FLAC : profil de dr_flac (2026-10-25)

Même démarche que pour l'ALAC (§57). **Banc** `re/flac/bench/` (local) : dr_flac v0.13.4 configuré et appelé comme dans `fw/hifi/codec_flac.c` (lecture par rappel depuis la mémoire, 1024 trames en s16 ou 512 en s32 par appel, conversion au format de l'anneau), sortie comparée à ffmpeg : **identique au bit près** sur tous les extraits. Extraits de 20 s tirés de la bibliothèque beets (recopiés tels quels par ffmpeg), plus deux réencodages :

| Extrait | Débit | Instructions par seconde d'audio |
|---|---|---|
| F3 Martin Roscoe, piano (16 bits 44,1 kHz) | ~395 kb/s | 6,7 M |
| F1 Pink Floyd (16 bits 44,1 kHz) | ~676 kb/s | 7,6 M |
| F4 le même, réencodé en `-compression_level 8` | ~678 kb/s | 8,5 M |
| F2 Motörhead (16 bits 44,1 kHz) | ~1 073 kb/s | 9,0 M |
| F5 Pink Floyd rééchantillonné en 24 bits 96 kHz (6 s) | ~2 120 kb/s | 18,8 M |

Pour comparaison : notre ALAC 16 bits fait 15,4 M (§57), le MP3 PacketVideo 14,7 M, l'AAC 13,5 M. Sur l'appareil, le FLAC 16 bits à ~1 050 kb/s coûte 15 MHz (§52) : par la même règle, le 24/96 coûterait environ 31 MHz, toujours au palier de 50 MHz.

**Profil** (F2, 1 s) : 73 % dans `drflac__decode_samples_with_residual__rice__scalar`, qui fait à la fois le décodage de Rice et la prédiction ; le reste dans la conversion de sortie et la copie de l'entrée. dr_flac utilise déjà l'instruction `clz` sur ARM (pas le défaut du décodeur d'Apple). **Profil par ligne** de cette fonction (`re/flac/bench/lineprof.py`) :
- **prédiction LPC : ~45 %** (dont ~8 % en arithmétique 64 bits : dr_flac la choisit quand bits + précision + log2(ordre) > 32, ce qui arrive pour le canal de différence stéréo, sur 17 bits) ;
- **décodage de Rice : ~30 %** ;
- **CRC-16 des trames : ~18 %** (`drflac__crc16`, octet par octet à chaque rechargement du cache de bits).

**Pistes** (aucune n'est nécessaire : le FLAC reste au palier le plus bas, même en 24/96) :
- le CRC : `DR_FLAC_NO_CRC` gagnerait environ 13 % du total, mais une trame abîmée ne serait plus détectée ni sautée (§46) : elle sortirait en bruit. Un CRC par table de 16 bits (deux octets par recherche) garderait la vérification pour moitié moins cher ;
- la prédiction sur le **HIFIACC** : c'est le seul mode de l'accélérateur déjà connu et validé (§1.5, modèle exact avec le module FLAC d'origine). Il prendrait la part la plus lourde (~45 %), avec la lecture du canal de différence en 64 bits ;
- une prédiction 32 bits déroulée par ordre, comme pour l'ALAC.

## 59. Suréchantillonnage des fréquences basses (0.78, 2026-10-25)

**Défaut trouvé en inventoriant la bibliothèque** : le pilote audio ne connaît que 44,1, 48, 88,2, 96, 176,4 et 192 kHz, et toute autre fréquence était remplacée sans prévenir par 44,1 kHz. 15 MP3 à 22,05 kHz de la bibliothèque (The Offspring « Americana », un titre des Yeah Yeah Yeahs et un du Chœur de l'Armée rouge) auraient été joués deux fois trop vite. Même chose pour tout MP3 MPEG-2 ou 2.5 (8 à 24 kHz) et tout fichier à 32 kHz.

**Correction (choix de l'utilisateur : suréchantillonner dans le cœur B)**, dans le runtime (`fw/hifi/rt.c`), donc pour tous les codecs :
- après `codec_open`, une fréquence que les DAC ne prennent pas est multipliée par le plus petit facteur entier L parmi 2, 3, 4 et 6 qui mène à 44,1, 48, 88,2 ou 96 kHz (22,05 → 44,1 ; 24 → 48 ; 16 → 48 ; 11,025 → 44,1 ; 12 → 48 ; 8 → 48 ; 32 → 96). Sinon, le fichier est refusé (« unreadable file ») au lieu d'être joué à la mauvaise vitesse ;
- la fréquence, la longueur et le départ annoncés au cœur A sont multipliés par L ; `out_space()` est divisé par L, si bien que les codecs n'ont rien à changer ;
- chaque trame écrite passe par un filtre polyphasé (sinus cardinal fenêtré par Kaiser, β = 8, 24 × L − 1 coefficients en Q14, généré par `tools/mkupsample.py` dans `hifi/upsample_tab.h`) : −0,1 dB jusqu'à 0,4 de la fréquence d'entrée, images atténuées de 70 à 77 dB au-delà de 0,6. Chaque phase somme à exactement 1 (pas d'ondulation du continu). Arithmétique 32 bits pour les sources 16 bits, 64 bits pour les 24 bits, avec saturation ;
- le retard du filtre (un nombre entier de trames grâce à la longueur impaire) est retiré au début et vidé à la fin : la sortie a exactement L fois les trames de l'entrée, et l'enchaînement sans blanc est préservé ;
- les codecs n'incrémentent plus `ctl->frames` eux-mêmes mais appellent `out_done()` ; en suréchantillonnage, le runtime compte les trames au fur et à mesure qu'il les écrit (sinon le compteur, en trames d'entrée pendant un appel du codec, faussait la position vue par le cœur A et la capture de l'émulateur).

**Essais dans l'émulateur** (extraits tirés d'un MP3 à 22,05 kHz de la bibliothèque) :
- MP3 22,05 kHz → 44,1 kHz, MP3 8 kHz → 48 kHz, WAV 32 kHz → 96 kHz, WAV mono 16 kHz → 48 kHz : bonnes fréquences, durées exactes, enchaînements avec changement de fréquence sans manque ;
- WAV 32 kHz : sortie **identique au bit près** à un modèle Python du filtre, et à **71,9 dB** du rééchantillonneur soxr de ffmpeg (sans décalage) ; WAV 16 kHz : 37 dB face à soxr, l'écart venant du contenu proche de 8 kHz, que notre filtre atténue plus tôt ;
- images de suréchantillonnage mesurées dans la sortie : −78 à −92 dB.

Coût : 24 multiplications par échantillon de sortie et par canal, soit environ 2 millions par seconde à 44,1 kHz. Image `work/NEWFW078.IMG` (avec l'ALAC de la 0.77), CRC `0xcce80ba7`. Le fichier à 44,1 kHz de l'essai passe intact, et le total des trames comptées égale celui des trames écrites.

## 60. Fréquences d'échantillonnage du firmware d'origine : image d'essai (2026-10-25)

**Ce que contient le firmware d'origine** (`HIFIEC39.IMG`) :
- trois tables de la PLL de l'ACODEC à partir de `0x48c8c` (fichier), entrées de 6 mots (quartz 24 000 kHz, sortie de la PLL en kHz, 4 paramètres), 6 entrées par famille :
  - 44,1 kHz : 28 224 `3 18 25 3` (×2), 56 448 `6 36 25 3` (×2), `4 73 25 3`, vide ;
  - 48 kHz : 30 720 `0 24 25 10` (×2), 61 440 `0 48 25 10`, 122 880 `0 48 25 9` (×2), vide ;
  - 8 kHz : 20 480 `0 48 50 8` (×2), 40 960 `0 96 50 8`, 81 920 `0 96 25 8` (×2), vide ;
- la séquence d'initialisation des CS43131 (paires registre, valeur, à partir de `0x48a44`), où `0x01000b` vaut 1 ;
- la fonction résidente `0x0306185e(fréquence)`, qui écrit `0x01000b` dans les DAC : **0 pour 8, 16, 32, 64 et 128 kHz ; 1 pour 11,025, 22,05 et 44,1 ; 2 pour 12, 24 et 48 ; 3 pour 88,2 ; 4 pour 96 ; 5 pour 176,4 ; 6 pour 192**. Toute autre fréquence n'écrit rien.

Le firmware d'origine prévoit donc 15 fréquences, de 8 à 192 kHz. Nos six (44,1 à 192 kHz) ont les mêmes paramètres ; elles ont été vérifiées sur l'appareil en 0.16 avec une note générée.

**Image d'essai `NEWFWRT1`** (`make EXTRA=-DRATE_TEST` ; `EXTRA` vaut désormais aussi pour les images du cœur B ; CRC `0x7910dfca`) : neuf fréquences ajoutées au pilote (8, 11,025, 12, 16, 22,05, 24, 32, 64, 128 kHz), avec la PLL et l'indice d'origine, MCLK = PLL / 5 comme les autres, et un rapport MCLK/SCLK de 8 (8, 11,025, 12 kHz), 4, ou 2 (128 kHz) ; le cœur B les laisse passer sans suréchantillonner. La version normale, reconstruite sans l'option, garde le CRC de la 0.78.

**Fichiers d'essai** (`work/rate_test/`, sur l'appareil dans `test_rates/`) : un WAV par fréquence, 44,1 kHz d'abord comme référence, puis de 8 à 192 kHz : 1 s de son pur à 1 kHz (−12 dBFS), 1 s de silence, puis 5 s de « One More Time » (Daft Punk, bibliothèque) rééchantillonné par soxr ; 16 bits jusqu'à 64 kHz, 24 bits au-delà. À l'écoute, la note et la musique doivent avoir la même hauteur et la même vitesse partout ; le journal donne les changements de fréquence, les erreurs I2C et les manques.

**Résultat de `NEWFWRT1` sur l'appareil** (journal `work/hwlog_1025_rt1/`) :
- **justes** (son pur et musique à la bonne hauteur et à la bonne vitesse) : 8, 12, 16, 24, 32, 44,1, 48, 88,2, 96, 176,4 et 192 kHz ;
- **11,025 kHz** : aucun son ; l'anneau du DAC ne se vide plus (la piste suivante attend 26 s) : pas d'horloge I2S ;
- **22,05 kHz** : pas de son pur, la musique n'arrive qu'après ~5 s ; vidage très lent ;
- **64 kHz** : lecture trop lente (2,9 s d'audio jouées en 65 s) ;
- **128 kHz** : lecture trop lente (7 s jouées en 23 s, au lieu de 6,75 s à 96 kHz) ;
- aucune erreur I2C, aucun manque.

Les quatre fréquences fautives sont exactement celles des deux entrées de PLL qu'aucune autre n'utilise : 28 224 kHz `3 18 25 3` (11,025, 22,05) et 81 920 kHz `0 96 25 8` (64, 128). Ces entrées des tables d'origine ne donnent pas la fréquence annoncée sur ce matériel. Toutes les autres PLL sont justes ; les indices des DAC (0, 1, 2) aussi.

**`NEWFWRT2`** (CRC `0xa645fdab`) : les quatre fréquences reprennent des PLL éprouvées, seul le rapport MCLK/SCLK de l'I2S change : 11,025 kHz = 56,448 MHz, MCLK 1024 fs (rapport 16) ; 22,05 kHz = 56,448 MHz, 512 fs (8) ; 64 kHz = 40,96 MHz, 128 fs (2) ; 128 kHz = 122,88 MHz, 192 fs (3).

**Résultat de `NEWFWRT2` sur l'appareil** (journal `work/hwlog_1025_rt2/`) : **les 15 fréquences sont justes à l'écoute** (son pur et musique à la bonne hauteur et à la bonne vitesse), aucune erreur I2C, aucun manque. De 24 à 192 kHz, chaque fréquence reste réglée 7,0 s, la durée exacte des fichiers.

**Défaut du lecteur révélé par l'essai** : en dessous de 24 kHz, le changement de fréquence vers la piste suivante arrive trop tôt (après 3,0 s à 8 kHz, 4,7 s à 11,025, 5,0 s à 12, 6,0 s à 16, 6,8 s à 22,05 kHz), alors que le journal annonce l'attente de toute la fin de la piste (« waits for 6016 ms » à 8 kHz). L'attente du vidage de l'anneau avant un changement de fréquence (`drain_step`, et deux boucles de `player_chain_end`) abandonne au bout de **2 s fixes** ; or l'anneau de 192 Kio dure 1,1 s à 44,1 kHz mais 6,1 s à 8 kHz en 16 bits : la fin des pistes à basse fréquence était coupée (jusqu'à 4 s à 8 kHz). Le défaut n'existe pas en 0.78, où ces fréquences sont suréchantillonnées (l'anneau dure alors au plus 1,1 s), mais il faut le corriger avant de les jouer directement : délai = durée de l'anneau à la fréquence en cours, plus une marge.

**0.79 (validée par l'utilisateur)** :
- les 15 fréquences de `NEWFWRT2` passent dans la table normale du pilote (`audio.c`) ; le cœur B (`rt.c`) les laisse toutes passer : sortie sans rééchantillonnage de 8 à 192 kHz. Le suréchantillonneur de la 0.78 reste en secours pour les fréquences hors de cette liste (7,35 kHz, 37,8 kHz…) ;
- le délai d'attente du vidage de l'anneau (`drain_step`, `player_chain_end`) devient la durée de l'anneau à la fréquence en cours plus 1 s (`drain_limit_us`) : 2,1 s à 44,1 kHz, 7,1 s à 8 kHz en 16 bits ;
- l'option `RATE_TEST` disparaît ; image `work/NEWFW079.IMG`, CRC `0x2e494990`.

**Essai sur l'appareil de la 0.79** (journal `work/hwlog_1025_079/`) : aucune faute, aucune erreur I2C, démarrage à 200 MHz (un redémarrage : sortie normale de l'écran de charge au débranchement).
- `test_rates/` : **chaque fréquence dure 7,0 s**, de 8 à 192 kHz : plus aucune fin de piste coupée.
- **ALAC avec notre décodeur (0.77)** : 16 bits stéréo **21 à 25 MHz** au palier de 50 MHz (41 MHz avec celui d'Apple en 0.76) ; mono 11 à 13 MHz ; **24 bits 96 kHz : 49 à 51 MHz**, aux paliers de 67 et 80 MHz (108 MHz et 200 MHz en permanence en 0.76). Le gain mesuré (−45 à −55 %) suit celui du banc QEMU (−50 et −59 %).
- Fichiers abîmés : sans faute.
- Le HIFIACC pour le prédicteur de l'ALAC n'est donc plus nécessaire : l'ALAC 16 bits est au palier le plus bas.

## 61. Musepack (0.80, 2026-10-25)

**Inventaire** : la bibliothèque beets contient 183 fichiers Musepack (13 albums), tous en SV7, 44,1 kHz stéréo ; aucun autre format non lu. La feuille de route classait le Musepack « non envisagé, faute de décodeur permissif » : c'était faux, la bibliothèque de référence **libmpcdec** est sous licence BSD 3-clause et a un mode virgule fixe.

**Banc QEMU** (`re/mpc/bench/`, local) : libmpcdec r475 en `MPC_FIXED_POINT`, deux extraits SV7 de la bibliothèque (~205 et ~182 kb/s) : **11,8 M instructions par seconde d'audio** (moins que le MP3 : 14,7 M), 109 Kio de tas (dont 64 Kio de tampon du démultiplexeur) ; écart avec ffmpeg 2 LSB au plus (62,6 et 79,1 dB) ; 481 échantillons de décalage avec ffmpeg, le retard du décodeur que libmpcdec retire. Profil : filtre de synthèse 68 %, lecture du flux 6 %. Deux ajustements de compilation sans toucher aux sources : `-fcommon` (les en-têtes déclarent des tables sans `extern` ; sur ARM la vraie définition l'emporte bien sur la commune), et des remplaçants pour ce que la bibliothèque prend à la bibliothèque C hors décodage (`pow`, `log10`, `sprintf`, `abs`, le CRC32 du SV8).

**Intégration** (conception validée par l'utilisateur) :
- `third_party/libmpcdec` (r475, non modifié, sans `mpc_reader.c` qui utilise stdio) ; image `HP_CODEC_MPC` (9), `fw/hifi/codec_mpc.c` : 41 Ko de code. Le lecteur de libmpcdec lit l'anneau d'entrée ; sortie 16 bits empaquetée ; déplacement par `mpc_demux_seek_sample` à l'ouverture (SV7 : relecture des en-têtes de trame depuis le début ; SV8 : la table de recherche) ;
- **retour en arrière dans le protocole** (`rewind_pos`, `rewind_seq`, `rewind_ack` de `hp_ctl`) : le SV8 lit sa table de recherche en fin de fichier (`SO` → `ST`) puis revient au début ; `in_seek()` vers une position antérieure le demande au cœur A, qui vide l'anneau et reprend sa lecture à cette position ;
- tags **APEv2** (`tags.c`) : pied « APETAGEX » dans les 32 derniers octets (ou avant un ID3v1), éléments Title, Artist, Album, Track, `REPLAYGAIN_*` ;
- extensions `mpc`, `mp+`, `mpp` ; « MPC » à l'écran.

**Essais dans l'émulateur** :
- SV7 (extrait de 1,5 Mo) : sortie **identique au bit près** au banc (883 103 trames) ; lecture jusqu'à la coupure du fichier ;
- fichier complet (The White Stripes, « Hypnotize ») : tags APEv2 et ReplayGain d'album lus (11 ms) ; déplacement de 5,8 à 33,6 s exact à l'échantillon ; ouverture du déplacement 540 ms en SV7 (590 Ko relus pour parcourir les en-têtes) ;
- le même converti en **SV8** par `mpc2sv8` (outil des sources Musepack, compilé sur le PC) : table de recherche en fin de fichier, retour en arrière à l'ouverture, sortie identique au SV7 ; déplacement par la table : ouverture en 112 ms.
- fichiers abîmés (variantes de `damage.py` d'un SV7 de 108 s, avec un déplacement vers la zone abîmée) : en-tête détruit, zéros, aléatoire → refusés ; tronqué → arrêt à la coupure ; zéros ou aléatoire ajoutés → lu jusqu'à la vraie fin ; **milieu abîmé → la piste s'arrête au dommage** (une trame SV7 n'a pas de synchronisation, seulement la longueur de la suivante : libmpcdec ne peut pas reprendre ; l'erreur est traitée comme la fin de la piste) ;
- déplacement en SV7 : ~0,7 s de blanc vers 50 s et ~1,6 s vers 100 s dans l'émulateur (relecture des en-têtes depuis le début) ; une table de positions tenue par le cœur A pendant la lecture l'éviterait (à faire si la gêne se confirme).

**Sur l'appareil** (`work/hwlog_1026_080/`) : charge du cœur B 28,4 à 29,1 MHz en moyenne sur un morceau entier, 45,7 MHz au plus, au palier de 50 MHz ; tags APEv2 et ReplayGain d'album lus ; SV8 enchaîné sans blanc, même longueur exacte que le SV7 ; les variantes abîmées se comportent comme dans l'émulateur. Déplacement en SV7 : 0,05 à 0,15 s d'ouverture même vers 100 s (1,6 s dans l'émulateur, dont la carte est lente) : pas besoin de table de positions ; SV8 : 0,06 s.

**Journal « queued behind » faux** (vu en 0.75 : « 24346666 ms ») : après un refus lent (un fichier de zéros parcouru sur 3 Mio, 2,6 s), la fin de la piste précédente était déjà jouée, mais `tail.on` restait levé et `tail.end - audio.rd` négatif s'affichait en non signé. La fin de piste est désormais abandonnée si elle est dépassée à l'ouverture suivante ; reproduit sur la 0.79 dans l'émulateur (A.WAV, deux MP3 refusés, D.WAV), disparu en 0.80.

Image `work/NEWFW080.IMG`, CRC `0xfe8387af`.

**Nuit d'autonomie (2026-10-26, `work/hwlog_1026_night079/`)** : l'image en place était la 0.80 avant la correction du journal (CRC `0x541df300`), et non la 0.79 prévue. 7,9 h de lecture continue depuis la carte SD (1000mods, Daft Punk… : 46 MP3, 38 Vorbis, 11 FLAC), écran éteint, 92 pistes sans aucune erreur ni sous-remplissage ; cœur B 40 MHz en moyenne. Batterie de 4 179 à 3 637 mV. Sur la même plage de tension que la nuit du 24 octobre (0.61, MP3 320 kb/s, cœur B 57 MHz en moyenne) : 4 100 → 3 750 mV en 4,82 h contre 4,62 h (72,7 contre 75,8 mV/h), 4 072 → 3 710 mV en 5,28 h contre 4,88 h (68,5 contre 74,1 mV/h) : 4 à 8 % de mieux, avec un mélange de formats plus lourd en moyenne. La fin de la courbe (sous 3,64 V, jusqu'au seuil d'arrêt de 3,3 V) reste à mesurer : l'autonomie totale n'est donc qu'estimée, autour de 10 h.

## 62. microVorbis : défauts de Tremor lowmem, protections, banc de fuzzing (0.81, 2026-10-26)

**Source** : [micro-vorbis](https://github.com/esphome-libs/micro-vorbis) et [micro-ogg-demuxer](https://github.com/esphome-libs/micro-ogg-demuxer) (esphome-libs, Apache-2.0 ; Tremor fork BSD). Leur code vise l'ESP32 (Xtensa, PSRAM) et ne fait rien gagner au Cortex-M3 ; leur liste de modifications (`src/tremor/CHANGES.md`) recense des défauts de lowmem. Rien n'est recopié : les corrections ci-dessous sont écrites ici d'après cette liste et d'après libvorbis.

**Blocs de 8192 échantillons** : `mdct_step8`, cas 0, écrivait en `x+5, x+6` au lieu de `x+6, x+7` (corrigé). Ce cas ne sert qu'aux blocs de 8192, que Tremor accepte seulement à 64 kHz et plus. libvorbis 1.3.7 n'en produit à aucune fréquence ni qualité ; les 964 Vorbis de la bibliothèque sont tous en 256/2048. Un fichier construit avec un libvorbis modifié (96 kHz, 256/8192) reste faux même corrigé (12,5 dB contre ffmpeg, 0,6 dB avant) : lowmem a d'autres défauts à cette taille. **Décision de l'utilisateur : ces blocs ne sont pas pris en charge**, refusés à l'ouverture (`info.c`, `blocksizes[1] > 4096`), comme le floor 0.

**Floor 0 : faute du cœur B** : la bibliothèque contient 18 fichiers en libVorbis 20000508 (bêta de 2000, floor 0 ; Les Vieilles Salopes, « Aime-moi tendre, aime-moi vrai »). Leur refus (`ECHO_NO_FLOOR0`) passait par `vorbis_info_clear()`, qui libérait les entrées de `floor_param` jamais remplies : de la mémoire non initialisée de `heap.c`. Dans l'émulateur, lecture hors mémoire du cœur B ; sur le PC avec `heap.c`, faute dans `floor1_free_info` (pointeur `0x30002f002e002d`). Correction : les tableaux de la configuration (`floor_param`, `floor_type`, `residue_param`, `map_param`, `mode_param`) en `calloc`, et l'échec d'allocation contrôlé. En 0.81, ces fichiers sont refusés proprement (« Cannot play »).

**Protections du dictionnaire** (`codebook.c`), contre les en-têtes de configuration malformés :
- `dim` entre 1 et 32 (les vecteurs du décodage sont sur la pile de 8 Kio du cœur B ; les encodeurs réels : 8 au plus), et la borne de libvorbis `ilog(dim) + ilog(entries) <= 24` ;
- `_make_words` borné à la taille de son tableau, refus des arbres incomplets (sauf un seul mot) ;
- longueurs de mots de 32 bits au plus en mode ordonné (sinon `marker[33]` déborde), et pas plus de mots qu'une longueur n'en permet ;
- `_book_maptype1_quantvals` sans débordement (produits saturés) ni boucle sans fin ;
- décalages de 32 bits ou plus refusés (`decpack` à la configuration, `decode_map` au décodage) ;
- index du type 3 borné ; un parcours sorti de l'arbre (`-1`) est une erreur de paquet ;
- toutes les allocations contrôlées.

Et dans `floor1.c` : une abscisse en double dans la liste des points est refusée (comme libvorbis : sinon division par zéro dans `render_line`).

**Banc de fuzzing** (`re/fuzz/`, local) : `fuzz_host.c` exécute sur le PC nos vrais `oggpage.c`, `codec_vorbis.c` (Tremor) et `codec_opus.c` (libopus), avec ASan et UBSan (hors décalages de valeurs signées et débordements volontaires de Tremor), sur un bouchon du runtime du cœur B. Guidé par la couverture comme AFL, avec gcc seul (`-fsanitize-coverage=trace-pc`, table partagée de 64 Kio) ; chaque entrée dans un processus enfant (statiques neuves, plantages et boucles sans fin détectés) ; mutations surtout dans les 12 premiers Kio (les en-têtes). Graines : six Vorbis et cinq Opus de la bibliothèque réemballés sans tags (`ffmpeg -c copy`, 6 s, 40 Ko), deux mono et le début d'un fichier floor 0. Résultats en 25 minutes par banc :

| Banc | Exécutions | Plantages | Boucles sans fin |
|---|---|---|---|
| pages Ogg seules | 402 498 | 0 | 0 |
| Opus | 80 498 | 0 | 0 |
| Vorbis 0.80 (sans protections) | 54 721 | 5 059 | 4 |
| Vorbis protégé | 221 161 | 325 | 0 |

Sur 400 plantages tirés de la version 0.80 : débordements de tas dans `_make_words` (356), libération d'un pointeur non initialisé après un en-tête refusé (28), décalages de 32 bits ou plus (4), `marker[33]` (1), divisions par zéro (residue, floor 1). Les 325 de la version protégée avaient tous la même cause, la division par zéro du floor 1, corrigée ensuite ; les 4 boucles sans fin ne se reproduisent plus. Sur Cortex-M3, une division par zéro donne 0 (sans `DIV_0_TRP`) : sur l'appareil, ce cas aurait donné un son faux plutôt qu'une faute.

**Seconde campagne** (version protégée, deux fois 25 minutes, graines enrichies de 60 anciens plantages) : deux nouveaux cas.
- `floor1_info_unpack` : un en-tête qui finit trop tôt donne `partitions = -1`, puis `malloc(-1)`. Refusé désormais. Et dans notre `heap.c`, `malloc(0xffffffff)` débordait dans son calcul de taille (`n + 7`) et rendait un bloc de 8 octets : toute demande plus grande que le tas est maintenant refusée.
- Une lecture avant le tableau des résidus : un artefact du banc. `submap.residue` est un `char`, signé sur le PC, non signé sur ARM, où le contrôle `>= ci->residues` rejette bien 255. Le banc est maintenant compilé en `-funsigned-char`, comme l'ARM.

**Troisième campagne** (deux Vorbis, Opus et Ogg, 25 minutes chacun, en `char` non signé) : Ogg (361 680 exécutions) et Opus (77 138) toujours sans plantage ; deux défauts de plus dans le décodage de l'audio Vorbis :
- `render_line` lisait hors de `FLOOR_fromdB_LOOKUP` (256 valeurs) : avec `mult = 3`, la plage permise va jusqu'à 86 × 3 = 258. Les deux extrémités de chaque segment sont bornées à 0-255 (les points entre elles le sont alors aussi ; rien ne change pour un fichier valide) ;
- `res_inverse` : un mot de partition décodé au-delà de `partitions^dim` indexait `stagemasks` hors du tableau. Comme libvorbis : à l'ouverture, le dictionnaire des partitions doit pouvoir coder tous les mots (`partitions^dim <= entries`, sinon refus) ; au décodage, un mot `>= partvals` termine le paquet.

Les 5 390 plantages trouvés en tout (dont 5 059 de la 0.80) passent tous sans erreur sur la 0.81.

**Sortie inchangée** : 26 morceaux complets de la bibliothèque décodés sur le PC avant et après les protections : identiques au bit près. Dans l'émulateur, l'image ARM 0.81 donne la même sortie que la 0.80 sur un fichier Vorbis (441 000 trames, identiques au bit près).

**Quatrième campagne** (0.81 définitive, deux fois 25 minutes) : 433 945 exécutions, aucun plantage ni boucle sans fin.

**Image ARM bloquée, faute de dépendances** : une image construite par étapes restait bloquée à l'ouverture de tout fichier Vorbis dans l'émulateur (le cœur A abandonne au bout de 3 s), alors que le PC décodait bien. Deux variantes (sans le contrôle des résidus, sans le changement de `heap.c`) échouaient de même. Cause : le champ `partvals` ajouté à `vorbis_info_residue` (`codec_internal.h`) ; le Makefile ne fait pas dépendre les objets de Tremor de leurs en-têtes : seul `res012.o` avait été recompilé, `info.o` et `mapping0.o` gardaient l'ancienne taille de la structure. Après `rm -rf build/tremor`, l'image passe : floor 0 et blocs de 8192 refusés proprement, fichier ordinaire identique au bit près à la 0.80. Image `work/NEWFW081.IMG`, CRC `0x8deb4265`. Correction proposée (à valider) : des dépendances générées (`-MMD -MP`) pour tous les objets du Makefile.

**Outils publiés** : `fw/tools/fuzz/` (`fuzz_host.c`, `build.sh` avec les cibles `ogg`, `vorbis`, `opus` et `vorbis-heap`, `mkseeds.sh` qui fabrique des graines de synthèse ou à partir d'un dossier de musique), et la page `docs/FUZZING.md`. Les graines et les fichiers trouvés restent hors du dépôt (extraits de musique).

**À étudier plus tard** (feuille de route) : micro-mp3 (dérivé du même décodeur OpenCore que notre MP3) et micro-flac, de la même façon.

## 63. Fuzzing de l'AAC, du MP3, du conteneur MP4 et des tags (0.82, 2026-10-26)

**Cibles ajoutées au banc** (`fw/tools/fuzz/`, §62) : `mp3` (`codec_mp3.c` et pvmp3dec), `mp4` (`src/mp4.c`, cœur A : `mp4_open()`, puis `mp4_feed()` jusqu'au bout, puis une seconde ouverture avec un déplacement au milieu), `aac` (`mp4.c`, puis `codec_aac.c` et pvaacdec sur le flux HP4P qu'il fabrique, à travers le vrai anneau `HP_IN_BASE`), `aacpkt` (`codec_aac.c` seul, sur un flux HP4P muté directement, configuration AAC comprise), `tags` (`src/tags.c` pour tous les formats). Les décodeurs PacketVideo sont compilés en C++ comme dans le Makefile, mais sans `PV_ARM_GCC_V5` (opérations en C portable, pas d'assembleur). Les mutations des cibles MP4 visent la boîte `moov`, où qu'elle soit ; celles de `tags` aussi les 512 derniers octets (ID3v1, APEv2). Graines : `mkseeds.sh` étendu (MP3 de synthèse en MPEG-1, 2 et 2.5, CBR et VBR, avec et sans Xing, avec ID3v2 et ID3v1 ; M4A de 8 à 96 kHz, `moov` au début et à la fin) ; flux HP4P obtenus par `FUZZ_STREAM`.

**Fidélité du banc** : sortie du PC contre l'image ARM dans l'émulateur. AAC : identique au bit près (fichier de synthèse et fichier iTunes). MP3 : 1 LSB au plus d'écart (les opérations ARM de pvmp3dec contre leurs versions C). Deux ajustements : `trans4m_freq_2_time_fxp.cpp` sans ASan (une boucle en pipeline lit l'élément précédant une table à son dernier tour, valeur jamais utilisée) ; les décalages de 32 bits ou plus non contrôlés dans les décodeurs PV (`ms_synt.cpp` en fait sur des fichiers valides ; sur ARM, décalage arithmétique : le signe, ce que veut le code).

**Première série** (25 minutes par cible) :

| Cible | Exécutions | Plantages | Boucles sans fin |
|---|---|---|---|
| `mp4` (deux) | 682 290 | 0 | 0 |
| `tags` (deux) | 614 384 | 0 | 0 |
| `aac` | 213 286 | 1 278 | 0 |
| `aacpkt` | 150 129 | 3 690 | 0 |
| `mp3` (deux) | 271 391 | 403 | 0 |

Notre code du cœur A (`mp4.c`, `tags.c`) : rien. Les défauts sont tous dans les décodeurs PacketVideo.

**Défauts et corrections** (écrites ici, marquées `ECHO MINI (ETUDE §63)`) :
1. **AAC, TNS** (`get_tns.cpp`) : `top -= longueur` devenait négatif ; rangé dans l'entier non signé `tempInt`, −33 donnait 4 294 967 263, que la macro `SCALE_FACTOR_BAND_OFFSET` (qui ne protège que ≤ 0) laissait passer. Sur ARM 32 bits, la lecture retombe 68 octets avant la table : pas de faute, mais une borne de filtre arbitraire, utilisée ensuite par `apply_tns` pour écrire. Correction : `top` borné à 0 (bande de départ `max(top - longueur, 0)`, comme la norme et FAAD2).
2. **AAC, impulsions** (`getics.cpp`, `pulse_nc.cpp`) : `pulse_start_sfb` (6 bits, jusqu'à 63) au-delà des bandes : lecture hors de la table des bandes puis écritures hors de `coef[]`. Correction, comme FAAD2 : refus si `pulse_start_sfb` dépasse le nombre de bandes, et arrêt de toute impulsion au-delà de la dernière bande.
3. **AAC, quantification inverse** (`huffspec_fxp.cpp`) : `max > 8192` laissait passer 8 192 (index 1 025 d'une table de 1 025) ; et `max` ne comptait pas les valeurs laissées dans `quantSpec` par une trame interrompue (−12 185 lu avec `max` = 16). Correction : `max` recalculé sur les 1 024 coefficients (`LN2`) juste avant le contrôle, borné à 8 191 (environ 0,1 MHz du cœur B).
4. **AAC, échappement** (`unpack_idx.cpp`) : le préfixe d'échappement n'avait pas de limite ; arrêté à N = 12 comme la norme (valeurs < 8 192) ; un flux valide se lit à l'identique.
5. **MP3, régions de Huffman** (`pvmp3_huffman_parsing.cpp`) : `region0_count + region1_count + 2` jusqu'à 24 dans `l[23]` (lecture du champ voisin `s[]`, sans danger mémoire) ; index borné à 22, comme ffmpeg.
6. **MP3, gain des sous-blocs** (`pvmp3_dequantize_sample.cpp`) : fenêtre calculée jusqu'à 4 dans `subblock_gain[3]` et `scalefac->s[3][…]` ; bornée à 2.

Les défauts 1 et 2 écrivaient hors des tableaux sur l'appareil ; 3 et 4 lisaient hors d'une table ; 5 et 6 donnaient seulement des valeurs fausses sur des trames déjà corrompues.

**Contrôles** : les 5 371 plantages rejoués sans erreur ; 34 M4A (toute la bibliothèque) et 16 MP3 tirés au hasard décodés avant et après les corrections : identiques au bit près.

**Seconde série** (après ces corrections, 25 minutes par cible) : `aac` 247 062 exécutions et `mp3` deux fois (254 987), sans plantage ; `aacpkt` 242 424 exécutions, 72 plantages, deux causes plus profondes, que les précédentes masquaient :
7. **Configuration AAC lue au-delà de son tampon** (`codec_aac.c`, notre code) : `cfg[64]` sur la pile ; un « program config element » (PCE) peut annoncer 255 octets de commentaire et des dizaines d'éléments, et le lecteur de bits de pvaacdec n'est pas borné (`get_prog_config.cpp`). Lectures seules, de valeurs arbitraires. Correction : un tampon statique de 64 + 1 024 octets remis à zéro à chaque ouverture (une configuration de plus de 64 octets reste refusée).
8. **PNS et stéréo d'intensité** (`pns_intensity_right.cpp`) : pour le canal droit, les informations de fenêtres et un tableau de groupes d'une autre séquence de fenêtres (`{1}` d'une fenêtre longue contre 8 fenêtres courtes) : la boucle lisait des groupes au-delà du tableau, et ses pointeurs sortaient de la mémoire du décodeur (`mem` de `codec_aac.c`). Correction : arrêt dès qu'un groupe ne progresse pas ou dépasse `num_win` (dans un flux valide, ils croissent jusqu'à `num_win`).

Après ces deux corrections : les 3 762 plantages `aacpkt` et les 1 278 `aac` rejoués sans erreur ; les 34 M4A toujours identiques au bit près.

**Troisième série** (0.82 définitive) : `aacpkt` deux fois (507 574 exécutions) et `aac` (261 817), aucun plantage ni boucle sans fin. Dans l'émulateur, l'image ARM 0.82 (`work/NEWFW082.IMG`, CRC `0xbbf146bd`, construite à neuf) donne la même sortie que la 0.81 sur un MP3 et deux M4A (504 259 trames, identiques au bit près).

**Décharge complète (fin de l'essai d'autonomie, 0.80, `work/hwlog_1027_endurance/`)** : la même session que la nuit du 26 (démarrage à 4 179 mV, 9 minutes sur l'USB à 7,9 h), poursuivie jusqu'à l'arrêt automatique : **15,8 h**, dont 15,5 h de lecture (180 pistes depuis la carte : 89 MP3, 76 Vorbis, 22 FLAC ; cœur B 40 MHz en moyenne), sans erreur ni trou. Après l'USB : 3 600 mV à 10,2 h, 3 500 mV à 13,7 h, alerte « battery: low (3399 mV) » à 15,38 h, arrêt « battery empty » à 3 281 mV à 15,79 h (24,5 minutes après l'alerte) ; la relance suivante est refusée (« battery empty at start », 3 347 mV). Autonomie en lecture de l'ordre de 15 h, écran éteint.

## 64. Corrections de pvmp3dec d'après micro-mp3 ; micro-flac (0.83, 2026-10-27)

**micro-mp3** (esphome-libs, Apache-2.0, dérivé du même décodeur OpenCore) : sa liste de modifications (`src/opencore-mp3dec/CHANGES.md`), vérifiée point par point dans notre pvmp3dec d'Android 11. Déjà réglés : la taille de `Scratch_mem` (198) et `bitrate_index == 15` (Android 11), les régions de Huffman et la fenêtre des sous-blocs (§63). Corrigés ici, écrits d'après la liste et marqués `ECHO MINI (ETUDE §64)` :
1. **Fin de la zone « count1 »** (`pvmp3_huffman_parsing.cpp`) : depuis l'index 574, le dernier groupe de quatre écrivait `is[576..577]`, au-delà de `work_buf_int32[576]`, dans `circ_buffer[]` (l'historique du filtre polyphasé), puis les remettait à zéro. Un groupe n'est décodé que s'il tient (`i <= 572`). Écriture interne à la structure : invisible pour ASan, d'où son absence au §63.
2. **Trame rendue muette** (`pvmp3_framedecoder.cpp`, réservoir vide ou CRC faux) : `overlap[]` est vidé aussi ; sinon la trame muette jouait la fin de la précédente et la suivante s'y superposait (un claquement sur les fichiers abîmés).
3. **MPEG-2.5 à 8 kHz** : la limite des blocs mixtes, le début du réordonnancement et la borne d'intensité MPEG-2 ne sont plus fixées à 36 mais tirées de la table des bandes (72 à cette fréquence).
4. **Bloc mixte sans région courte** (`pvmp3_reorder.cpp`) : retour immédiat, au lieu de réinjecter des valeurs de la trame précédente.
5. **Gain global élevé** (`pvmp3_dequantize_sample.cpp`) : les trois décalages à gauche sont saturés (`sat_shift_left`) ; un `global_gain` légal proche de 255 débordait.

Non repris : l'arrondi au plus proche des multiplications (change la sortie, contredit nos opérations ARM, comme l'arrondi Vorbis écarté), la réduction du tampon du réservoir (6 Ko non nécessaires), l'égaliseur (inutilisé), le seuil de 32 octets des trames (voix à 8 kb/s, à reprendre avec le contrôle des informations latérales de micro-mp3), `pvmp3_header_sync()` (chemin non emprunté par `codec_mp3.c`).

**Contrôles** : les 403 plantages MP3 du §63 rejoués sans erreur ; 60 MP3 de la bibliothèque identiques au bit près avant et après ; image ARM 0.83 (`work/images/NEWFW083.IMG`, CRC `0xe734f314`) identique à la 0.82 dans l'émulateur (un MP3, deux M4A) ; deux campagnes `mp3` de 25 minutes (117 345 et 124 527 exécutions) sans plantage ni boucle sans fin.

**micro-flac** : une réécriture à partir de l'implémentation simple de Nayuki, pas un dérivé de dr_flac : rien à transposer. Ses optimisations (LPC en 32 ou 64 bits selon le risque de dépassement, boucles déroulées par ordre) existent déjà dans dr_flac ; un CRC facultatif est déconseillé (notre resynchronisation des fichiers abîmés s'appuie dessus). Suite logique : une cible de fuzzing `flac` (dr_flac n'a jamais été fuzzé ici), puis `alac` et `mpc`.

**Sur l'appareil (0.83, `work/hwlog/1027_083/`)** : l'album en floor 0 (`test_floor0/`, 18 pistes) refusé proprement à chaque essai : 45 ouvertures refusées, « Cannot play », **aucune faute du cœur B** (en 0.80, une faute à chaque refus). Lecture normale de 43 pistes (Vorbis, MP3, FLAC, Musepack) depuis la mémoire interne et la carte, sans faute ni coupure ; charge du cœur B habituelle (Vorbis 45-55 MHz, MP3 15-23 MHz). Pas encore essayés sur l'appareil : des M4A et les fichiers abîmés MP3/M4A.

## 65. Jauge de batterie recalée sur la décharge mesurée (0.84, 2026-10-27)

La courbe Li-ion générique de `battery_pct()` (`board.c`) sous-estimait fortement la charge restante : 20 % affichés à 3 700 mV, alors qu'il restait 59 % du temps de lecture (environ 9 h). Nouvelle table, tirée de la décharge complète du §64 (part du temps restant jusqu'à l'arrêt, en lecture, cœur B vers 40 MHz) : 3 300 → 0, 3 400 → 3, 3 450 → 8, 3 500 → 13, 3 550 → 22, 3 600 → 36, 3 650 → 50, 3 700 → 59, 3 750 → 65, 3 800 → 70, 3 900 → 79, 4 000 → 89, 4 100 → 96, 4 160 → 100 %. Réserves : entre 3 600 et 3 700 mV, la mesure est un peu faussée par 9 minutes sur l'USB à 7,9 h ; c'est une courbe en charge (en pause, la tension remonte et la jauge est un peu optimiste). Une seconde décharge sans interruption la confirmerait.

**Alerte de batterie faible** à 3 425 mV (au lieu de 3 400) : environ 45 minutes de lecture avant l'arrêt à 3 300 mV (24 minutes auparavant). Le réarmement reste à 3 500 mV.

L'affichage en rouge de la batterie suit le même seuil (`playscreen.c`). Image `work/images/NEWFW084.IMG`, CRC `0xf1ccb555`.

## 66. HIFIACC contre dr_flac : banc de comparaison sur l'appareil (2026-10-27)

**But** : savoir si le filtre LPC de HIFIACC (§1.5) donne exactement les échantillons de dr_flac, à quelle vitesse, et dans quel domaine, avant de l'intégrer au FLAC (puis à l'ALAC). Seul l'appareil peut répondre : dans l'émulateur, HIFIACC est notre propre modèle.

**Ce qu'est HIFIACC** (TRM chapitre 26) : un bloc conçu par Rockchip (« a design of fixed point accelerator »), dans le domaine PD_HIGH avec le cœur B ; aucune trace publique ailleurs. Fonctions : FFT/IFFT 64 à 1 024 points ; MAC 32 × 32 bits à résultat 64 bits, 1 à 1 024 termes ; FIR FLAC (coefficients fixes) et ALAC (coefficients variables), ordre « 1 à 32 » ; décodeur APE (versions 3970/3990) ; FIFO de 32 mots de 32 bits, par DMA ou directement par le processeur ; 192 kHz / 24 bits annoncés à 300 MHz. Le TRM ne donne pas les registres (d'où §1.5), mais il a aussi des chapitres SYNTH (27) et Imdct36 (28), avec leurs cartes mémoire : la base des accélérateurs MP3.

**Banc** (`make EXTRA=-DECHO_HWTEST`, jamais livré) : `fw/hifi/hwtest_lpc.c`, appelé par dr_flac après chaque sous-trame LPC (accroche sous `ECHO_HWTEST` dans `dr_flac.h`). Il reconstruit l'entrée de HIFIACC (amorce, puis résidus exacts en 64 bits), écrit les coefficients en ordre inverse, met les prises inutilisées à zéro, lance HIFIACC et le DMAC2 (canal 1 vers `+0x40`, canal 0 depuis `+0xa0`, listes chaînées par blocs de 2 048 mots), attend la fin (5 ms au plus), compare au résultat de dr_flac, et mesure les deux temps (TIMER1), celui de dr_flac avec ses propres fonctions de prédiction (`drflac__calculate_prediction_32/64`). Compteurs par ordre dans `hp_ctl.hwt`, relevés par le cœur A en fin de piste (`hwt:` dans le journal). La sortie reste celle de dr_flac. Jeu d'essai `work/testsets/test_hifiacc/` (ordres 4 à 32, précision 5 à 15 bits, 16 et 24 bits, mono, blocs de 1 152 à 8 192).

**Mises au point** :
- HW1 : sans liste chaînée (bloc ≤ 2 048), le canal ne lit pas la LLI et garde les adresses de la sous-trame précédente : écritures hors de nos tampons, cœur B qui ne répond plus jusqu'au redémarrage. Les registres du canal reçoivent désormais le premier bloc.
- HW2 : avec une chaîne, le registre LLP doit pointer sur la **première** LLI (la HW2 sautait le premier morceau).
- HW4 : blocs au-delà de 8 191 (champ de taille de 13 bits) en deux passes, la seconde avec pour amorce les dernières sorties de la première : exact.

**Résultats (HW3 à HW5, `work/hwlog/1027_hw3` à `1027_hw5`)** :
- ordres **3 à 32** : toujours **identiques au bit près** (3 884 sous-trames en HW5 : ordres 3, 4-8, 12, 16, 24, 32), en 16 et 24 bits, mono et stéréo, y compris les produits au-delà de 32 bits (le MAC est sur 64 bits), les blocs de 8 191 et les passes enchaînées ;
- ordres **1 et 2** : **toujours faux** (134 et 1 différences, aucune exacte). À l'ordre 1, la sortie s'emballe d'un facteur voisin de 3 × coefficient (comme si le filtre avait au moins trois prises). La remise à zéro des prises inutilisées n'y change rien. **Le module FLAC d'origine (78) fait de même : à `0x42e8`, il calcule les ordres 1 et 2 en logiciel** (`cmp r5,#1 / cmpne r5,#2 / beq` vers `0x3034`). Il ne manque aucune initialisation : sa séquence est la nôtre (taille et `0xa0000000`, ordre et décalage, coefficients inversés, lancement, DMA, attente du bit 15 de `+0x08`). Ces ordres n'apparaissent qu'au début des pistes (introductions quasi silencieuses), pas après un saut ;
- **vitesse** (prédiction de dr_flac contre HIFIACC, DMA compris, mêmes sous-trames) : environ **6 fois plus rapide** en 16 bits à l'ordre 8, **15 à 16 fois** aux ordres élevés et en 24 bits (où dr_flac calcule en 64 bits) ;
- aucun dépassement de délai depuis HW3. Les saccades de la HW4 (cœur B à 197 MHz sur `w24_o32`) venaient du triple calcul du banc, pas de HIFIACC.

**Domaine fiable** : ordres 3 à 32, toute précision de coefficient, 16 à 24 bits, blocs jusqu'à 8 191 (au-delà, en passes). Ordres 1 et 2 : logiciel.

Images : `work/images/NEWFWHW1.IMG` à `NEWFWHW5.IMG`.

## 67. FLAC sur HIFIACC (0.85, 2026-10-27)

Intégration simple, d'après le §66 : `fw/hifi/hifiacc.c` (pilote, partagé en esprit avec le banc), inclus par `codec_flac.c` sous `ECHO_HIFIACC` (défini pour l'image FLAC dans le Makefile).
- Dans dr_flac (`drflac__decode_samples__lpc`), pour une sous-trame d'ordre 3 à 32 : décodage des **résidus seuls** (indicateur `echo_residual_only` : les partitions gardent l'ordre réel, les décodeurs de Rice reçoivent l'ordre 0), puis la prédiction **sur place** par HIFIACC (la sortie suit l'entrée dans la FIFO), en passes au-delà de 8 191 échantillons. Ordres 1 et 2 : dr_flac, comme le module d'origine.
- Dépassement de délai (5 ms) : prédiction logicielle sur place sur les résidus ; après 3 dépassements, logiciel pour le reste de la piste. Compteurs `hacc_sub` et `hacc_soft` de `hp_ctl`, journalisés en fin de piste (« hifiacc: … »).
- Le module FLAC n'a plus que 4,8 Ko de données (les tampons du banc sont réservés à `ECHO_HWTEST`).

Documentation pour d'autres développeurs : `docs/HIFIACC.md`. Image `work/images/NEWFW085.IMG`, CRC `0x2a006a84`.

**0.85 sur l'appareil** (`work/hwlog/1027_085/`) : 2 919 sous-trames sur HIFIACC, aucune erreur ; charge du cœur B sur l'album de 1000mods vers 11 MHz contre 15,5 en 0.80 (environ −28 %, mesure gênée par des écoutes courtes avec sauts). Seul défaut : 3 dépassements de délai sur `w24_o32.flac` (blocs de 8 192 à l'ordre 32), puis le logiciel pour la fin de la piste : HIFIACC tourne à la fréquence du cœur B, environ un produit par cycle, soit à peu près 5 ms pour 8 191 échantillons à l'ordre 32 à 50 MHz. **0.86** : délai porté à 30 ms. Image `work/images/NEWFW086.IMG`, CRC `0xd1462b2`.

## 68. Accélérateurs MP3 IMDCT36 et SYNTH : rétroconception du module 73 (2026-10-27)

**TRM** (chapitres 27 et 28) : blocs distincts de HIFIACC. Adresses côté cœur A `0x62042000` (Imdct36) et `0x62044000` (Synth) ; le cœur B les voit à **`0x60010000`** et **`0x60050000`** (§4 ; RECHO utilise les mêmes). Imdct36 : 54 mots de SRAM (18 entrées, 36 résultats en lecture seule), 4 registres, ROM de 32 coefficients de 20 bits, mode « pré-décalage » (résultat décalé de 2 bits à droite). Synth : SRAM0 de 32 mots (entrées puis résultats), SRAM1 de 1 024 × 20 bits (historique), 5 registres, ROM de 1 024 coefficients de 20 bits ; « canal et phase » réglés par registre.

**Module 73 (MP3 d'origine), désassemblé** (`out/elf/ovl73_codec_mp3.elf`, code à `0`) :

| Adresse | Écrit / lu | Rôle déduit |
|---|---|---|
| `0x60010000` | 18 mots écrits (`memcpy` de 72 octets) | entrées de l'IMDCT ; **le calcul part à l'écriture** (aucun registre de lancement) |
| `0x60010048` | 36 mots lus (144 octets) | résultats |
| `0x60010800` | 3 à l'initialisation, 0 à l'arrêt | marche (et autorisation d'interruption, ou mode « pré-décalage » : à trancher) |
| `0x6001080c` | 0 dans l'interruption | acquittement |
| `0x60050000` | 32 mots écrits puis 32 lus (128 octets) | SRAM0 : sous-bandes en entrée, **échantillons PCM 16 bits** en sortie (lus en demi-mots) ; le calcul part à l'écriture |
| `0x60050800` | 1 à l'initialisation, 0 à l'arrêt | marche |
| `0x60050804` | `canal \| ((15 − phase) << 1)` avant chaque bloc | canal (bit 0) et phase (bits 1 à 4) ; la phase avance de 1 (modulo 16) par bloc de 32 |
| `0x6005080c` | `0x11` dans l'interruption | acquittement |
| `0x60050810` | 0 à l'initialisation et à chaque fin de bloc | état ou acquittement (à trancher) |
| `0x60051000` | 4 096 octets mis à zéro à la remise à zéro du décodeur | SRAM1 : historique de la synthèse |

**Fonctionnement d'origine** : par interruptions et en pipeline. IMDCT36 : boucle des 32 sous-bandes ; à chacune, attente de l'indicateur « terminé » posé par l'interruption, puis en logiciel la fenêtre selon le type de bloc (`0x5648`), le recouvrement-addition (`0x5956`) et l'inversion de signe des sous-bandes impaires, puis la copie des 18 entrées suivantes. **IMDCT36 ne fait donc que la transformée de 36 points.** SYNTH : par blocs de 32 sous-bandes et par canal, il fait la synthèse polyphasée complète, fenêtre et historique compris, et rend directement 32 échantillons PCM ; le logiciel les range en entrelacé (mono dupliqué).

**Pour notre décodeur** (pvmp3dec) : SYNTH remplacerait `pvmp3_poly_phase_synthesis` (avec `pvmp3_polyphase_filter_window` et `pvmp3_dct_16`, environ 38 % du temps au §49) ; IMDCT36 remplacerait la transformée de `pvmp3_mdct_18` (environ 12 %), la fenêtre et le recouvrement restant en logiciel. Formats d'entrée à faire correspondre (pvmp3dec travaille en Q31/Q28 selon les étapes) : c'est l'objet du banc de comparaison (étape 2).

**0.86 sur l'appareil** (`work/hwlog/1027_086/`) : l'album de 1000mods en pistes entières : 19 910 sous-trames sur HIFIACC, aucune en logiciel, aucune erreur ; cœur B **10,8 MHz** sur « Road to Burn » (15,5 en 0.80) et **11,0 MHz** sur « 7 Flies » (16,2) : **environ −30 %** de charge sur le FLAC 16 bits.

## 69. SYNTH contre pvmp3dec sur l'appareil (images de test HS1, HS2, 2026-10-27)

**Banc** (`make EXTRA=-DECHO_HWTEST`, `fw/hifi/hwtest_synth.c`, accroche sous `ECHO_HWTEST` dans `pvmp3_framedecoder.cpp` après `pvmp3_poly_phase_synthesis`) : pour chaque créneau (32 sous-bandes, `work_buf_int32[sb × 18 + créneau]`) et chaque canal, la séquence du module 73 : `+0x804` = `canal | ((15 − phase) << 1)`, les 32 entrées à `0x60050000` (le calcul part), attente de `+0x80c ≠ 0`, acquittement (`+0x80c` = `0x11`, `+0x810` = 0), lecture des 32 sorties (16 bits bas) ; phase par canal avancée de 1 modulo 16 ; historique SRAM1 et phases remis à zéro à chaque piste ; comparaison au PCM de pvmp3dec. Un décalage d'entrée différent par piste (le numéro d'essai est gardé par le cœur A à travers les redémarrages du cœur B).

**Résultats** (`work/hwlog/1027_hs1b`, `1027_hs2`) :
- **fin du calcul** : `+0x80c` passe de 0 à 1 ; plus de 8 millions de créneaux sans dépassement de délai (200 µs), sans blocage : l'interrogation remplace l'interruption du module d'origine ;
- signe et ordre directs (corrélation positive, l'ordre inverse n'est pas meilleur) ;
- **échelle** : sans décalage, SYNTH rend 1/16 de pvmp3dec (et 1/64, 1/256 avec 2, 4 bits à droite) : il attend des entrées **16 fois plus grandes** ;
- **avec `x << 4`** : rapport d'amplitude 1,000 ; sur « Daftendirekt », **99,996 % des 14,5 millions d'échantillons à 1 LSB près, écart maximal 2 LSB**, 63 % identiques (une autre piste : 2 LSB au plus). Décalages 2, 3, 5 : écarts de milliers de LSB.

**Conclusion** : SYNTH remplace la synthèse polyphasée de pvmp3dec (`pvmp3_poly_phase_synthesis`, avec la fenêtre et les DCT de 16 points, environ 38 % du temps au §49) à 2 LSB près, comparable à l'écart de pvmp3dec avec ffmpeg (1 LSB). Reste à mesurer son temps et à étudier IMDCT36, puis l'intégration.

Images : `work/images/NEWFWHS1.IMG`, `NEWFWHS2.IMG` (version « 0.86hs2 »).

## 70. IMDCT36 et temps de SYNTH (HS3) ; SYNTH intégré au MP3 (HM1, 2026-10-27)

**HS3** (`work/hwlog/1027_hs3/`, 8 extraits d'une minute de *Discovery*, `work/testsets/test_mp3acc/`) :
- SYNTH à `x << 4` : 2 LSB au plus sur chaque extrait ; son temps mesuré (888 µs pour 18 créneaux contre 996 µs pour pvmp3dec) incluait les comparaisons du banc et ne disait rien ;
- IMDCT36, comparé à une IMDCT 36 points exacte (64 bits, table générée sur le PC) : **`+0x800` = 1** donne la transformée à l'échelle de l'entrée (rapport 1,0000 sans décalage, ×16 avec `x << 4`) ; **`+0x800` = 3** la divise par environ 4 096 (le mode « pré-décalage » choisi par le module 73 pour son format Q28). Seulement 27 % des sorties à 1 LSB de la référence (coefficients sur 20 bits) ; **`+0x80c` ne change jamais** pour IMDCT36 : aucun indicateur de fin trouvé (les résultats étaient justes après 200 µs). `pvmp3_mdct_18` ne prend que 16 µs, fenêtre comprise, et son historique a un format propre que relisent les blocs courts : intégrer IMDCT36 imposerait de réécrire toute l'étape IMDCT pour un gain incertain (environ 12 % du MP3). **Décision de l'utilisateur : SYNTH seul** ; IMDCT36 plus tard, si cela vaut la peine.

**HM1** (`make EXTRA=-DECHO_HWSYN`, version « 0.86hm1 », `work/hwlog/1027_hm1/`) : SYNTH **joue** à la place de `pvmp3_poly_phase_synthesis` (`echo_synth_play` dans `hwtest_synth.c`, entrées `<< 4`, attente de `+0x80c`, acquittement, 32 sorties par créneau) ; sur les 100 premières granules de chaque piste, la synthèse logicielle est aussi calculée dans un tampon à part pour comparer. Sur les 8 extraits :
- **charge du cœur B : 20,9 à 23,3 MHz, contre 36,7 à 39,1 MHz en 0.86, soit environ −42 %** ;
- 921 600 échantillons comparés : **99,995 % à 1 LSB près, 2 LSB au plus**, 65 % identiques ;
- 1,3 million de créneaux, aucun dépassement de délai ni erreur ; SYNTH occupe environ 7 % du temps réel.

## 71. MP3 sur SYNTH (0.87, 2026-10-27)

`fw/hifi/mp3synth.c`, inclus par `codec_mp3.c` sous `ECHO_SYNTH` (défini pour l'image MP3 dans le Makefile) : `echo_synth_granule()` remplace `pvmp3_poly_phase_synthesis` dans `pvmp3_framedecoder.cpp` (la séquence validée en HM1, §70). Après 3 dépassements de délai (200 µs par créneau), la piste repasse en logiciel (bref défaut possible : l'historique logiciel n'a pas suivi). `echo_synth_reset()` vide l'historique SRAM1 et les phases au démarrage, dans la remise à zéro du décodeur (ouverture, saut) et quand pvmp3dec rend une trame muette après une erreur. Compteurs `synth_gran` et `synth_soft` de `hp_ctl`, journalisés en fin de piste (« synth: … »). Documentation : `docs/MP3ACC.md`. Image `work/images/NEWFW087.IMG`, CRC `0xc3cc98fa`.

## 72. Sonde par USB (image de test PR1, 2026-10-27)

But : explorer les fonctions sans référence de HIFIACC (FFT, MAC), et plus tard d'autres blocs, depuis le PC, sans réinstaller d'image à chaque essai. RECHO n'a pas de tel mécanisme (ses chaînes ne montrent que des auto-tests : « hifi: self-test », « mp3hw: … » ; « SHELL » est un nom d'écran) ; il utilise IMDCT36 avec un état lisible (« timed out (stat %x / %x) ») : un indice pour plus tard.

**Principe** (`make EXTRA=-DECHO_PROBE`, `fw/src/probe.c`, client `fw/tools/probe.py`) : une boîte aux lettres dans le fichier `PROBE.BIN` (4 Kio, d'un seul tenant) de la mémoire interne. À l'entrée en mode USB, le firmware note ses secteurs ; `msc.c` détourne l'écriture de ses 8 secteurs (une requête, exécutée quand le dernier arrive) et leur lecture (la réponse) ; rien n'atteint la mémoire flash. Côté PC, `O_DIRECT` et `fsync` font passer chaque requête et chaque réponse par l'appareil, sans pilote ni droits d'administrateur. Commandes : informations, lecture et écriture de mots (seulement dans le domaine PD_HIGH vu du cœur A, `0x01000000`-`0x0107ffff` : mémoires du cœur B, HIFIACC, DMAC2, et les accélérateurs MP3 côté cœur A, `0x62040000`-`0x6204ffff`), mise sous tension de PD_HIGH avec les blocs HiFi remis à zéro (cœur B tenu), lancement et arrêt du cœur B (code chargé dans HIGHRAM0, qu'il exécute à partir de 0), installation d'une image de la racine à la sortie du mode USB (les contrôles et l'écriture du menu), puis redémarrage. L'image de test garde le menu d'installation complet.

Image `work/images/NEWFWPR1.IMG` (version « 0.87pr1 »).

Premier essai (2026-10-27, au travers de `PROBE.BIN`, sans pilote ni droits) : le cœur A lit et écrit directement HIFIACC et le DMAC2. Carte mesurée : registres `+0x000`-`+0x0ff` ; **8 Kio de mémoire 32 bits à `+0x1000`-`+0x2fff`** ; `+0x3000`-`+0x8fff` ne retient pas les écritures (mémoires des filtres APE, valeurs instables) ; quatre mémoires de 1 Kio, chacune vue en quatre reflets : `+0x9000` (16 bits, ALAC), `+0xa000` (32 bits), `+0xb000` (16 bits, coefficients du FLAC), `+0xc000` (32 bits) ; au-delà, un reflet. Une exploration à l'aveugle (`fw/tools/hacc_explore.py`) avait repéré le type 2 dans `+0x0c` ; arrêtée quand le SDK de référence (lu localement, rien de recopié) a donné le nom des registres.

## 73. La FFT de HIFIACC (mesurée sur l'appareil, 2026-10-27)

Registres (noms du SDK de référence, valeurs vérifiées sur l'appareil) : `+0x0c` FUN_CFG, bits 31-29 = fonction (APE G 0, APE D 1, **FFT 2**, ALAC 3, MAC 4, FLAC 5 : notre `0xa…` du FLAC) ; `+0x24` FFT_CFG ; `+0x30` MAC_CFG ; `+0x34`/`+0x38` résultat de la MAC (bas, haut). Données de la FFT : **partie imaginaire à `+0x1000`, partie réelle à `+0x2000`** (1 024 mots de 32 bits chacune), résultat à la place.

Séquence qui marche : `+0x24` = `taille << 29 | options` (64 → 0, 128 → 1, 256 → 2, 512 → 3, 1 024 → 4) ; `+0x0c` = `0x40000000`, puis `0x40000003` (lancement) ; puis **`+0x0c` = 0 avant de relire** : tant que la fonction reste choisie, la mémoire appartient au moteur et toute lecture rend la même valeur (piège qui avait trompé l'exploration à l'aveugle). Le calcul est fini avant la requête suivante de la sonde (moins d'une milliseconde) ; `+0x08` reste à 0 (pas d'indicateur de fin sans l'interruption activée).

- **Entrées en ordre bit-inversé, sortie en ordre naturel** (une impulsion au mot 32 d'une FFT de 64 points est la fréquence 1).
- `+0x24` bit 27 = 1 : FFT complète, sans échelle, `X[k] = Σ x[n] e^(−2iπkn/N)` ; bit 27 = 0 : la même divisée par N. **Pas de mode IFFT** : `ifft(x) = conj(FFT(conj x)) / N`.
- `+0x24` bit 20 = 1 : la partie imaginaire de l'entrée est ignorée (FFT d'un signal réel) ; le SDK met les bits 27 et 20 ensemble, d'où un facteur ½ apparent sur une entrée complexe.
- Précision (signaux aléatoires de 15 à 24 bits, 64 à 1 024 points) : écart maximal **0,003 à 0,008 % de la crête** (de l'ordre de −86 dB, cosinus d'environ 14 bits) ; aucun débordement observé jusqu'à 24 bits d'entrée et 1 024 points (sorties de plus de 2^28).
- Pour Vorbis (FFT de 64 et 512 points sur Tremor 32 bits) et l'AAC (512) : utilisable, mais la précision est inférieure à celle des décodeurs logiciels (à mesurer sur l'audio décodé) ; temps de calcul à mesurer depuis le cœur B.

**MAC** : type 4 dans `+0x0c`, `+0x30` = nombre de termes `<< 18` (le registre retient la valeur) ; avec la même séquence, en mémoire ou par la FIFO, le résultat reste à 0. Exploration automatique de la nuit : `fw/tools/mac_explore.py`, `work/hwlog/1027_hacc_mac/`.

Émulateur : la classe `Accel` (`tools/emu_host.py`) modélise la FFT (transformée exacte arrondie, pas exacte au bit près vis-à-vis du matériel) et la mémoire occupée (lecture `0x0badf00d` tant que `+0x0c` choisit la FFT) ; vérifié contre numpy et contre la réponse impulsionnelle mesurée ; LPC inchangé.

**MAC abandonnée** (décision de l'utilisateur, 2026-10-27) : 22 000 essais automatiques sans résultat dans `+0x34`/`+0x38` ; peu utile au firmware (`SMLAL` du cœur B, coût du transfert).

## 74. Vorbis : l'IMDCT de Tremor sur la FFT de HIFIACC (2026-10-27)

1. **Profil** (banc QEMU de `re/vorbis`, enveloppe `--wrap=mdct_backward`, Tremor du firmware) : `mdct_backward` = **52 % du décodage** (q3) et **41 %** (q9) ; un bloc de 2 048 coûte 92 400 instructions, un de 256, 8 900.
2. **Formule** (Tremor compilé sur PC, matrice reconstruite par impulsions) : la sortie de `mdct_backward` (n/2 valeurs, à l'échelle 1) est `out[2m] = y[n/4−1−m]`, `out[2m+1] = y[3n/4−1−m]` de l'IMDCT `y[t] = Σ X[k] cos(2π/n (t + ½ + n/4)(k + ½))` ; par une FFT de n/4 points : `d = w · FFT(w · (X[2m] + i X[n/2−1−2m]))`, `w[m] = e^(−2iπ(m+⅛)/n)`, puis `out[4q] = −Im d[q]`, `out[4q+1] = −Re d[q]`, `out[4q+2] = Re d[n/4−1−q]`, `out[4q+3] = Im d[n/4−1−q]`. Tremor lui-même s'écarte de l'IMDCT exacte de 1 à 2·10⁻⁵ de la crête.
3. **Sur l'appareil, par la sonde**, 90 vrais blocs (vidés du banc) : blocs de 256, **1 LSB au plus** sur la sortie 16 bits ; blocs de 2 048, **4 LSB au plus, 0,06 en moyenne**, 0,3 à 1,1 % au-delà de 1 LSB. Agrandir l'entrée (jusqu'à 2^29,6) ne change rien : l'erreur est relative (les cosinus du matériel). Indicateur de fin : `+0x04` bit 15 activé, `+0x08` bit 15 à la fin (comme le LPC).
4. **Code** : `fw/hifi/hififft.c` (tailles 256 et 2 048, table `fw/hifi/fft_tw.h` de `fw/tools/mkfft_tw.py`, `rbit` pour l'ordre bit-inversé, délai 3 ms puis logiciel, logiciel définitif après 3 délais), accroche dans `mdct_backward` (`third_party/tremor-lowmem/mdct.c`), seulement avec `-DECHO_HWFFT` (le son vient de HIFIACC, ligne « fft: » du journal) ou `-DECHO_HWTEST` (les deux calculés, le son de Tremor, ligne « hwf: » : écarts et temps). La version normale reste identique au bit près.
5. **Émulateur** (FFT idéale) : Vorbis q3 de 129 s décodé en entier, sans faute, **1 LSB au plus** de Tremor (98,7 % exacts).
6. Images : `work/images/NEWFWFV1.IMG` (« 0.87fv1 », banc, CRC `0x5ed51874`) et `NEWFWFX1.IMG` (« 0.87fx1 », le son par HIFIACC, CRC `0x3aa32531`), copiées sur l'appareil ; essais en conditions réelles à faire.

**Premiers essais sur l'appareil, image FV1 (2026-10-28, `work/hwlog/1028_fft/`)** : 6 pistes Vorbis (« Beast In… », 3 entières), **0 délai dépassé** ; matériel (rotations et transferts compris) **14,8 s contre 58,8 s pour Tremor** sur une piste entière, soit environ **4 fois plus rapide** (la MDCT pesant 41 à 52 % du décodage, environ −30 à −40 % de charge attendus). Précision sur la sortie 16 bits : 57 % des échantillons exacts, 37,6 % à 1 LSB, **5,4 % au-delà, max 9 LSB** (plus que sur le q3 du banc : 4 LSB, 1,1 %), probablement une musique plus forte (erreur relative à la crête).

**Image FX1, le son par HIFIACC (2026-10-28, `work/hwlog/1028_fft2/`)** : album « Beast In… » (Vorbis, 44,1 kHz), 4 pistes entières et une vingtaine de lectures courtes (sauts) ; **aucun repli logiciel** (≈ 125 000 blocs). Pistes entières : **34,1 MHz en moyenne**, 97 % du temps au palier le plus bas (50 MHz). Référence estimée sur la même piste d'après FV1 (56,4 MHz, matériel + Tremor ; FV1 − temps matériel ≈ 51,6 MHz ; FV1 − temps Tremor ≈ 37 MHz) : **environ −34 %**. Un manque de 5 376 octets par piste (au démarrage, comme les manques d'une piste en 0.87) ; lors des sauts de piste rapprochés, des manques de 65 à 85 Ko, à comparer avec la 0.87 sur le même usage (pas de référence Vorbis dans ce journal).

**Nuit avec FX1 (2026-10-28, `work/hwlog/1028_fx1_nuit/`)** : 7,6 h de lecture (FLAC, MP3, Vorbis, Musepack), dont 3,45 h de Vorbis en 54 pistes : **1 876 399 blocs sur HIFIACC, 0 repli logiciel** ; **34,6 MHz en moyenne** sur les 39 pistes Vorbis de plus d'une minute. Aucun manque pendant la lecture d'une piste : les vrais manques tombent tous aux changements de piste faits à la main, dans les 23 premières minutes. Les « 1 (9 600 octets) » de chaque fin de piste ensuite étaient le compteur reporté d'une piste enchaînée à l'autre (les statistiques ne sont pas remises à zéro quand le DAC est gardé) ; un départ à froid affichait « underruns during it 4294967295 » (valeurs de départ notées avant la remise à zéro de `audio_start`). Rien d'anormal à l'écoute. Batterie 4 179 → 3 647 mV en 7,3 h.

**0.88 (2026-10-28)** : la FFT par défaut (`-DECHO_NO_HWFFT` pour s'en passer, `-DECHO_HWTEST` pour le banc) ; compteurs de manques par piste, par différence avec des totaux jamais remis à zéro (`audio.ur_all`, `lost_all`, `i2s_all`), et le minimum de remplissage propre à chaque piste, enchaînée ou non. Image `work/images/NEWFW088.IMG` (CRC `0xc865b73a`). **Sur l'appareil** (`work/hwlog/1028_088/`, 22 min, 29 pistes) : 4 pistes Vorbis entières enchaînées à **34,0-34,7 MHz, 0 manque**, ≈ 153 000 blocs, 0 repli ; compteurs à 0 après chaque enchaînement, plus de « 4294967295 ». Restent des manques de 1,6 à 9 Ko aux sauts de piste (13 ms dans l'anneau au moment du saut) et de **87 à 157 Ko après une recherche de position dans un fichier Ogg** (son rendu après 490 à 830 ms), aussi en **Opus** : la recherche de position Ogg, pas la FFT (ROADMAP, à faire).

## 75. Recherche de position dans les fichiers Ogg (0.89, 2026-10-28)

**Constat (0.88, `work/hwlog/1028_088/`)** : après une recherche, 450 à 500 ms de silence en Vorbis, 640 à 830 ms en Opus (87 à 157 Ko de manques), contre 28 à 44 ms pour un saut de piste. Pas la FFT : l'Opus fait pareil. Le nombre affiché dans la ligne « chain: » (« open … (1 123 012 bytes) ») est la position de lecture dans le fichier, pas une quantité lue. Deux causes : le cœur A cherchait par bisection la page d'**une seconde avant** la cible (lectures de 4 Ko, arrêt quand l'intervalle passait sous 4 Ko), puis le cœur B **décodait et jetait** cette seconde (et la page) jusqu'à la cible, même au palier de 200 MHz.

**Choix (avec l'utilisateur)** : pas de position exacte à l'échantillon ; le son repart au début de la page qui précède la cible (inaudible pour l'usage).

**0.89** :
- cœur A (`ogg_page_before`, `fw/src/player.c`) : la dernière page dont le granule est ≤ cible ; l'intervalle part de la page de fin du commentaire (après la couverture, connue du lecteur d'étiquettes) ; une estimation proportionnelle aux granules des bornes (le dernier granule du fichier, lu avant), la moitié dès qu'une estimation n'a pas réduit l'intervalle de moitié ; lectures de 8 Ko ; arrêt **exact** (la première page à granule après la meilleure dépasse la cible). Ligne de journal « player: start at … ms: Ogg page at …, N reads in … us » ;
- cœur B (`codec_vorbis.c`, `codec_opus.c`) : saut sur cette page, sortie jetée jusqu'au premier granule (la page elle-même : le pré-décodage d'Opus), puis lecture ; plus de rejet jusqu'à la cible. La position affichée vient du granule.

**Banc sur PC** (60 fichiers Ogg de la bibliothèque, 600 recherches, simulation de l'algorithme) : la première version (arrêt à 4 Ko, départ à 0) trouvait une mauvaise page 7 fois (cible proche du début : couvertures de 150 à 300 Ko sans granule, jusqu'à 126 lectures) ; la version finale : **0 erreur, 8 lectures en moyenne, 20 au plus**. Les pages de ces fichiers font 14 à 23 Ko (environ 1 s) : le son repart en moyenne 0,47 s avant la cible, 1,02 s au plus.

**Émulateur** (version finale, deux fichiers à couverture de 150 à 300 Ko, recherche de +57 s par le mode recherche) : Vorbis 9 lectures, Opus 6, la page attendue dans les deux cas (0,83 s et 0,19 s avant la cible), lecture jusqu'à la fin sans faute.

**Sur l'appareil** (`work/hwlog/1028_089/`, première version ; `1028_089b/`, version finale ; 52 recherches, aucune erreur) :

| | 0.88 | 0.89 (1re) | 0.89 finale |
|---|---|---|---|
| Vorbis : lectures, bisection | — | 10-19, 17-47 ms | 5-11, 15-41 ms |
| Vorbis : silence | 450-500 ms | 160-233 ms | **166-204 ms** |
| Opus : lectures, bisection | — | 9-20, 16-35 ms | 4-12, 11-42 ms |
| Opus : silence | 640-830 ms | 278-333 ms | **310-339 ms** |

Il reste surtout le décodage de la page qui précède la cible (environ 150 ms en Vorbis, 280 ms en Opus) : piste en prospective (ROADMAP) : le granule de cette page transmis au cœur B, qui démarrerait sur la page de la cible (Opus : 80 ms de pré-décodage, RFC 7845) ; et de l'anneau rempli de silence au moment de la recherche, pour que le court manque ne rejoue pas l'ancien son.


## 76. AAC : les FFT de l'IMDCT de pvaacdec sur HIFIACC (0.92, 2026-10-28)

**Où** : l'IMDCT de pvaacdec (`imdct_fxp.cpp`) garde ses pré- et post-rotations ; seules ses deux FFT changent de place : `mix_radix_fft` (blocs longs, 512 points complexes) et `fft_rx4_short` (blocs courts, 64 points). Accroche par une macro `ECHO_FFT` aux deux appels (marqués « ECHO MINI »), code dans `fw/hifi/aacfft.c` (inclus par `codec_aac.c`). Le pilotage de la FFT (registres, lancement, attente de 3 ms, 3 délais puis logiciel) est sorti de `hififft.c` dans `fw/hifi/haccfft.h`, commun à Vorbis et à l'AAC.

**Contrat de pvaacdec** : `data[]` complexe en place, `*max` = l'OU des |x| en entrée et en sortie, valeur rendue = l'exposant `e` (sortie = DFT × 2^−e). Ses sorties **ne sont pas en ordre naturel** ; HIFIACC rend l'ordre naturel, relu dans les cases de pvaacdec (vérifié sur PC contre les deux fonctions) :
- blocs longs : les fréquences paires 2j dans la première moitié, les impaires 2j+1 dans la seconde (son découpage en base 2) ;
- blocs courts : les chiffres en base 4 de la case, inversés (base 4).

**Échelle** : HIFIACC calcule sans échelle sur 32 bits. L'entrée est décalée juste assez pour que rien ne déborde : aucune sortie ne dépasse la somme des |re| + |im| de l'entrée (borne L1, bien plus serrée que points × crête sur de vrais blocs, donc moins de bits perdus). Les sorties sont ensuite **ramenées à 24 bits**, la plage de la FFT de pvaacdec, la seule pour laquelle le reste de son IMDCT a été écrit et essayé (k bits de plus dans l'exposant ; aucune perte de précision par rapport à pvaacdec, coût négligeable).

**Banc sur l'appareil** (`-DECHO_HWTEST` : les deux FFT calculées, le son de pvaacdec, écarts en LSB de la crête du bloc prise sur 16 bits ; `work/hwlog/1028_hwt1`, `1028_hwt2`) :

| | décalage | écart max | exacts ou 1 LSB | temps FFT, Guerrilla Radio (207 s) |
|---|---|---|---|---|
| hwt1 | sur la crête | 9-15 LSB | — | matériel 7,0 s |
| hwt2 | borne L1 | **6-9 LSB** | **87 %** | **matériel 10,9 s, pvaacdec 24,4 s** |

Vorbis dans la même image (code commun) : 3 LSB au plus, comme en 0.88 ; aucun délai dépassé. Le temps matériel compte les transferts, l'ordre bit-inversé et la relecture rangée.

**Fausse piste** : un « silence vers 1:20 de Testify » entendu avec la 0.92, la 0.92b et une image `-DECHO_FFTCMP` (le son de HIFIACC, comparé à pvaacdec sur une copie : 8 LSB au plus, `work/hwlog/1028_fc1`). Décodage sur PC avec une FFT idéale propre, UBSan sans débordement nouveau. Écoute du fichier sur PC par l'utilisateur : **le silence fait partie de la piste**. `ECHO_FFTCMP` reste disponible pour un prochain doute (version « 0.92fc »).

**0.92 sur l'appareil** (`work/hwlog/1028_092f/`, `1028_092v/`) : l'album AAC (13 pistes, 50 min) enchaîné sans pause : **22,8-23,1 MHz par piste contre 25,0-25,3 MHz en 0.75 (−9 %)**, ≈ 290 000 blocs, 0 repli logiciel, 0 manque aux enchaînements (un manque de 2 560 octets sur un redémarrage manuel de piste, l'anneau à 13 ms). Vorbis inchangé : Claws 34,7 MHz (0.88 : 34,6-34,7), 0 repli, 0 manque. Le gain est modeste : la FFT ne pèse qu'environ 25 % du décodage AAC (profil QEMU, §54), et le cœur B reste au palier de 50 MHz.

Image : `work/images/NEWFW092.IMG` (CRC `0x1851a67c`).

**Fuzzing de `fat.c` (cible `fat`, 0.91)** : 6 instances × 2 h, 8 955 972 exécutions, **0 plantage, 0 blocage** ; la couverture plafonne vers 55-68 entrées après 20 min (graines à varier).

## 77. Musepack : la synthèse sur SYNTH, et un débordement du chargeur de codecs (0.93, 2026-10-28)

**Pourquoi** : le filtre de synthèse pèse 68 % du décodage de libmpcdec (banc QEMU, §61) ; c'est la même synthèse polyphase à 32 sous-bandes que celle du MP3, que SYNTH fait déjà depuis la 0.87 (§68-71).

**Banc sur PC** (`libmpcdec` compilée pour le PC, `--wrap` de `mpc_decoder_synthese_filter_float`, la synthèse de pvmp3dec comme modèle de SYNTH : elle en est à 2 LSB au plus sur l'appareil) : recherche de l'échelle et des signes sur de vrais blocs `Y_L`/`Y_R`. Résultat : pvmp3dec reçoit `Y >> 5`, donc **SYNTH reçoit `Y >> 1`** (pvmp3dec `<< 4`) ; aucun changement de signe, de sous-bande ni de retard. Sur 12 pistes SV7 entières de la bibliothèque (214 millions d'échantillons) : **1 LSB au plus, 57 % exacts**. Une synthèse MPEG standard en double (matriçage `cos((16+i)(2k+1)π/64)`, fenêtre `Di_opt` de libmpcdec / 65 536) donne la sortie de libmpcdec à 1 LSB par `arrondi(somme(Y) / 2^14)`.

**Code** :
- `fw/hifi/synth.h` : le pilote commun (remise à zéro de l'historique, `syn_begin`, `SYN_IO`, `syn_wait`), utilisé par `mp3synth.c` (même logique qu'en 0.87) et `mpcsynth.c` ;
- `fw/hifi/mpcsynth.c` (inclus par `codec_mpc.c`) : par trame et par canal, 36 tranches de 32 échantillons `>> 1` ; la sortie 16 bits rendue à l'échelle de libmpcdec (`<< MPC_FIXED_POINT_FRACTPART`), `codec_run` inchangé ; historique remis à zéro à chaque `codec_open` (piste, recherche) ; un délai dépassé : la trame en logiciel, 3 : le reste de la piste ;
- accroche dans `third_party/libmpcdec/synth_filter.c` (marquée « ECHO MINI ») ; `-DECHO_SYNTH` par le Makefile comme pour le MP3 ; `-DECHO_HWTEST` : les deux calculées, le son de libmpcdec, ligne « hwm: ».

**Émulateur** : classe `Synth` (`tools/emu_host.py`, branchée dans `fw/tools/emu_coreb.py`) : la synthèse standard ci-dessus, `arrondi(somme / 2^13)`, saturée, un historique par canal ; la fenêtre lue dans `synth_filter.c`. À 1 LSB de libmpcdec. Le MP3 passe aussi par ce modèle (avant : mémoire simple, délai dépassé puis logiciel). Scénarios `play-mpc` et `play-mp3` (`work/emu/scenarios.py`, local).

**Débordement trouvé en chemin** : dans l'émulateur, plus aucun MPC ne s'ouvrait depuis la **0.91** (« Cannot play », `open failed … in 0/0`), confirmé sur l'appareil ; le codec du cœur B était identique au bit près à celui de la 0.90. Cause : `hifi_load_own` (`fw/src/hifi.c`) garde une copie des données initialisées du codec (`data_copy`, 1 024 octets) pour un redémarrage sans rechargement ; il ne s'en sert pas quand elles sont plus grandes, mais **les copiait quand même** : les 4 992 octets de Musepack débordaient de 3,9 Ko sur le BSS du cœur A. Présent depuis la 0.80, sans effet visible tant que la zone écrasée ne servait pas pendant l'ouverture ; le réarrangement du BSS de la 0.91 y a placé `own_file` (la copie du fichier joué) : sa taille à 0, donc `in_end = 0`, et libmpcdec voyait un fichier vide. Correction : la copie seulement quand elle tient. **La release 0.92 ne lit pas le Musepack.**

**Sur l'appareil** :
- image de banc « 0.93hwt » (`work/hwlog/1028_093syn/`) : environ 40 millions d'échantillons comparés, **74-76 % exacts, 2 LSB au plus**, 0 délai dépassé ; temps de synthèse sur une piste de 114 s : **SYNTH 8,3 s, libmpcdec 52,1 s** ;
- image « 0.93 » (`work/hwlog/1028_093s/`) : pistes entières enchaînées à **10,3-10,8 MHz** (la même piste : 29,8 MHz avant, **−65 %**), palier de 50 MHz, charge maximale 24 %, 0 délai dépassé, 0 manque pendant la lecture.
