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
- `--mode file|stdin|batch`
- `--gpu-id auto|-1|0|1|...` (`-1` = CPU, `1` = iGPU Intel dans ce setup)
- `--tile-size N` (force un tiling plus conservateur, utile contre les OOM)
- `--max-batch-items N` (limite le buffering interne en stdin/batch)
- `--verbose` (logs) / `--profiling` (métriques par image)

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

### Mode `stdin` + `--keep-alive` (framed)

En `--keep-alive`, le mode `stdin` utilise un framing binaire (pour éviter d’avoir besoin d’EOF à chaque image) :
- stdin : `[size:u32_le][bytes...]` (répété, `size=0` = stop)
- stdout : `[status:u32_le][size:u32_le][bytes...]` (status=0 ok)

### Mode `stdin` batch (protocol v1)

Activer avec `--batch-size N`. Le payload stdin/stdout est binaire (v1) :
- stdin : `[num_images:u32][size:u32][bytes...]...`
- stdout : `[num_results:u32][size:u32][bytes...]...`

Voir `NCNN_STDIN_STDOUT_SPEC.md` et `test_batch_stdin.py`.

### Mode `batch` (BDRP)

Mode batch streaming via “BDReader Protocol” (`BDRP`) pour traiter plusieurs images en une seule invocation.

Voir `BATCH_PROCESSING_SPEC.md`.

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
  --engine realcugan --mode stdin --batch-size 5 --gpu-id -1 --quality F \
  --model /config/workspace/BDReader-Rust/backend/models/realcugan/models-se \
  < test_output_perf_igpu/batch_v1_5.bin > /dev/null
```
