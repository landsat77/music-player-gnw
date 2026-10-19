# Music Player — installation

Requires Retro-Go SD **v2.0.0-rc1 or later** on the Game & Watch.

1. Copy `MusicPlayer.bin` (in the release zip: `homebrews/MusicPlayer.bin`)
   to the SD card:

       /homebrews/MusicPlayer.bin

2. Put your music in a `music` folder at the root of the SD card. Sub-folders
   are fine (by artist, by album...):

       /music/Amiga/space_debris.mod
       /music/Albums/My Album/01 - First Song.mp3

   Formats: MOD, S3M, XM, IT (tracker modules), MP3 and MIDI (.mid, .kar,
   .rmi).

   For MIDI, also put a SoundFont (`.sf2`) in `/music`, for example
   TimGM6mb.sf2 (6 MB). A `.sf2` with the name of a song, or in its folder,
   is used first. It is copied to the external flash the first time.

3. Start "Music Player" from the Homebrew tab.

A module too big for the RAM of the console (more than about 500 KB of
samples) is copied to the external flash the first time it is played: a
progress bar is shown, a few seconds per MB. Later plays start at once.

Controls — browser: up / down = move, left / right = page, A = open / play,
B = parent folder, GAME = now playing, TIME = play mode. Now playing: A =
pause, left / right = previous / next track, up / down = seek, B or GAME =
back to the browser (the music goes on). PAUSE/SET = Retro-Go menu
(savestates; Options: play mode, screen off).

---

# Music Player — installation (français)

Nécessite Retro-Go SD **v2.0.0-rc1 ou plus récent** sur la Game & Watch.

1. Copier `MusicPlayer.bin` (dans le zip de la release :
   `homebrews/MusicPlayer.bin`) sur la carte SD :

       /homebrews/MusicPlayer.bin

2. Mettre la musique dans un dossier `music` à la racine de la carte SD. Les
   sous-dossiers sont acceptés (par artiste, par album...) :

       /music/Amiga/space_debris.mod
       /music/Albums/Mon Album/01 - Premier titre.mp3

   Formats : MOD, S3M, XM, IT (modules de trackers), MP3 et MIDI (.mid,
   .kar, .rmi).

   Pour le MIDI, mettre aussi une SoundFont (`.sf2`) dans `/music`, par
   exemple TimGM6mb.sf2 (6 Mo). Une `.sf2` au nom d'un morceau, ou dans son
   dossier, passe en premier. Elle est copiée dans la flash externe la
   première fois.

3. Lancer « Music Player » depuis l'onglet Homebrew.

Un module trop gros pour la RAM de la console (plus d'environ 500 Ko de
samples) est copié dans la flash externe à sa première lecture : une barre de
progression s'affiche, quelques secondes par Mo. Les lectures suivantes
démarrent tout de suite.

Commandes — navigateur : haut / bas = se déplacer, gauche / droite = page,
A = ouvrir / lire, B = dossier parent, GAME = lecture en cours, TIME = mode de
lecture. Lecture en cours : A = pause, gauche / droite = piste précédente /
suivante, haut / bas = avancer / reculer, B ou GAME = retour au navigateur (la
musique continue). PAUSE/SET = menu Retro-Go (sauvegardes ; Options : mode de
lecture, extinction de l'écran).
