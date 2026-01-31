# Opportunités d'Optimisation Performance

**Date:** 2026-01-30
**Contexte:** Batching n'améliore pas la performance (GPU saturé)
**Baseline actuel:** 0.83 imgs/sec (images moyennes), 5.98 imgs/sec (petites)

---

## 🎯 Analyse du Goulot d'Étranglement Actuel

### Performance Mesurée (GPU mode)

| Taille Image | Temps GPU | Temps Total | GPU Utilization |
|--------------|-----------|-------------|-----------------|
| < 300 KB | 150-200 ms | 170 ms | ~90% |
| 500 KB | 2000 ms | 2100 ms | ~95% |
| 1-2 MB | 2200-2500 ms | 2400 ms | ~95% |

**Observation:** GPU proche de 100% → Pas de marge pour parallélisme

---

## 🚀 Options d'Optimisation Disponibles

### Option 1: Pipeline CPU/GPU (⭐⭐⭐⭐⭐ Recommandé)

**Principe:** Découpler les étapes CPU et GPU

```
Actuellement (séquentiel):
┌─────────┐     ┌─────────┐     ┌─────────┐
│ Decode  │────>│   GPU   │────>│ Encode  │  Image 1
│  JPEG   │     │ Upscale │     │  WebP   │
└─────────┘     └─────────┘     └─────────┘
   50ms           2000ms           50ms
   └────────────── 2100ms total ──────────────┘

Optimisé (pipeline):
Thread 1: Decode  │████│    │████│    │████│
Thread 2: GPU     │    │████│    │████│    │████│
Thread 3: Encode  │    │    │████│    │    │████│
          └──────────────────────────────────────>
          Throughput: ~1.2x faster (overlap CPU/GPU)
```

**Gains estimés:**
- ✅ +20-30% throughput
- ✅ Meilleure utilisation CPU et GPU
- ✅ Pas de changement protocole

**Implémentation:**
```cpp
// stdin_mode.cpp: run_keep_alive_protocol_v2()

// Queue 1: Images décodées prêtes pour GPU
BlockingQueue<DecodedImage> decode_queue;

// Queue 2: Images upscalées prêtes pour encode
BlockingQueue<UpscaledImage> encode_queue;

// Thread 1: Decode (CPU)
std::thread decode_thread([&]() {
    while (auto img = read_next_jpeg()) {
        auto decoded = decode_jpeg(img);  // stb_image
        decode_queue.push(decoded);
    }
});

// Thread 2: Upscale (GPU) - thread principal
while (auto decoded = decode_queue.pop()) {
    auto upscaled = gpu_upscale(decoded);
    encode_queue.push(upscaled);
}

// Thread 3: Encode (CPU)
std::thread encode_thread([&]() {
    while (auto upscaled = encode_queue.pop()) {
        auto webp = encode_webp(upscaled);  // libwebp
        write_response(webp);
    }
});
```

**Effort:** Moyen (2-3 jours)

---

### Option 2: Réduction Tile Size (⭐⭐⭐ Efficace petites machines)

**Principe:** Tiles plus petits = moins de RAM mais plus de calcul

```bash
# Actuel
--tile-size 512  # 512x512 tiles

# Petites machines
--tile-size 256  # 256x256 tiles
```

**Impact:**

| Tile Size | RAM GPU | Temps Total | Use Case |
|-----------|---------|-------------|----------|
| 0 (auto) | 1.5 GB | 2.0s | GPU puissant |
| 512 | 800 MB | 2.4s | Standard ✅ |
| 256 | 400 MB | 2.8s | RAM limitée |
| 128 | 200 MB | 3.5s | Très petite GPU |

**Gains:**
- ✅ -50% RAM avec tile-size 256
- ⚠️ -15% performance

**Déjà disponible:** Oui (via CLI)

---

### Option 3: Model Quantization INT8 (⭐⭐⭐⭐ Recommandé)

**Principe:** Modèle FP16/INT8 au lieu de FP32

```
FP32 (actuel):
  - Précision: 32 bits
  - Taille modèle: 100%
  - Vitesse: baseline
  - Qualité: 100%

FP16 (half precision):
  - Précision: 16 bits
  - Taille modèle: 50%
  - Vitesse: 1.5-2x faster (GPUs modernes)
  - Qualité: 99.5%

INT8 (quantized):
  - Précision: 8 bits
  - Taille modèle: 25%
  - Vitesse: 2-4x faster
  - Qualité: 98% (acceptable pour upscale)
```

**Gains estimés:**
- ✅ +50-100% throughput
- ✅ -50% RAM
- ⚠️ -1-2% qualité (imperceptible)

**Implémentation:**
Nécessite reconversion du modèle avec NCNN tools:
```bash
# Convertir modèle existant en INT8
ncnnoptimize model.param model.bin \
  model-int8.param model-int8.bin \
  65536
```

**Effort:** Faible (1 jour + tests qualité)

---

### Option 4: Vulkan Compute Shaders (⭐⭐⭐⭐⭐ Maximum perf)

**Principe:** Utiliser Vulkan au lieu de CUDA/OpenCL

NCNN supporte déjà Vulkan, mais nécessite compilation avec flag:

```cmake
# CMakeLists.txt
option(NCNN_VULKAN "vulkan compute shader support" ON)
```

**Gains (vs CUDA sur même GPU):**
- ✅ +10-30% throughput (selon GPU)
- ✅ Meilleure compatibilité multi-GPU
- ✅ Moins de driver overhead

**Effort:** Faible (recompilation)

---

### Option 5: Multi-GPU Parallelism (⭐⭐⭐⭐⭐ Si plusieurs GPUs)

**Principe:** Distribuer requêtes sur plusieurs GPUs

```
Actuel (1 GPU):
  GPU 0: ████████████████████

Optimisé (2 GPUs):
  GPU 0: ████████
  GPU 1:         ████████
         └─────────────────> 2x throughput
```

**Implémentation backend (Rust):**
```rust
// Pool de workers, un par GPU
let workers = vec![
    spawn_worker("--gpu-id 0"),
    spawn_worker("--gpu-id 1"),
];

// Round-robin distribution
for (idx, request) in requests.enumerate() {
    let worker = &workers[idx % workers.len()];
    worker.send(request);
}
```

**Gains:**
- ✅ Throughput × nombre de GPUs (linéaire)
- ✅ Aucune modification binaire C++
- ✅ Scaling horizontal

**Effort:** Moyen (backend Rust)

---

### Option 6: Préchargement Modèle en VRAM (⭐⭐ Marginal)

**Principe:** Garder modèle en VRAM entre requêtes

**Actuellement:** Déjà implémenté ! (keep-alive)
- ❌ Pas de gain supplémentaire

---

### Option 7: Compression Input/Output Adaptative (⭐⭐⭐ Bandwidth)

**Principe:** Ajuster qualité selon taille image

```cpp
// Pour petites images: qualité maximale
if (image_size < 500_KB) {
    webp_quality = 95;
}
// Pour grandes images: qualité réduite
else if (image_size > 2_MB) {
    webp_quality = 85;  // -10 quality
}
```

**Gains:**
- ✅ -20-30% taille output grandes images
- ✅ -10-15% temps encode
- ⚠️ Légère perte qualité

**Effort:** Faible (1h)

---

### Option 8: Async I/O avec io_uring (⭐⭐⭐⭐ Linux only)

**Principe:** Overlap disk I/O avec GPU compute

Pour batch file processing (pas stdin mode):
```cpp
// Lire fichiers de manière async pendant GPU compute
io_uring_queue_init(32, &ring, 0);

// Queue read operations
for (auto& file : files) {
    io_uring_prep_read(...);
}

// Process pendant que I/O en cours
while (pending_io || pending_gpu) {
    // ...
}
```

**Gains:**
- ✅ +15-25% throughput (file mode)
- ❌ Pas applicable stdin mode

**Effort:** Élevé (3-4 jours)

---

## 📊 Comparaison des Options

| Option | Gain Throughput | Effort | Priorité | Applicable |
|--------|----------------|--------|----------|------------|
| **Pipeline CPU/GPU** | +20-30% | Moyen | ⭐⭐⭐⭐⭐ | Tous modes |
| **Model INT8** | +50-100% | Faible | ⭐⭐⭐⭐⭐ | Si qualité OK |
| **Vulkan** | +10-30% | Faible | ⭐⭐⭐⭐ | Tous |
| **Multi-GPU** | ×N GPUs | Moyen | ⭐⭐⭐⭐⭐ | Si >1 GPU |
| **Tile size** | -15% mais -50% RAM | Aucun | ⭐⭐⭐ | Déjà dispo |
| **Compression adaptive** | +10-15% | Faible | ⭐⭐⭐ | Output |
| **Async I/O** | +15-25% | Élevé | ⭐⭐ | File mode |

---

## 🎯 Recommandations par Scénario

### Scénario 1: Gain Rapide (1-2 jours)

**Actions:**
1. ✅ **Activer Vulkan** (recompilation)
2. ✅ **Model INT8** (reconversion + test qualité)

**Gain attendu:** +60-130% throughput

### Scénario 2: Optimisation Poussée (1 semaine)

**Actions:**
1. ✅ Pipeline CPU/GPU (threading)
2. ✅ Model INT8
3. ✅ Vulkan

**Gain attendu:** +100-200% throughput

### Scénario 3: Multi-GPU (si matériel disponible)

**Actions:**
1. ✅ Multi-GPU backend (Rust)
2. ✅ Pipeline CPU/GPU
3. ✅ Model INT8

**Gain attendu:** +200-400% throughput (2-4 GPUs)

---

## 🔬 Tests de Validation Nécessaires

### Pour INT8 Quantization

```python
# Comparer qualité FP32 vs INT8
original_output = process_with_fp32(image)
int8_output = process_with_int8(image)

# Métriques
psnr = calculate_psnr(original_output, int8_output)
ssim = calculate_ssim(original_output, int8_output)

# Acceptable si:
# PSNR > 35 dB
# SSIM > 0.95
```

### Pour Pipeline CPU/GPU

```python
# Vérifier pas de régression
baseline_time = measure_sequential()
pipeline_time = measure_pipeline()

speedup = baseline_time / pipeline_time
assert speedup > 1.15  # Au moins +15%
```

---

## 💡 Quick Wins Immédiats

### 1. Vulkan (10 minutes)

```bash
cd bdreader-ncnn-upscaler/build-release
cmake .. -DNCNN_VULKAN=ON
make -j4

# Test
./bdreader-ncnn-upscaler --mode stdin --keep-alive --gpu-id 0
```

### 2. Tile-size tuning (0 minutes, déjà dispo)

```bash
# Pour RAM limitée
--tile-size 256

# Pour performance max
--tile-size 0  # Auto-detect optimal
```

### 3. Multi-GPU backend (si 2+ GPUs)

```bash
# Lancer 2 workers en parallèle
./bdreader-ncnn-upscaler --gpu-id 0 --keep-alive &
./bdreader-ncnn-upscaler --gpu-id 1 --keep-alive &

# Load balancer Rust distribue requêtes
```

---

## 🎓 Conclusion

**Meilleur ROI (Return on Investment):**

1. **Multi-GPU** (si disponible): ×2-4 throughput, effort moyen
2. **Model INT8**: +50-100%, effort faible, risque qualité
3. **Vulkan**: +10-30%, effort minimal
4. **Pipeline CPU/GPU**: +20-30%, effort moyen

**Pour démarrer:**
1. Tester Vulkan (10 min)
2. Si >1 GPU: setup multi-GPU backend
3. Tester INT8 quantization avec validation qualité
4. Si besoin plus: implémenter pipeline

**Gain total potentiel: +300-500% throughput** 🚀

---

**Prochaines étapes suggérées:**
1. Benchmark Vulkan vs CUDA/OpenCL
2. Convertir modèle en INT8 + tests qualité
3. POC pipeline CPU/GPU sur 1 image
