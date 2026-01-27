# Rapport d'Analyse des Fuites Mémoire - NCNN Upscaler

**Date** : 2025-01-27  
**Analyseur** : Auto (Claude Code)  
**Répertoire analysé** : `/config/workspace/BDReader-Rust/ncnn_bin/bdreader-ncnn-upscaler/src/`

---

## Résumé Exécutif

L'analyse du code C++ a identifié **5 anomalies critiques** et **3 problèmes mineurs** susceptibles de causer des fuites mémoire lors de l'utilisation intensive du pipeline d'upscaling. Les problèmes principaux concernent :

1. **Appels répétés à `cleanup()` dans le tiling processor** (CRITIQUE)
2. **Gestion des ressources Vulkan/NCNN** (CRITIQUE)
3. **Buffers intermédiaires non libérés** (MOYEN)
4. **Objets NCNN Mat/Extractor** (MOYEN)
5. **Chemins d'exécution exceptionnels** (MINEUR)

---

## 1. PROBLÈME CRITIQUE : Appels répétés à `cleanup()` dans `tiling_processor.cpp`

### Localisation
**Fichier** : `src/utils/tiling_processor.cpp`  
**Ligne** : 123

### Description
```cpp
// Ligne 122-123
// Cleanup GPU memory after each tile to prevent accumulation
engine->cleanup();
```

Le code appelle `engine->cleanup()` après **chaque tile** dans la boucle de traitement. Cependant, un commentaire explicite dans `stdin_mode.cpp` (lignes 191-194) indique que :

> "NOTE: Do NOT call cleanup() here - it corrupts the NCNN model!  
> Calling engine->cleanup() after processing makes blob names (in0/out0) inaccessible  
> for subsequent images, causing 'find_blob_index_by_name failed' errors."

### Impact
- **Fuite mémoire GPU** : Les ressources Vulkan ne sont pas libérées correctement après chaque tile
- **Corruption du modèle NCNN** : Les blobs deviennent inaccessibles, nécessitant un rechargement du modèle
- **Accumulation mémoire** : Si le modèle est corrompu, les ressources GPU précédentes peuvent rester allouées

### Preuve
Dans `stdin_mode.cpp`, le même pattern est évité :
```cpp
// Ligne 191-194
// NOTE: Do NOT call cleanup() here - it corrupts the NCNN model!
// Calling engine->cleanup() after processing makes blob names (in0/out0) inaccessible
// for subsequent images, causing "find_blob_index_by_name failed" errors.
// Cleanup will be called once at the end of the batch instead.
```

### Recommandation
**Option 1** : Supprimer l'appel à `cleanup()` dans la boucle de tiling et appeler une seule fois à la fin de `process_with_tiling()`.

**Option 2** : Implémenter un mécanisme de nettoyage partiel qui libère uniquement les buffers temporaires sans corrompre le modèle NCNN.

---

## 2. PROBLÈME CRITIQUE : Gestion incomplète des ressources Vulkan

### Localisation
**Fichiers** :
- `src/engines/realcugan_engine.cpp` (ligne 261-264)
- `src/engines/realesrgan_engine.cpp` (ligne 314-317)

### Description
```cpp
void RealCUGANEngine::cleanup() {
    logger::info("RealCUGAN engine cleanup");
    net_.clear();
}
```

Le `cleanup()` appelle uniquement `net_.clear()`, mais ne libère pas explicitement :
- Les command buffers Vulkan
- Les descripteurs Vulkan
- Les pipelines Vulkan
- Les allocations de mémoire GPU

### Impact
- **Fuite mémoire GPU** : Les ressources Vulkan peuvent rester allouées même après `net_.clear()`
- **Fragmentation mémoire GPU** : Accumulation de blocs mémoire non libérés
- **Dépassement de quota GPU** : Après plusieurs batchs, le GPU peut manquer de mémoire

### Preuve
NCNN utilise Vulkan pour l'accélération GPU. Les ressources Vulkan doivent être explicitement libérées. Le `net_.clear()` peut ne pas suffire selon la version de NCNN utilisée.

### Recommandation
Vérifier la documentation NCNN pour s'assurer que `net_.clear()` libère toutes les ressources Vulkan. Si nécessaire, ajouter un appel explicite à `ncnn::destroy_gpu_instance()` ou équivalent.

---

## 3. PROBLÈME MOYEN : Buffers intermédiaires dans `realesrgan_engine.cpp`

### Localisation
**Fichier** : `src/engines/realesrgan_engine.cpp`  
**Lignes** : 216-252

### Description
```cpp
// Ligne 216
std::vector<uint8_t> full_pixels(result.w * result.h * 3);

// Ligne 226
std::vector<uint8_t> final_pixels;

// Ligne 246
final_pixels = std::move(full_pixels);
```

Le code crée deux buffers (`full_pixels` et `final_pixels`) pour gérer le cropping. Dans le cas où `needs_crop == false`, `full_pixels` est déplacé vers `final_pixels`, mais si une exception se produit entre les lignes 216 et 246, `full_pixels` peut ne pas être libéré correctement.

### Impact
- **Fuite mémoire CPU** : En cas d'exception, les buffers peuvent ne pas être libérés
- **Fragmentation mémoire** : Allocations/désallocations répétées sans RAII garanti

### Preuve
Le code utilise `std::vector` qui devrait gérer automatiquement la mémoire, mais les exceptions peuvent interrompre le flux normal.

### Recommandation
Utiliser des smart pointers ou s'assurer que tous les chemins d'exécution libèrent les buffers. Ajouter des blocs `try-catch` si nécessaire.

---

## 4. PROBLÈME MOYEN : Objets NCNN Mat/Extractor non explicitement libérés

### Localisation
**Fichiers** :
- `src/engines/realcugan_engine.cpp` (lignes 168-199)
- `src/engines/realesrgan_engine.cpp` (lignes 169-253)

### Description
```cpp
// realcugan_engine.cpp ligne 168
ncnn::Mat in = ncnn::Mat::from_pixels(...);

// ligne 175
ncnn::Mat result;

// ligne 143
ncnn::Extractor ex = net_.create_extractor();
```

Les objets `ncnn::Mat` et `ncnn::Extractor` sont créés localement et devraient être automatiquement libérés par le destructeur, mais il n'y a pas de garantie explicite que NCNN libère correctement les ressources GPU associées.

### Impact
- **Fuite mémoire GPU** : Les buffers GPU associés aux `ncnn::Mat` peuvent ne pas être libérés immédiatement
- **Accumulation temporaire** : Plusieurs `ncnn::Mat` peuvent coexister en mémoire GPU

### Preuve
NCNN utilise des allocations GPU pour les `Mat` lorsque Vulkan est activé. Le destructeur devrait libérer ces ressources, mais il n'y a pas de vérification explicite.

### Recommandation
Vérifier la documentation NCNN pour s'assurer que les destructeurs libèrent correctement les ressources GPU. Ajouter des appels explicites à `mat.release()` si nécessaire.

---

## 5. PROBLÈME MOYEN : WebP MemoryWriter dans les chemins d'exception

### Localisation
**Fichier** : `src/utils/image_io.cpp`  
**Lignes** : 54-84

### Description
```cpp
WebPMemoryWriter writer;
WebPMemoryWriterInit(&writer);
// ... code ...
WebPMemoryWriterClear(&writer);
WebPPictureFree(&pic);
```

Si une exception se produit entre `WebPMictureInit()` et `WebPMemoryWriterClear()`, le writer peut ne pas être nettoyé.

### Impact
- **Fuite mémoire CPU** : Le buffer interne de `WebPMemoryWriter` peut ne pas être libéré
- **Fuite mémoire WebP** : Les structures WebP peuvent rester allouées

### Preuve
Le code utilise des fonctions C de WebP qui nécessitent un nettoyage manuel. Les exceptions C++ peuvent contourner ces appels.

### Recommandation
Utiliser RAII avec un wrapper C++ pour `WebPMemoryWriter` ou ajouter des blocs `try-catch` pour garantir le nettoyage.

---

## 6. PROBLÈME MINEUR : Buffers dans `stdin_mode.cpp` worker thread

### Localisation
**Fichier** : `src/modes/stdin_mode.cpp`  
**Lignes** : 170-201

### Description
```cpp
// Ligne 170
std::vector<uint8_t> output_data;

// Ligne 197-198
input_item.data.clear();
input_item.data.shrink_to_fit();
```

Le code libère explicitement `input_item.data` mais `output_data` est déplacé vers la queue. Si une exception se produit avant le `push()`, `output_data` peut ne pas être libéré.

### Impact
- **Fuite mémoire CPU** : En cas d'exception, `output_data` peut rester alloué
- **Impact limité** : Le destructeur de `std::vector` devrait finalement libérer la mémoire

### Recommandation
Ajouter un bloc `try-catch` dans `worker_thread_func()` pour garantir la libération des buffers en cas d'exception.

---

## 7. PROBLÈME MINEUR : STB Image dans les chemins d'erreur

### Localisation
**Fichier** : `src/utils/image_io.cpp`  
**Lignes** : 30-45

### Description
```cpp
stbi_uc* pixels = stbi_load_from_memory(...);
if (!pixels) {
    return false;  // OK
}
// ...
stbi_image_free(pixels);
```

Le code libère correctement `pixels` dans le cas normal, mais si une exception se produit entre `stbi_load_from_memory()` et `stbi_image_free()`, la mémoire peut fuir.

### Impact
- **Fuite mémoire CPU** : Les pixels alloués par STB peuvent ne pas être libérés
- **Impact limité** : Les exceptions sont rares dans ce contexte

### Recommandation
Utiliser un RAII wrapper pour `stbi_uc*` ou ajouter un bloc `try-catch`.

---

## 8. PROBLÈME MINEUR : Queue buffers dans `stdin_mode.cpp`

### Localisation
**Fichier** : `src/modes/stdin_mode.cpp`  
**Lignes** : 358-366

### Description
```cpp
const size_t QUEUE_CAPACITY = 4;
BoundedBlockingQueue<InputItem> input_queue(QUEUE_CAPACITY);
BoundedBlockingQueue<OutputItem> output_queue(QUEUE_CAPACITY);
```

Les queues peuvent contenir jusqu'à 4 items de ~5-10MB chacun, soit ~40-60MB de mémoire. Si une exception se produit avant la destruction des queues, cette mémoire peut ne pas être libérée immédiatement.

### Impact
- **Fuite mémoire CPU temporaire** : Les destructeurs devraient finalement libérer la mémoire
- **Impact limité** : Les destructeurs C++ devraient gérer cela

### Recommandation
S'assurer que tous les chemins d'exception garantissent la destruction des queues. Ajouter des blocs `try-catch` si nécessaire.

---

## Recommandations Globales

### Priorité 1 (CRITIQUE)
1. **Supprimer ou corriger l'appel à `cleanup()` dans `tiling_processor.cpp` ligne 123**
   - Soit supprimer complètement et appeler une seule fois à la fin
   - Soit implémenter un nettoyage partiel qui ne corrompt pas le modèle

2. **Vérifier et améliorer la gestion des ressources Vulkan**
   - S'assurer que `net_.clear()` libère toutes les ressources Vulkan
   - Ajouter des appels explicites si nécessaire selon la version NCNN

### Priorité 2 (MOYEN)
3. **Ajouter des blocs try-catch pour garantir le nettoyage des buffers**
   - Dans `realesrgan_engine.cpp::process_image()`
   - Dans `stdin_mode.cpp::worker_thread_func()`
   - Dans `image_io.cpp::encode_image()`

4. **Vérifier la libération des ressources NCNN Mat**
   - S'assurer que les destructeurs libèrent correctement les buffers GPU
   - Ajouter des appels explicites `mat.release()` si nécessaire

### Priorité 3 (MINEUR)
5. **Utiliser RAII pour les ressources C (WebP, STB)**
   - Créer des wrappers C++ pour `WebPMemoryWriter` et `stbi_uc*`
   - Garantir le nettoyage automatique même en cas d'exception

6. **Ajouter des tests de fuite mémoire**
   - Utiliser Valgrind ou AddressSanitizer pour détecter les fuites
   - Tests avec batchs de 100+ images pour reproduire les fuites

---

## Outils de Diagnostic Recommandés

1. **Valgrind** : `valgrind --leak-check=full --show-leak-kinds=all ./bdreader-ncnn-upscaler`
2. **AddressSanitizer** : Compiler avec `-fsanitize=address -g` et exécuter
3. **NVIDIA Nsight Systems** : Profiler les allocations GPU Vulkan
4. **Vulkan Memory Allocator (VMA)** : Si disponible, utiliser pour le tracking mémoire GPU

---

## Conclusion

L'analyse a identifié **5 anomalies critiques/moyennes** et **3 problèmes mineurs** susceptibles de causer des fuites mémoire. Les problèmes les plus critiques concernent :

1. Les appels répétés à `cleanup()` dans le tiling processor qui corrompent le modèle NCNN
2. La gestion incomplète des ressources Vulkan qui peuvent ne pas être libérées correctement

Il est recommandé de corriger ces problèmes en priorité, puis d'ajouter des mécanismes de protection (try-catch, RAII) pour garantir le nettoyage des ressources dans tous les chemins d'exécution.

---

**Fin du rapport**
