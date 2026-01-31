# Batch vs Single Image - Analyse Comparative

**Date:** 2026-01-30
**GPU:** --gpu-id 0
**Images:** 16 photos JPEG (165 KB - 1.9 MB)
**Total:** 9.63 MB

---

## 🎯 Résultats Synthétiques

### Performance Globale

| Mode | Batch Size | Total Time | Throughput | Efficiency |
|------|------------|------------|------------|------------|
| **Single** | 1 | 19.24s | 0.831 imgs/s | 100% (baseline) |
| **Batch 2** | 2 | 19.36s | 0.827 imgs/s | 49.7% |
| **Batch 4** | 4 | 19.70s | 0.812 imgs/s | 24.4% |
| **Batch 8** | 8 | 19.63s | 0.815 imgs/s | 12.3% |

### Latence par Image

| Mode | Avg Request Time | Latency/Image | Speedup |
|------|------------------|---------------|---------|
| **Single** | 1.202s | 1202 ms | 1.00x |
| **Batch 2** | 2.419s | 1210 ms | 0.99x |
| **Batch 4** | 4.923s | 1231 ms | 0.98x |
| **Batch 8** | 9.809s | 1226 ms | 0.98x |

### Mémoire

| Mode | RAM Start | RAM End | Growth |
|------|-----------|---------|--------|
| **Single** | 174 MB | 373 MB | +199 MB |
| **Batch 2** | 188 MB | 374 MB | +186 MB |
| **Batch 4** | 198 MB | 384 MB | +185 MB |
| **Batch 8** | 215 MB | 394 MB | +178 MB |

---

## 📊 Analyse Détaillée

### Observation Clé : Pas de Gain de Batching

**Constat surprenant :** Le batching **n'améliore pas** la performance avec ce GPU/configuration.

```
Throughput (imgs/sec):
  Single:  0.831  ← baseline
  Batch 2: 0.827  (↓ 0.5%)
  Batch 4: 0.812  (↓ 2.3%)
  Batch 8: 0.815  (↓ 1.9%)
```

**Latence par image reste constante** (~1.2s) quel que soit le batch size.

---

## 🔬 Explications Possibles

### 1. GPU Déjà Saturé en Mode Single

Le GPU est peut-être déjà à 100% d'utilisation lors du traitement d'une seule image à la fois. Dans ce cas:
- Pas de ressources GPU inutilisées à exploiter
- Le batching n'apporte aucun parallélisme supplémentaire
- Traitement séquentiel même avec batch

### 2. Goulot d'Étranglement Mémoire

Avec des images de résolution variable (jusqu'à 1.9 MB):
- La bande passante mémoire GPU peut être le limitant
- Charger plusieurs images simultanément ne change rien
- Le temps est dominé par les transferts CPU↔GPU

### 3. Architecture NCNN Séquentielle

NCNN peut traiter les images séquentiellement même dans un batch:
```
Batch 4:  [img1] → [img2] → [img3] → [img4]  (séquentiel)
Au lieu de: [img1, img2, img3, img4]  (parallèle)
```

### 4. Tile-Based Processing

Le `--tile-size 512` découpe chaque image en tuiles:
- Chaque tuile est traitée séquentiellement
- Le parallélisme est déjà exploité au niveau des tuiles
- Batching d'images n'ajoute rien

---

## 💡 Implications Pratiques

### ✅ Recommandation : Utiliser Single Image Mode

**Pour ce GPU/configuration:**

```bash
# Mode optimal (single image, requests parallèles au niveau backend)
bdreader-ncnn-upscaler \
  --mode stdin \
  --keep-alive \
  --gpu-id 0 \
  --max-batch-items 1  # ← Single image par requête
```

**Avantages:**
- ✅ Même throughput (0.83 imgs/s)
- ✅ Latence minimale par requête (1.2s vs 9.8s)
- ✅ Moins de RAM initiale (174 MB vs 215 MB)
- ✅ Plus réactif (résultats immédiats)

### 📋 Cas d'Usage

| Scénario | Config Recommandée | Raison |
|----------|-------------------|--------|
| **API temps réel** | Single image | Latence minimale par requête |
| **Worker batch** | Single image | Même throughput, plus simple |
| **Queue processing** | Single image | Meilleur contrôle de priorité |

---

## 🎮 Test avec Petites Images (< 300 KB)

**Rappel des tests précédents avec petites images:**

| Batch Size | Throughput | Latency/Img |
|------------|------------|-------------|
| 1 | ~8 imgs/s | 120 ms |
| 2 | ~7.8 imgs/s | 130 ms |
| 4 | ~5.9 imgs/s | 170 ms |

**Observation:** Même sur petites images, le batching dégrade légèrement la performance.

---

## 📈 Graphique Conceptuel

```
Throughput (imgs/sec)
    0.85 │ █
    0.84 │ █
    0.83 │ █ ← Single (baseline)
    0.82 │   █ ← Batch 2
    0.81 │     █ ← Batch 4
    0.80 │     █ ← Batch 8
         └─────────────────────
           1   2   4   8
           Batch Size
```

**Conclusion visuelle:** Performance plate ou légèrement dégradée avec batching.

---

## 🔍 Comparaison Théorique vs Réel

### Attendu (Scaling Linéaire Idéal)

| Batch | Expected Throughput | Expected Efficiency |
|-------|---------------------|---------------------|
| 1 | 0.83 imgs/s | 100% |
| 2 | 1.66 imgs/s | 100% |
| 4 | 3.32 imgs/s | 100% |
| 8 | 6.64 imgs/s | 100% |

### Réel (Mesuré)

| Batch | Actual Throughput | Actual Efficiency |
|-------|-------------------|-------------------|
| 1 | 0.83 imgs/s | 100% |
| 2 | 0.83 imgs/s | **49.7%** ⚠️ |
| 4 | 0.81 imgs/s | **24.4%** ⚠️ |
| 8 | 0.82 imgs/s | **12.3%** ⚠️ |

**Écart majeur:** Le scaling théorique n'est pas atteint.

---

## ⚙️ Optimisations Possibles (Hors Scope Actuel)

### Option 1: Multi-GPU

Si plusieurs GPUs disponibles:
```bash
# Process 1 sur GPU 0
--gpu-id 0 --max-batch-items 1

# Process 2 sur GPU 1
--gpu-id 1 --max-batch-items 1

# → 2x throughput
```

### Option 2: Pipeline CPU Preprocessing

Précharger/décoder images sur CPU pendant GPU processing:
- Thread 1: Decode JPEG (CPU)
- Thread 2: Upscale (GPU)
- Thread 3: Encode WebP (CPU)

### Option 3: Tile-Level Batching

Batcher les tuiles au lieu des images complètes:
- Découper image en 16 tuiles
- Traiter 4 tuiles en parallèle
- Potentiel: meilleur usage GPU

---

## 🎯 Recommandations Finales

### Pour Production

1. **Utiliser `--max-batch-items 1`**
   - Meilleure latence
   - Même throughput
   - Plus simple

2. **Paralléliser au niveau Backend**
   ```
   Client → Load Balancer → [Worker 1, Worker 2, Worker 3, ...]
   Chaque worker: single image mode
   ```

3. **Prioriser Keep-Alive**
   - Gain principal: éviter spawn (10-50x)
   - Batching: gain nul sur ce GPU

### Pour Tests Futurs

- [ ] Tester avec GPU plus puissant (RTX 3080+)
- [ ] Tester avec images très petites (< 100 KB)
- [ ] Tester avec tile-size différent
- [ ] Profiler GPU utilization (nvidia-smi)

---

## ✅ Validation Conformité TODO.md

### Section 7: Gains Mesurables

- ✅ Keep-alive validé: 16 images sans redémarrer
- ✅ Batch N → N résultats: confirmé (8 → 8)
- ⚠️ Gains batch: **nuls sur ce GPU** (découverte importante)

### Conclusion

**Le protocole fonctionne parfaitement, mais le batching d'images n'apporte pas de gain de performance sur ce GPU avec images moyennes/grandes.**

**Recommandation:** Utiliser mode single image (`--max-batch-items 1`) pour minimiser la latence sans sacrifier le throughput.

---

**Test effectué avec:**
```bash
python3 tests/batch_vs_single_comparison.py \
  --gpu-id 0 \
  --num-images 16 \
  --batch-sizes "1,2,4,8"
```
