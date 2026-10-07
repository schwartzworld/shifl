<p align="center"><img src="assets/logo/shifl-logo.png" alt="SHIFL" width="360"></p>

# SHIFL 2.3 — démarrage rapide

**SHIFL** transforme le M-VAVE FM-1 en groovebox à jouer en live, pour tous les styles : trois synthés et une batterie de 16 sons sur les touches blanches, 9 moteurs de synthèse, 68 sons rangés par famille (basses, claviers, orgues, nappes, leads, plucks, stabs), 37 kits de batterie (808, 909, trap, phonk, house, techno, UK garage, jungle, amapiano, synthwave, chiptune, ambient…), tes propres samples, ghost notes et ratchets, note repeat, accords sur une touche, 16 effets punch-in, et un écran à la teenage engineering qui montre toujours ce que tes mains peuvent faire. Aucun motif d'usine : tout ce que tu entends, tu le joues.

Le manuel complet (en anglais) : [SHIFL.md](SHIFL.md).

---

## Installer

1. Double-clique **`INSTALL-SHIFL.bat`** dans le dossier SHIFL : il compile le firmware et ouvre l'installateur sur `http://localhost:8766/webapp/installer/`.
2. Dans **Chrome ou Edge**, branche le FM-1 en USB (câble de données, directement, sans hub).
3. **INSTALL**, autorise le MIDI, attends *Done*. Garde la fenêtre noire ouverte jusque-là.

L'éditeur web : `http://localhost:8766/webapp/editor/` (ou **`OPEN-EDITOR.bat`**).

## Un beat en soixante secondes

1. **ALGORITHM** sur la piste **4** (orange, batterie). Les touches blanches jouent 16 sons : **F3 kick**, G3 kick 2, A3 snare, B3 clap, **C4 charley**… **PRESETS** choisit le kit : essaie *808* ou *BOOMBAP*.
2. **REC** : *rec ready*. **Joue librement, à ton tempo** — pas de clic, pas de décompte. Garde **OCT−** enfoncé en frappant pour des ghost notes, **OCT+** pour des frappes fortes.
3. **Appuie sur REC sur le « 1 » qui suit ta dernière mesure** : la boucle se ferme, sa durée fixe le tempo, les frappes se calent sur la grille et la boucle joue aussitôt.
4. **REC** pendant la lecture : tu enregistres par-dessus (overdub). Maintiens **ARP** et garde la touche du charley : un roulement en 1/16, enregistré en ratchets.
5. **ALGORITHM** sur la piste **1** (bleue, *808 BOOM*), **REC**, joue une basse. Maintiens **SCL** et appuie sur la tonalité du morceau (ex. ré) ; sur la piste 2, maintiens SCL et tourne **KNOB 1** sur *7TH* : chaque touche blanche joue un accord de la gamme.
6. Maintiens **FX** + une touche blanche : un effet punch-in. Toujours FX enfoncé : **KNOB 2** = DUST (vinyle), **KNOB 3** = DUCK (pompe).
7. Une erreur ? Maintiens **EDIT** et appuie sur **OCT−** : annuler.

## Les couleurs

| Couleur | Piste | Bouton |
| --- | --- | --- |
| **bleu** | 1 · synthé | KNOB 1 |
| **vert** | 2 · synthé | KNOB 2 |
| **jaune** | 3 · synthé | KNOB 3 |
| **orange** | 4 · batterie | KNOB 4 |

Blanc = ce que tu touches. Rouge = enregistrement.

## Tapé ou maintenu : les calques

Chaque bouton de fonction a deux vies. **Tapé** (appuyé puis relâché sans rien toucher d'autre) : ses pages s'ouvrent, comme avant. **Maintenu** : un **calque** — les 16 touches blanches et les KNOB 1–4 changent de rôle, l'écran affiche les 16 touches en tuiles et les boutons en cadrans. Relâché : on rejoue.

**Repères lumineux :** les tuiles sont 4 rangées de 4 (touches 1–4, 5–8, 9–12, 13–16). Tant qu'un calque est maintenu, et sur la piste batterie, la première touche de chaque rangée (1, 5, 9, 13) s'allume faiblement ; ce qui est actif (un effet, un pas, un son) reste allumé à fond.

**Verrouiller un calque :** maintiens son bouton et tape **HOME** : le calque reste ouvert quand tu lâches le bouton, tes deux mains sont libres pour les touches et les potards (*LOCK* à l'écran, le bouton clignote). N'importe quel autre bouton le referme (HOME, son propre bouton, ENV…) ; PLAY, REC et OCT− / OCT+ continuent de marcher dedans.

| Maintenir | Touches | KNOB 1 · 2 · 3 · 4 |
| --- | --- | --- |
| **FX** — *punch* | effet punch-in tant que la touche est tenue | FILTRE · DUST · DUCK · — |
| **EDIT** — *effacer* | efface ce son / cette note du motif | DÉCALER · LONGUEUR ×2 / ½ · TRANSPOSER · — |
| **ARP** — *roll* | note repeat calé sur la grille | VITESSE · — · — · — |
| **SEQ** — *pas* | les pas 1–16 de la page | SON / NOTE · DIV · SWING · LONGUEUR |
| **SCL** — *tonalité* | la tonalité du morceau | ACCORD · GAMME · TOUCHES · TRANSPOSER |
| **GLO** — *mix* | 1–4 mute · 5–8 solo · 16 tap tempo | volume des pistes 1 · 2 · 3 · 4 |
| **SAVE** — *chanson* | 1–4 joue la section A–D (mesure suivante) · 5–8 sauve la boucle dans A–D · 13 boucle / chanson · 14 REC chanson · 16 la chaîne | — |

| Commande | Action |
| --- | --- |
| **PLAY** | lecture / arrêt des quatre pistes (marche aussi dans un calque) |
| **REC** | en lecture : enregistre tout de suite / arrête · à l'arrêt : arme (la première note démarre) · prise libre : ferme la boucle |
| **REC maintenu** | efface la piste choisie (un anneau se remplit, ~2 s ; relâche avant et rien ne se passe) |
| **SAVE** | sur TRACKS : l'écran SONG · ailleurs : les pages SAVE |
| **EDIT + OCT− / OCT+** | annuler / rétablir |
| **ALGORITHM** | choisit la piste (sur toutes les pages) |
| **PRESETS** | le son de la piste, ou le kit de batterie |
| **SELECT** | tempo (toujours, même dans un calque) |
| **OCT− / OCT+** | synthés : octave (les deux : retour à 0) · batterie, maintenus : ghost / fort |
| **HOME** | l'écran TRACKS · maintenu : menu (couleur, coupe-bas, zoom, lumières, touches, notes, audio USB, calibration, à propos) · tapé pendant qu'un calque est maintenu : le verrouille |

## La batterie : 16 sons sur les touches blanches

| Touche | Son | Touche | Son | Touche | Son | Touche | Son |
| --- | --- | --- | --- | --- | --- | --- | --- |
| F3 | kick | C4 | charley fermé | G4 | snare 2 | D5 | ride |
| G3 | kick 2 | D4 | charley ouvert | A4 | tom grave | E5 | shaker |
| A3 | snare | E4 | charley pédale | B4 | tom aigu | F5 | conga |
| B3 | clap | F4 | rim | C5 | crash | G5 | cloche |

Une touche noire joue le son de la touche blanche à sa gauche (deux doigts sur un son pour les roulements rapides). Chaque frappe a un **niveau** — GHOST, SOFT, NORM (comme jouée), HARD — et un **ratchet** x1–x4 (la frappe répétée dans son pas).

## Enregistrer

| Quand | REC fait | Ensuite |
| --- | --- | --- |
| **En lecture** | enregistre la piste **tout de suite** | REC à nouveau arrête l'enregistrement, la boucle continue |
| **À l'arrêt, projet avec des notes** | arme (*rec ready*) | **ta première note démarre la boucle et devient le pas 1** |
| **À l'arrêt, projet vide** | arme (*play freely*) | une **prise libre** : la boucle suit ton jeu (ou, en mode *tempo*, comme avec des notes) |

**L'écran REC règle la façon d'enregistrer** (armé, avant la première note) :

- **KNOB 1 — mode** (projet vide seulement) : **free** = prise libre, le tempo suit ton jeu · **tempo** = enregistre au tempo réglé (SELECT).
- **KNOB 2 — length** : la boucle de la piste, **1, 2 ou 4 mesures**.
- **KNOB 3 — start** : **note** = ta première note démarre la boucle · **count** = appuie sur **PLAY** : une mesure de clics (4, 3, 2, 1 à l'écran), puis la boucle démarre et enregistre.

Le mode et le départ restent comme tu les as laissés (réglages de la FM-1). Avec des notes déjà dans le projet, pas de mode : on enregistre toujours au tempo réglé. Pendant le décompte, REC l'annule et PLAY revient à *rec ready*.

**Prise libre :** joue librement ; l'écran montre les secondes et la boucle que ça donnerait (*2 bars · 92 bpm*). **REC sur le « 1 » qui suit ta dernière mesure** : SHIFL choisit 1, 2 ou 4 mesures au tempo le plus proche, cale tes notes et lance la boucle. **PLAY** abandonne la prise.

Les notes vont au pas le plus proche **tel que tu l'as entendu** (la latence de ~12 ms est compensée). La lumière PLAY clignote à chaque temps : un métronome visuel. Clic audible : GLO → GLOBAL → CLICK.

**Swing** façon MPC : de 50 % (droit) à 75 % — GLO → GLOBAL → SWING pour tout, SEQ maintenu + KNOB 3 par piste (ils s'additionnent).

## Les calques en détail

**FX — punch.** Les 16 touches blanches = les 16 effets (boucles 1/4 à 1/32, stutter, reverse, tape stop, demi-vitesse, filtres, téléphone, bit crush, alias, gate, écho, wobble), tant que la touche est tenue. KNOB 1 = filtre DJ (gauche passe-bas, droite passe-haut, centre OFF), KNOB 2 = DUST, KNOB 3 = DUCK.

**EDIT — effacer.** EDIT + une touche : ce son (batterie) ou cette note (synthé) quitte le motif — **en lecture**, de chaque pas que la tête de lecture traverse tant que tu tiens la touche (façon MPC : garde le charley une mesure et les charleys de cette mesure disparaissent) ; **à l'arrêt**, de tout le motif. KNOB 1 décale le motif d'un pas, KNOB 2 le double (×2, copie) ou le coupe en deux, KNOB 3 transpose (synthés). **EDIT + OCT−** annule, **EDIT + OCT+** rétablit.

**ARP — roll (note repeat).** ARP + une touche tenue : elle se répète sur la grille à la vitesse de KNOB 1 (1/8, 1/16, 1/32, 32T, 1/64), toujours en rythme. En enregistrement, un roll s'écrit en ratchets.

**SEQ — pas (séquenceur pas à pas).** Les 16 touches blanches = les 16 pas de la page ; les quatre premières touches noires (F#3, G#3, A#3, C#4) ou OCT− / OCT+ = pages 1–4.
- Pas vide : appuie, il est posé (batterie : avec le son affiché, le dernier pad frappé ou KNOB 1 ; synthé : avec la dernière note ou le dernier accord joué).
- Pas posé : appuie et relâche, il s'efface. Tiens-le et tourne un bouton : il est modifié et gardé — KNOB 1 son / note, **KNOB 2 niveau**, **KNOB 3 ratchet**. Plusieurs pas tenus se modifient ensemble.
- Sans pas tenu : KNOB 2 = DIV, KNOB 3 = swing de la piste, KNOB 4 = longueur (1–64 pas ; chaque piste boucle sur sa longueur).

**SCL — tonalité et accords.** Une touche = la tonalité du morceau (la fondamentale des trois synthés). KNOB 1 = **ACCORD** de la piste : OFF, TRIAD, 7TH, 9TH (1-3-7-9, le voicing lo-fi / R&B), SUS4, POWER. Avec un accord, **les touches blanches parcourent la gamme à partir de C4** : C4 = l'accord du I, D4 du II, E4 du III… un doigt, un accord, enregistré comme tel. KNOB 2 = gamme (16 gammes), KNOB 3 = touches (OFF chromatique, SNAP arrondi à la gamme, WHITE gamme sur les blanches), KNOB 4 = transposition. Changer de son ne change jamais la tonalité, le motif ni le mix de la piste.

**GLO — mix.** Touches blanches 1–4 = mute des pistes 1–4, 5–8 = solo, la dernière (G5) = **tap tempo**. KNOB 1–4 = volume des pistes 1–4.

## Annuler, effacer, sauvegarder

- **Annuler / rétablir :** EDIT + OCT− / OCT+ (un niveau : le dernier passage d'enregistrement, effacement, piste effacée, modification de pas ou de motif).
- **Effacer une piste :** maintiens REC ; après 0,7 s un anneau se remplit ; tiens encore ~1,3 s. Relâche avant : rien. Annuler la ramène.
- **Sauvegarder :** SAVE + touches 5–8 sauvent la boucle dans la section / le projet A–D (= SLOT 1–4).
- **Sauvegarde automatique :** à l'arrêt, 2,5 s sans toucher (au plus toutes les 20 s), le projet en cours est gardé ; au rallumage, SHIFL revient comme tu l'as laissé.
- **Nouveau projet :** SAVE → TOOLS → NEW (tourner sur GO).

## Le master : DUST, DUCK, FILT

- **DUST** : le mix dans un vieux sampler et sur un vinyle — saturation douce, fréquence d'échantillonnage réduite, moins de bits, passe-bas, et pendant la lecture un léger souffle et des craquements (à l'arrêt, silence).
- **DUCK** : chaque kick fait « pomper » les synthés (sidechain), sur une croche, à tout tempo.
- **FILT** : filtre DJ — à gauche passe-bas, à droite passe-haut, au centre OFF.

Réglables avec FX maintenu (KNOB 1–3) ou GLO → MASTER.

## Mode chanson

Une chanson enchaîne 4 sections, **A–D** (chacune garde les 4 pistes : sons, motifs, kit). Elle se construit en jouant :

1. Fais une boucle (le couplet). Maintiens **SAVE** et appuie sur la **5ᵉ touche blanche** (*save A*). Change la boucle (le refrain) et sauve-la dans **B** (6ᵉ touche), un pont dans **C**, une fin dans **D**. Sur une section déjà utilisée, rappuie dans les 3 s pour confirmer.
2. **Jouer les sections en direct :** SAVE + touche blanche **1–4**. En lecture, la section démarre à la mesure suivante, toujours en rythme ; à l'arrêt, elle devient la boucle tout de suite.
3. **Enregistrer la chanson en jouant :** SAVE + touche **14** (*rec*) : dès la mesure suivante, chaque section jouée et son nombre de mesures sont écrits dans la chanson. Rappuie (ou STOP) pour finir : *SONG PARTS 5*. Elle se sauvegarde toute seule une fois arrêtée.
4. **La rejouer :** SAVE + touche **13** bascule *loop* / *song* ; en mode song, **PLAY** joue tout le morceau et s'arrête à la fin.

L'écran **SONG** (SAVE tapé sur TRACKS, ou SAVE + touche 16) montre la chaîne et permet de la retoucher aux boutons (KNOB 1 étape, 2 section, 3 mesures, 4 nombre d'étapes).

## L'éditeur web

Chrome ou Edge, FM-1 en USB, **Connect**. Il suit l'appareil en direct.

- **Sequencer** sur la piste batterie : une grille 16 sons × pas, avec le **kit**. Choisis un **niveau** (GHOST, SOFT, NORM, HARD) et un **roll** (x1–x4), puis clique : une frappe ; reclique (même niveau et roll) : effacée ; Maj+clic : un niveau plus fort.
- **Settings → MASTER** : DUST, DUCK, FILT, ROLL. **Tracks** : les quatre tranches (volume, pan, mute ; SOLO et REC affichés). **Samples** : tes sons USR1–USR3 et le découpage CHOP (même un enregistrement de plus de 7 s : coche les chops à garder, raccourcis-les, ou **Fit to slot**).

## Clavier MIDI

- **Prise MIDI IN** (jack 3,5 mm TRS du FM-1) : un clavier ou des pads avec une sortie MIDI, via un adaptateur TRS ↔ DIN MIDI. Si rien ne joue, essaie l'autre type d'adaptateur (type A / type B).
- **USB** : depuis un ordinateur ou un téléphone (DAW, appli de routage MIDI) ou un boîtier « USB MIDI host ». Un clavier USB branché directement sur le FM-1 ne peut pas marcher : ce sont deux appareils USB, il faut un hôte.
- **Canaux :** 1, 2, 3 = pistes synth 1, 2, 3 · 10 = la batterie · 4 à 16 = **la piste sélectionnée** (règle ton clavier sur le canal 4 et il suit ALGORITHM).
- **Horloge MIDI :** GLO → SYSTEM → **SYNC** = **USB** ou **TRS**. SHIFL suit le tempo, START, CONTINUE et STOP du maître, sans jamais dériver. Sans horloge pendant une demi-seconde, PLAY rejoue au tempo du FM-1. SYNC est un réglage de la FM-1 : il reste quand tu charges un projet.
- Le MIDI Bluetooth n'est pas pris en charge (la radio reste éteinte).

## Lumières (jouer dans le noir)

Maintiens **HOME** pour le menu : **LIGHTS**, **KEYS** et **NOTES** y sont ensemble. PRESETS déplace, **KNOB 1** règle, OCT+ fait défiler, OCT− ferme. C'est sauvegardé avec les réglages de la FM-1, pas avec un projet : charger un projet ou NEW PROJECT n'y change rien.

- **LIGHTS** — OFF, LOW, MID, HIGH : tous les boutons s'éclairent à ce niveau, on lit les étiquettes dans le noir (illisibles sur un FM-1 noir quand ils sont éteints). Ce qui est actif (la page, PLAY, REC, l'octave) reste en pleine lumière et clignote comme avant.
- **KEYS** — OFF, C KEYS, WHITE KEYS : les touches Do, ou toutes les touches blanches, s'éclairent aussi.
- **NOTES** — ON : sur une piste synth, les notes qui sonnent allument leur touche, jouées au clavier ou par le séquenceur (par @renebohne). Ça marche sur toutes les pages et dans tous les calques : là où les touches jouent ou effacent des notes (EDIT, ARP, SAVE, SCL), les notes sont allumées ; là où les touches sont des tuiles (effets FX, pas SEQ, mute / solo GLO), les notes brillent faiblement et les tuiles gardent leur pleine lumière.

L'éclairage faible est une impulsion très courte à chaque balayage du panneau : pas de scintillement.

## Enregistrer la FM-1 sur l'ordinateur (audio USB)

Branchée en USB, la FM-1 est aussi une **entrée audio** nommée **Felucca** (44,1 kHz, stéréo, sans pilote). Dans ta DAW ou dans Audacity, choisis cette entrée et enregistre : tu as la sortie master, exactement ce qu'on entend au casque (avec DUST, DUCK et FILT). **Le niveau : menu HOME → USB AUDIO.** **MASTER** (par défaut) : l'enregistrement suit le bouton MASTER, comme le casque. **FULL** : niveau fixe, comme MASTER à fond, protégé de la saturation par le limiteur, quel que soit le bouton ; MASTER ne règle alors que le casque (le bon choix pour une carte son sans réglage de niveau). Le MIDI, l'éditeur et l'installateur marchent toujours sur le même câble. La première fois, l'ordinateur reconfigure l'appareil (MIDI + audio) ; le port MIDI garde son nom. Cette entrée vient de Felucca 1.0.

## Sauvegarde complète

Dans l'éditeur, onglet **Projects** → **Backup** : **Save a backup** enregistre tout le contenu de la FM-1 dans un seul fichier (le morceau en cours, les projets 1 à 4, les presets utilisateur, les samples USR1 à USR3, les réglages). **Restore from a file** remet tout comme dans le fichier (ce qui est sur la FM-1 est remplacé). Arrête la lecture (PLAY) avant de restaurer.

## Secours

- **Secours USB :** maintiens **OCT−** seul à l'allumage (*SHIFL USB RESCUE*), puis réinstalle.
- **Installation interrompue :** le FM-1 reste en mode mise à jour ; relance INSTALL et il termine. Un paquet abîmé est refusé et le FM-1 attend un paquet correct.
- **Retour au firmware officiel :** sur la page d'installation, ouvre **Return to the official firmware (V15)** : fais d'abord une sauvegarde avec l'éditeur, télécharge FM-1 V15 sur m-vave.com, choisis son fichier FM-1.fwsc (seul ce fichier exact est accepté) et installe-le. M-UPGRADE de M-VAVE marche aussi (ferme les autres applis qui utilisent le MIDI).

SHIFL est libre (GPL-3.0), basé sur Felucca de Leo Kuroshita (Hügelton Instruments). M-VAVE et FM-1 sont des marques de leurs propriétaires ; SHIFL n'y est pas affilié.
