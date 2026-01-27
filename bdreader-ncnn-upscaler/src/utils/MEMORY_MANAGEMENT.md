# Memory Management Guidelines

Ce document décrit les pratiques de gestion mémoire dans le projet NCNN upscaler, avec un focus particulier sur la prévention des fuites mémoire.

---

## NCNN Engine Cleanup

### ⚠️ CRITICAL: Ne jamais appeler `engine->cleanup()` dans une boucle

**Pourquoi ?**

Appeler `cleanup()` après chaque item corrompt le modèle NCNN en rendant les noms de blobs (in0/out0) inaccessibles pour les items suivants, causant des erreurs `"find_blob_index_by_name failed"`.

### ✅ Usage Correct

```cpp
// ✅ CORRECT: Appeler cleanup() une seule fois à la fin
for (auto& item : items) {
    engine->process_single(...);
    // Ne PAS appeler cleanup() ici
}
engine->cleanup(); // Une seule fois à la fin

// ❌ WRONG: Appeler cleanup() dans la boucle
for (auto& item : items) {
    engine->process_single(...);
    engine->cleanup(); // CORROMPT LE MODÈLE !
}
```

### 📍 Localisations

- **`src/modes/stdin_mode.cpp`** : `cleanup()` appelé une seule fois à la fin du batch dans `worker_thread_func()`
- **`src/main.cpp`** : `cleanup()` appelé à la fin du programme

---

## Gestion des Ressources Vulkan

### Nettoyage Explicite

NCNN's `net_.clear()` devrait libérer les ressources Vulkan, mais nous ajoutons un nettoyage explicite pour plus de sécurité :

```cpp
void Engine::cleanup() {
    net_.clear();
#if NCNN_VULKAN
    if (use_vulkan_) {
        net_.opt.use_vulkan_compute = false; // Force release
        use_vulkan_ = false;
    }
#endif
    model_root_.reset();
}
```

### Ressources Vulkan Libérées

- Command buffers Vulkan
- Descripteurs Vulkan
- Pipelines Vulkan
- Allocations mémoire GPU

**Note** : `ncnn::destroy_gpu_instance()` est une fonction globale qui ne doit être appelée qu'à la fin du programme, pas dans `cleanup()`.

---

## RAII pour Ressources C

### WebP MemoryWriter

Toujours utiliser le wrapper RAII `WebPMemoryWriterRAII` pour garantir le nettoyage même en cas d'exception :

```cpp
// ✅ CORRECT: Utiliser RAII wrapper
WebPMemoryWriterRAII writer_raii;
WebPPictureRAII pic_raii;
// ... utilisation ...
// Destructeurs appellent automatiquement WebPMemoryWriterClear et WebPPictureFree

// ❌ WRONG: Nettoyage manuel (peut être contourné par exceptions)
WebPMemoryWriter writer;
WebPMemoryWriterInit(&writer);
// ... code qui peut throw ...
WebPMemoryWriterClear(&writer); // Peut ne pas être appelé si exception
```

### STB Image

Utiliser le wrapper RAII `STBImageRAII` :

```cpp
// ✅ CORRECT: Utiliser RAII wrapper
STBImageRAII pixels_raii;
pixels_raii.reset(stbi_load_from_memory(...));
// ... utilisation ...
// Destructeur appelle automatiquement stbi_image_free()

// ❌ WRONG: Nettoyage manuel
stbi_uc* pixels = stbi_load_from_memory(...);
// ... code qui peut throw ...
stbi_image_free(pixels); // Peut ne pas être appelé si exception
```

### Localisations

- **`src/utils/image_io.cpp`** : Wrappers RAII définis dans namespace anonyme
- Utilisés dans `decode_image()` et `encode_image()`

---

## Sécurité Exception (Exception Safety)

### Protection Try-Catch

Tous les buffers utilisent `std::vector` qui fournit RAII. Cependant, nous ajoutons des blocs try-catch pour :

1. Garantir que `cleanup()` est appelé dans les chemins d'erreur
2. Fournir des logs d'erreur détaillés
3. Prévenir les fuites de ressources dans les cas exceptionnels

### Exemples

**Dans `process_image()` des engines** :

```cpp
bool Engine::process_image(...) {
    try {
        // ... traitement ...
        result.release(); // Libération explicite des ressources GPU
        in.release();
        return true;
    } catch (const std::exception& e) {
        logger::error("Exception: " + std::string(e.what()));
        // Buffers libérés automatiquement par RAII
        return false;
    } catch (...) {
        logger::error("Unknown exception");
        return false;
    }
}
```

**Dans `worker_thread_func()`** :

```cpp
while (input_queue.pop(input_item)) {
    try {
        // ... traitement par image ...
    } catch (const std::exception& e) {
        // Per-image exception: log et continuer avec l'image suivante
        logger::error("Exception processing image: " + std::string(e.what()));
        metrics.errors.fetch_add(1);
        input_item.data.clear();
        continue; // Continue au lieu de break
    }
}
```

### Localisations

- **`src/engines/realcugan_engine.cpp`** : `process_image()` protégé
- **`src/engines/realesrgan_engine.cpp`** : `process_image()` protégé
- **`src/modes/stdin_mode.cpp`** : `worker_thread_func()` protégé par image
- **`src/utils/tiling_processor.cpp`** : `process_with_tiling()` protégé globalement et par tile

---

## Libération des Ressources NCNN Mat

### Libération Explicite

Les objets `ncnn::Mat` sont créés localement et devraient être automatiquement libérés par le destructeur. Cependant, nous ajoutons des appels explicites pour garantir la libération des ressources GPU :

```cpp
ncnn::Mat in = ncnn::Mat::from_pixels(...);
ncnn::Mat result;
// ... traitement ...
result.release(); // Libération explicite des buffers GPU
in.release();
```

**Note** : NCNN utilise des allocations GPU pour les `Mat` lorsque Vulkan est activé. Le destructeur devrait libérer ces ressources, mais l'appel explicite `release()` garantit la libération immédiate.

### Localisations

- **`src/engines/realcugan_engine.cpp`** : `process_image()` ligne 206-207
- **`src/engines/realesrgan_engine.cpp`** : `process_image()` ligne 260-261

---

## Buffers Intermédiaires

### Protection RAII

Tous les buffers intermédiaires utilisent `std::vector` qui fournit RAII automatique :

```cpp
// ✅ CORRECT: std::vector libère automatiquement même en cas d'exception
std::vector<uint8_t> full_pixels(result.w * result.h * 3);
std::vector<uint8_t> final_pixels;
// ... utilisation ...
// Destructeurs libèrent automatiquement la mémoire
```

### Libération Explicite (Optionnel)

Pour réduire l'utilisation mémoire de pointe, on peut libérer explicitement avant la fin de scope :

```cpp
input_item.data.clear();
input_item.data.shrink_to_fit(); // Réduit la capacité à 0
```

**Localisations** :
- **`src/modes/stdin_mode.cpp`** : `worker_thread_func()` ligne 205-206

---

## Queues Thread-Safe

### BoundedBlockingQueue

Les queues utilisent `std::queue` avec des mutex, garantissant la sécurité thread-safe. Les destructeurs libèrent automatiquement la mémoire :

```cpp
BoundedBlockingQueue<InputItem> input_queue(QUEUE_CAPACITY);
BoundedBlockingQueue<OutputItem> output_queue(QUEUE_CAPACITY);
// ... utilisation ...
// Destructeurs libèrent automatiquement tous les items
```

**Note** : En cas d'exception, les destructeurs C++ garantissent la libération de la mémoire des queues.

---

## Checklist de Validation

Lors de l'ajout de nouveau code, vérifier :

- [ ] `engine->cleanup()` n'est jamais appelé dans une boucle
- [ ] Toutes les ressources C (WebP, STB) utilisent des wrappers RAII
- [ ] Les fonctions critiques ont des blocs try-catch
- [ ] Les ressources NCNN Mat sont explicitement libérées avec `release()`
- [ ] Les buffers utilisent `std::vector` (RAII automatique)
- [ ] Les logs d'erreur sont détaillés pour le debugging

---

## Tests de Fuite Mémoire

### Valgrind

```bash
valgrind --leak-check=full --show-leak-kinds=all \
  ./bdreader-ncnn-upscaler --input test.jpg --output out.jpg
```

### AddressSanitizer

Compiler avec :
```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -g" ..
make
```

Exécuter et vérifier qu'il n'y a pas de fuites.

### Tests Batch

Tester avec des batchs de 100+ images pour reproduire les fuites :

```bash
# Créer un script de test
for i in {1..100}; do
    ./bdreader-ncnn-upscaler --input img_$i.jpg --output out_$i.jpg
done

# Monitorer la mémoire GPU (NVIDIA)
nvidia-smi -l 1
```

---

## Références

- [NCNN Documentation](https://github.com/Tencent/ncnn)
- [RAII Pattern](https://en.cppreference.com/w/cpp/language/raii)
- [Exception Safety](https://en.cppreference.com/w/cpp/language/exceptions)

---

**Dernière mise à jour** : 2025-01-27  
**Version** : 1.0
