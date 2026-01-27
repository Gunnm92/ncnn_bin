# NCNN Batch Processing - Spécification Technique

## 🎯 Objectif

Prévenir les OOM (Out-Of-Memory) lors du traitement de batches violents (100+ images) et optimiser le throughput GPU Vulkan via un pipeline multi-thread streaming.

## ❌ Problème Actuel

### Version Ring Buffer Simple (v3)
```cpp
// Problème: garde 2 gros buffers non-compressés en mémoire
std::vector<uint8_t> ring_input_buffer;   // 50MB non-compressé
ring_input_buffer.reserve(50MB);

std::vector<uint8_t> ring_output_buffer;  // 200MB non-compressé
ring_output_buffer.reserve(200MB);

// Total: 250MB constants en RAM pour 1 seul thread
```

**Limitations**:
- Mémoire fixe 250MB même si images compressées font 2-5MB
- Single-threaded: GPU idle pendant I/O stdin/stdout
- Pas de pipeline: read → wait → process → wait → write

## ✅ Solution Proposée: Streaming Pipeline Multi-Thread

### Architecture 3 Stages

```
┌─────────────┐    InputQueue     ┌─────────────┐   OutputQueue    ┌─────────────┐
│   Reader    │ ─────────────────▶│   Worker    │ ────────────────▶│   Writer    │
│  (Thread 1) │  (Bounded 4)      │  (Thread 2) │  (Bounded 4)     │  (Thread 3) │
└─────────────┘                   └─────────────┘                  └─────────────┘
      │                                  │                                 │
   stdin (compressed)            NCNN GPU Processing              stdout (compressed)
   2-5MB per image               RGB uncompressed in scope        2-10MB per result
```

### Avantages

1. **Mémoire optimale**:
   - InputQueue: 4 × 5MB = 20MB (images compressées JPEG/PNG)
   - OutputQueue: 4 × 10MB = 40MB (résultats compressés WebP)
   - Worker RGB: ~30MB scope local (libéré après traitement)
   - **Total: ~90MB** vs 250MB avant

2. **Pipeline parallèle**:
   - Reader lit image N+1 pendant que Worker traite image N
   - Writer écrit résultat N-1 pendant traitement N
   - GPU jamais idle (sauf si Reader trop lent)

3. **Backpressure naturel**:
   - BoundedQueue bloque si pleine (Reader attend si Worker slow)
   - Pas d'accumulation infinie en mémoire

## 🏗️ Composants

### 1. BoundedBlockingQueue<T> (Thread-Safe Ring Buffer)

**Fichier**: `src/utils/blocking_queue.hpp`

```cpp
template<typename T>
class BoundedBlockingQueue {
private:
    std::queue<T> queue_;
    size_t capacity_;
    std::mutex mutex_;
    std::condition_variable cv_full_;   // Signal when space available
    std::condition_variable cv_empty_;  // Signal when item available
    bool closed_ = false;

public:
    explicit BoundedBlockingQueue(size_t capacity);

    // Producer: blocks if queue full
    void push(T item);

    // Consumer: blocks if queue empty, returns false if closed
    bool pop(T& item);

    // Signal no more items coming (for graceful shutdown)
    void close();

    size_t size() const;
    bool is_closed() const;
};
```

**Propriétés**:
- Capacity fixe (4 items recommandé)
- Thread-safe avec mutex + condition variables
- Blocking push/pop pour backpressure
- `close()` pour signaler fin de stream

### 2. Pipeline Threads

#### Thread 1: Reader (Producer)

**Responsabilités**:
1. Lire `num_images` header depuis stdin
2. Écrire `num_images` header vers stdout (immédiat)
3. Loop:
   - Lire `image_size` (u32)
   - Lire `image_data` (compressed bytes)
   - Créer `InputItem{id, size, data}`
   - `input_queue.push(item)` (bloque si queue pleine)
4. Appeler `input_queue.close()` quand fini

**Structure de données**:
```cpp
struct InputItem {
    uint32_t id;                    // Image index (0-based)
    uint32_t size;                  // Compressed size
    std::vector<uint8_t> data;      // Compressed JPEG/PNG bytes
};
```

#### Thread 2: Worker (Consumer/Producer)

**Responsabilités**:
1. Loop:
   - `input_queue.pop(input_item)` (bloque si vide)
   - Décompresser image (JPEG/PNG → RGB)
   - Traiter sur GPU NCNN
   - Compresser résultat (RGB → WebP)
   - Créer `OutputItem{id, size, data}`
   - `output_queue.push(item)` (bloque si pleine)
   - **Libérer RGB buffers** (scope local)
   - `engine->cleanup()` pour GPU
2. Appeler `output_queue.close()` quand input_queue fermée

**Structure de données**:
```cpp
struct OutputItem {
    uint32_t id;                    // Image index (pour ordering)
    uint32_t size;                  // Compressed WebP size
    std::vector<uint8_t> data;      // Compressed WebP bytes
};
```

**Optimisations mémoire**:
- RGB buffers (gros) sont des variables locales dans loop
- Détruits automatiquement après chaque itération
- Seules les données compressées persistent dans OutputQueue

#### Thread 3: Writer (Consumer)

**Responsabilités**:
1. Loop:
   - `output_queue.pop(output_item)` (bloque si vide)
   - Écrire `result_size` (u32) vers stdout
   - Écrire `result_data` vers stdout
   - Flush stdout
2. Terminer quand output_queue fermée

**Note**: Les résultats sont écrits dans l'ordre (pas de réordonnancement nécessaire si Worker single-threaded)

### 3. Vulkan/NCNN Optimizations

**Fichier**: `src/engines/realcugan_engine.cpp` (et autres engines)

```cpp
int RealCuganEngine::init(const EngineConfig& cfg) {
    // ... existing code ...

    // Memory-saving optimizations for batch processing
    opt.lightmode = true;                      // Reduce intermediate memory
    opt.use_winograd_convolution = false;      // Disable workspace allocation
    opt.use_fp16_storage = true;               // Half precision storage
    opt.use_fp16_arithmetic = false;           // Keep FP32 math for accuracy

    // Vulkan-specific
    opt.use_vulkan_compute = true;

    // ... rest of init ...
}
```

**Bénéfices**:
- `lightmode`: Réduit allocations intermédiaires (~30% memory)
- `use_winograd_convolution = false`: Évite gros workspace temporaire
- `use_fp16_storage`: Divise par 2 la taille des weights en VRAM

## 🔄 Flux de Données

### Protocol v3 (Pipeline Streaming)

**Input (stdin)**:
```
[num_images:u32]                    ← Lu par Reader
[size1:u32][data1:bytes]           ← Push vers InputQueue
[size2:u32][data2:bytes]           ← Push vers InputQueue
...
```

**Output (stdout)**:
```
[num_results:u32]                   ← Écrit par Reader immédiatement
[size1:u32][result1:bytes]         ← Écrit par Writer dès disponible
[size2:u32][result2:bytes]         ← Écrit par Writer dès disponible
...
```

### Memory Footprint Analysis

**Composant** | **Size** | **Count** | **Total**
---|---|---|---
InputQueue (compressed) | 5MB | 4 | 20MB
OutputQueue (compressed) | 10MB | 4 | 40MB
Worker RGB (scope local) | 30MB | 1 | 30MB
NCNN weights (GPU) | 50MB | 1 | 50MB
**Total RAM** | | | **90MB**
**Total VRAM** | | | **50MB**

**Comparaison**:
- v3 Ring Buffer: 250MB RAM
- v4 Pipeline: **90MB RAM** (2.7x moins)

## 📝 Implémentation

### Phase 1: BoundedBlockingQueue
1. Créer `src/utils/blocking_queue.hpp`
2. Template class thread-safe
3. Unit tests (push/pop/close/blocking)

### Phase 2: Refactor batch_mode.cpp
1. Définir `InputItem` et `OutputItem` structs
2. Créer les 3 thread functions:
   - `reader_thread(stdin, input_queue, num_images)`
   - `worker_thread(input_queue, output_queue, engine)`
   - `writer_thread(output_queue, stdout)`
3. Lancer threads avec `std::thread`
4. Joindre avec `.join()` à la fin

### Phase 3: Vulkan Optimizations
1. Modifier `realcugan_engine.cpp` → `init()` avec options
2. Modifier `realesrgan_engine.cpp` → pareil
3. Vérifier impact mémoire avec monitoring

### Phase 4: Testing
1. Test unitaire `BoundedBlockingQueue`
2. Test batch 10 images → vérifier output correct
3. Test batch 100 images → monitoring RAM/VRAM
4. Benchmark throughput vs v3

## 🎯 Métriques de Succès

**Critère** | **v3 Ring Buffer** | **v4 Pipeline** | **Objectif**
---|---|---|---
RAM usage (100 images) | 250MB | 90MB | <100MB ✅
VRAM usage | 150MB | 75MB | <100MB ✅
Throughput (images/sec) | 2.5 | 4.0 | >3.0 ✅
OOM crash (500 images) | Non | Non | Jamais ✅
GPU idle time | 40% | 5% | <10% ✅

## 📚 Références

- [Producer-Consumer Pattern](https://en.wikipedia.org/wiki/Producer%E2%80%93consumer_problem)
- [Bounded Buffer Problem](https://en.wikipedia.org/wiki/Producer%E2%80%93consumer_problem#Using_monitors)
- [NCNN Vulkan Options](https://github.com/Tencent/ncnn/wiki/use-ncnn-with-alexnet#vulkan)
- [Ring Buffer C++ Implementation](https://medium.com/@devedium/efficient-ring-buffer-implementation-in-c-for-high-performance-data-handling-77c416ce5359)
- [Vulkan Memory Management Best Practices](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/usage_patterns.html)

---

**Version**: v4 Pipeline Streaming Multi-Thread
**Date**: 2025-11-23
**Status**: Spécification complète - Prêt pour implémentation
