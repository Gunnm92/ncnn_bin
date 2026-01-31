# Quick Reference - NCNN Protocol v2

**Binary optimisé:** `/config/workspace/ncnn_bin/bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler`

---

## 🚀 Commandes Rapides

### Lancer le serveur keep-alive

```bash
# Mode CPU (safe, universel)
./bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler \
  --mode stdin \
  --keep-alive \
  --gpu-id -1 \
  --max-batch-items 8 \
  --format webp

# Mode GPU (performance)
./bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler \
  --mode stdin \
  --keep-alive \
  --gpu-id 0 \
  --max-batch-items 8 \
  --tile-size 512 \
  --format webp
```

---

## 🧪 Tests & Validation

### Tests rapides (2 min)

```bash
# Test RAM et performance
python3 tests/quick_ram_test.py

# Test protocole v2
python3 tests/protocol_v2_integration.py \
  --binary bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler \
  --gpu-id -1

# Test keep-alive
python3 tests/protocol_v2_keepalive.py \
  --binary bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler \
  --gpu-id -1
```

### Tests complets (1-2h)

```bash
# Suite complète
python3 tests/ram_performance_tests.py --tests all --gpu-id -1

# Tests individuels
python3 tests/ram_performance_tests.py --tests batch   # Batch scaling
python3 tests/ram_performance_tests.py --tests heavy   # Images lourdes
python3 tests/ram_performance_tests.py --tests leak    # Fuite mémoire
python3 tests/ram_performance_tests.py --tests bench   # Performance
python3 tests/ram_performance_tests.py --tests stress  # Stress test
```

---

## 📊 Résultats Performance (CPU Baseline)

| Métrique | Valeur | Notes |
|----------|--------|-------|
| **Throughput** | 74 imgs/sec | 1x1 PNG, batch 2 |
| **Latency moyenne** | 51 ms | Batch 4 images |
| **RAM stable** | 14-15 MB | +1 MB / 100 req |
| **Aucune fuite** | ✅ | 100 requêtes testées |

---

## 📁 Fichiers Importants

### Binaires
- **Production:** `bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler` (15 MB)
- **Debug:** `bdreader-ncnn-upscaler/build-asan/bdreader-ncnn-upscaler` (225 MB)

### Code source
- **Protocole:** `bdreader-ncnn-upscaler/src/protocol_v2.hpp`
- **Keep-alive loop:** `bdreader-ncnn-upscaler/src/modes/stdin_mode.cpp`

### Tests
- **Quick test:** `tests/quick_ram_test.py`
- **Full suite:** `tests/ram_performance_tests.py`
- **Integration:** `tests/protocol_v2_integration.py`
- **Keep-alive:** `tests/protocol_v2_keepalive.py`

### Documentation
- **Spec:** `TODO.md`
- **Test report:** `RAM_PERFORMANCE_TEST_REPORT.md`
- **Benchmarks:** `PERFORMANCE_BENCHMARKS.md`

---

## 🔧 Configuration Recommandée

### Petite machine (< 2 GB RAM)

```bash
--max-batch-items 2
--tile-size 256
--gpu-id -1
```

### Machine moyenne (2-4 GB RAM)

```bash
--max-batch-items 4
--tile-size 512
--gpu-id 0  # Si GPU disponible
```

### Machine puissante (> 4 GB RAM)

```bash
--max-batch-items 8
--tile-size 512
--gpu-id 0
```

---

## 📋 Checklist de Production

- [x] Code optimisé et nettoyé
- [x] Tests protocole passent (100%)
- [x] Tests RAM passent (aucune fuite)
- [x] Performance validée (74 imgs/sec CPU)
- [x] Documentation complète
- [ ] Benchmarks GPU (nécessite matériel)
- [ ] Tests backend Rust (intégration)

---

## 🐛 Debugging

### Activer logs détaillés

```bash
# Logs protocole
--log-protocol

# Profiling par requête
--profiling

# Verbose général
--verbose
```

### Analyser stderr

Tous les logs vont sur stderr (stdout = données binaires pures).

```bash
./bdreader-ncnn-upscaler ... 2> debug.log
```

---

## ⚡ Performance Tips

1. **Keep-alive toujours** : ~10-50x plus rapide que spawn
2. **Batch optimal** : 4-8 images selon RAM
3. **GPU si possible** : ~5-10x plus rapide que CPU
4. **Format WebP** : Meilleur ratio qualité/taille
5. **Tile-size auto** : `--tile-size 0` laisse NCNN décider

---

## 🔗 Liens Rapides

- 📘 [Spécification complète](TODO.md)
- 📊 [Benchmarks détaillés](PERFORMANCE_BENCHMARKS.md)
- 🧪 [Rapport de tests](RAM_PERFORMANCE_TEST_REPORT.md)
- 💻 [Repo GitHub](https://github.com/your-org/your-repo) *(à ajouter)*

---

**Dernière mise à jour:** 2026-01-30
