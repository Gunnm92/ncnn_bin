# BDReader NCNN Upscaler

Ce dépôt fournit le binaire `bdreader-ncnn-upscaler` (C++/ncnn) pour upscaler des images via RealCUGAN et RealESRGAN, avec plusieurs modes d’entrée/sortie (fichier, stdin, batch).

## Pré-requis

Sur Ubuntu/Debian :

```bash
sudo apt-get install -y cmake g++ libvulkan-dev libwebp-dev
```

Notes :
- Vulkan est requis pour les modes GPU/iGPU (`--gpu-id 0/1/...`). Le mode CPU (`--gpu-id -1`) fonctionne sans GPU.
- ncnn est compilé depuis la copie vendored `./ncnn/` via CMake.

## Build

Release (prod) :

```bash
cmake -S bdreader-ncnn-upscaler -B bdreader-ncnn-upscaler/build-release -DCMAKE_BUILD_TYPE=Release
cmake --build bdreader-ncnn-upscaler/build-release -j
```

ASAN (debug fuites/mémoire) :

```bash
cmake -S bdreader-ncnn-upscaler -B bdreader-ncnn-upscaler/build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build bdreader-ncnn-upscaler/build-asan -j
```

Binaire :
- Release : `bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler`
- ASAN : `bdreader-ncnn-upscaler/build-asan/bdreader-ncnn-upscaler`

## Utilisation (CLI)

Aide : `bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler --help`

Options importantes :
- `--engine realcugan|realesrgan`
- `--mode file|stdin`
- `--gpu-id auto|-1|0|1|...` (`-1` = CPU, `1` = iGPU Intel dans ce setup)
- `--tile-size N` (force un tiling plus conservateur, utile contre les OOM)
- `--max-batch-items N` (limite le buffering interne en stdin/batch)
- `--verbose` (logs) / `--profiling` (métriques par image)
- `--log-protocol` (log détaillé par trame pour le debugging du framing binaire)
- `--model` (chemin complet vers un dossier RealCUGAN local ou réseau, défaut `models/realcugan/models-se`)
- `--model-name` (RealESRGAN uniquement) permet d’indiquer un modèle précis ; si vide, le binaire choisit automatiquement `realesr-animevideov3-x{scale}`.
- `--tile-size` = `0` laisse l’engine choisir (512 avec overlap~32) ; une valeur > 0 impose une grille minimale pour limiter la RAM, utile sur petites machines pour retomber à `>=384`.
- `--keep-alive` (avec `--mode stdin`) maintient le process vivant et active le protocole encadré décrit ci-dessous (`BRDR` version 2). Il n’y a plus d’option `--protocol`; la version v2 est implicite.

### Mode `file`

```bash
bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler \
  --engine realcugan --mode file \
  --input img_test/P00003.jpg --output /tmp/out.webp \
  --quality F --gpu-id 0 --format webp
```

### Mode `stdin` (1 image)

Le binaire lit une image compressée sur stdin et écrit l’image upscalée (compressée) sur stdout :

```bash
cat img_test/P00003.jpg | bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler \
  --engine realcugan --mode stdin --quality F --gpu-id 0 --format webp \
  > /tmp/out.webp
```

Note : en mode `stdin` “1 image”, le binaire lit stdin **jusqu’à EOF** avant de lancer le traitement. Si tu pipes depuis un programme (Rust/Go/etc.), il faut **fermer stdin** (EOF) après avoir écrit l’image, sinon ça peut “bloquer”.

### Mode `stdin` + `--keep-alive`

Chaque requête est encadrée par la trame `BRDR` version 2 pour éviter d’avoir à fermer stdin après chaque image :
- `stdin` : `[frame_len:u32_le][BRDR header (magic/version/msg_type/request_id)][payload...]`. `magic="BRDR"` (0x42524452), `version=2`, `msg_type=1` (request). `frame_len` inclut l’entête complet et le payload.
- `stdout` : `[payload_len:u32_le][request_id:u32][status:u32][error_len:u32][error_bytes][result_count:u32][out_len:u32][out_bytes]...]`. `status=0` → `result_count` images en sortie. `status!=0` renvoie un message d’erreur structuré et la boucle continue ensuite.
- `--profiling` (avec `--keep-alive`) écrit sur `stderr` une ligne métrique par requête (request_id, status_code, batch_count, bytes_in, bytes_out, error_len, latence) tout en gardant `stdout` pur binaire.
- `--log-protocol` ajoute un log par trame pour observer request_id, status, durée et erreur lorsque nécessaire.
- Les limites mémoire sont explicites : `message_len` plafonné à 64 MiB, chaque image à 50 MiB, et le batch effectif ne peut dépasser ~48 MiB de données compressées. Les réponses conservent l’ordre des entrées et respectent les codes `status` (`Ok`, `InvalidFrame`, `ValidationError`, `ResourceLimit`, `EngineError`, `Timeout`). En cas de corruption ou de dépassement de budget, un message d’erreur structuré est renvoyé sans arrêter le processus.
- Le script `tests/protocol_v2_integration.py` sert de payload de référence : il envoie deux images, vérifie `request_id`, et confirme que les `msg_type` non-request sont rejetés avec la bonne erreur.
`--log-protocol` imprime aussi cette décision par trame (request_id, statut, latence) pour faciliter le debug.

Ce protocole est désormais le seul flux streaming pris en charge. Les anciens flags `--protocol`, `--batch-size` et `--mode batch` ont été retirés : utilisez `--mode stdin --keep-alive` pour les intégrations RPC et `--mode file` pour les traitements ponctuels.

> Le format binaire décrit ici correspond exactement à la spécification listée dans `TODO.md` (section *Protocole NCNN v2 - Spécification Technique*). Si tu constates un écart (header, request_id, status, résultats), c’est cette section qu’il faut mettre à jour, car elle sert de référence canonique aux tests `tests/protocol_v2_*`.

## Mémoire / perf (prod)

- Profil CPU “low-mem” : activé automatiquement quand `--gpu-id -1` ou lors d’un fallback Vulkan→CPU (moins de RAM, souvent plus lent).
- Profil iGPU : activé automatiquement sur GPU intégré (Intel) pour limiter les risques d’OOM (tiling plus agressif + options ncnn conservatrices).
- Fallback automatique : si l’inférence Vulkan échoue, l’engine bascule CPU low-mem au lieu de crasher.

Conseils anti-OOM :
- Forcer un tiling plus petit : `--tile-size 256` (ou `384`) sur images très grandes.
- Sur iGPU, préférer `--gpu-id 1` (profil iGPU auto) plutôt que CPU si possible.

## Debug (ASAN)

Exemple (CPU) :

```bash
ASAN_OPTIONS=detect_leaks=1 ./bdreader-ncnn-upscaler/build-asan/bdreader-ncnn-upscaler \
  --engine realcugan --mode file \
  --input img_test/P00003.jpg --output /tmp/out.webp \
  --quality F --gpu-id -1
```
