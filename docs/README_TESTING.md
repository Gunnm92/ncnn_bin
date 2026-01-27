# Guide de Test des Modèles NCNN

Ce guide explique comment tester tous les modèles RealCUGAN et RealESRGAN avec le binaire `bdreader-ncnn-upscaler`.

## Script de Test Automatique

Le script `test_all_models.py` teste automatiquement tous les modèles disponibles et génère un rapport détaillé.

### Utilisation

```bash
cd /config/workspace/BDReader-Rust
python3 ncnn_bin/test_all_models.py
```

### Ce que fait le script

1. **Découverte automatique** : Trouve tous les modèles RealCUGAN et RealESRGAN dans `backend/models/`
2. **Tests individuels** : Teste chaque modèle avec 5 images de test
3. **Tests batch** : Teste le traitement par batch de 5 images
4. **Métriques** : Mesure les temps CPU/GPU pour chaque image et le batch
5. **Rapport** : Génère un rapport Markdown et JSON dans `ncnn_bin/results/`

### Résultats

Le script génère deux fichiers dans `ncnn_bin/results/` :

- `TEST_REPORT_YYYYMMDD_HHMMSS.md` : Rapport Markdown détaillé avec :
  - Résumé exécutif
  - Tableau comparatif de tous les modèles
  - Détails par modèle (temps par image, statistiques, throughput batch)
  
- `TEST_REPORT_YYYYMMDD_HHMMSS.json` : Données brutes en JSON pour analyse

### Configuration

Les paramètres peuvent être modifiés dans le script :

```python
NUM_TEST_IMAGES = 5      # Nombre d'images à tester
BATCH_SIZE = 5           # Taille du batch
BINARY_PATH = "..."      # Chemin vers le binaire
MODELS_BASE = "..."       # Chemin vers les modèles
TEST_IMAGES_DIR = "..."   # Dossier des images de test
RESULTS_DIR = "..."       # Dossier de sortie
```

## Structure des Résultats

### Modèles RealCUGAN

Les modèles RealCUGAN sont organisés en deux dossiers :
- `models-se/` : Modèles avec support denoising
- `models-nose/` : Modèles sans denoising

Chaque modèle est testé avec :
- **Scale** : 2x (fixe pour RealCUGAN)
- **Noise level** : -1 (aucun), 0, 1, 2, 3
- **Quality** : F (Fast), E (Balanced), Q (Quality), H (High)

### Modèles RealESRGAN

Les modèles RealESRGAN sont testés avec différents scales :
- **x2** : 2x upscale
- **x3** : 3x upscale
- **x4** : 4x upscale

## Métriques Mesurées

Pour chaque modèle, le script mesure :

### Tests Individuels (5 images)
- **Temps CPU** : Temps total d'exécution (par image)
- **Temps GPU** : Temps d'inférence GPU (par image)
- **Moyenne** : Temps moyen par image
- **Min/Max** : Temps minimum et maximum

### Tests Batch (5 images)
- **Temps total batch** : Temps pour traiter toutes les images
- **Temps GPU cumulé** : Somme des temps GPU individuels
- **Throughput** : Images par seconde (batch_size / temps_total)

## Exemple de Rapport

Le rapport généré contient :

```markdown
## 📊 Résumé Exécutif
- Total de modèles testés: 12
- Modèles réussis: 12
- Modèles échoués: 0

## 📈 Tableau Comparatif
| Modèle | Engine | Scale | Quality/Noise | Temps moyen/image | Throughput batch |
|--------|--------|-------|---------------|-------------------|------------------|
| up2x-conservative | realcugan | 2 | F | 1.12s | 0.93 img/s |
| realesr-animevideov3-x2 | realesrgan | 2 | - | 0.96s | 1.08 img/s |
...
```

## Notes Techniques

- **Temps CPU vs GPU** : Actuellement, le binaire ne sort pas de métriques JSON détaillées avec `--profiling`, donc le temps total est utilisé comme approximation pour CPU et GPU.
- **Mode batch** : Le script simule le batch en faisant plusieurs appels séquentiels. Pour un vrai test batch avec protocole binaire, il faudrait implémenter le protocole décrit dans `NCNN_STDIN_STDOUT_SPEC.md`.
- **Timeout** : Chaque image a un timeout de 120 secondes pour éviter les blocages.

## Dépannage

### Erreur "Binaire introuvable"
Vérifiez que le binaire est compilé :
```bash
ls -la /config/workspace/BDReader-Rust/ncnn_bin/build/bdreader-ncnn-upscaler
```

### Erreur "Images introuvables"
Vérifiez que les images de test existent :
```bash
ls /config/workspace/BDReader-Rust/ncnn_bin/img_test/*.jpg
```

### Timeout sur certains modèles
Certains modèles peuvent être plus lents. Augmentez le timeout dans le script si nécessaire.

## Améliorations Futures

- [ ] Implémenter le vrai mode batch avec protocole binaire
- [ ] Extraire les métriques GPU/CPU détaillées depuis les logs
- [ ] Ajouter des graphiques de performance
- [ ] Comparaison visuelle des résultats
- [ ] Tests avec différents GPU (multi-GPU)

## Tests du protocole v2 keep-alive

- **Unité** : `protocol_request_payload_test` (CMake target) vérifie le parsing `BRDR`/meta/batch_count/images` et rejette un `batch_count` > `--max-batch-items`. Il se compile avec `cmake --build build --target protocol_request_payload_test` puis `ctest -R protocol_request_payload_test`.  
- **Intégration** : `tests/protocol_v2_integration.py` construit un message encodé, lance `bdreader-ncnn-upscaler --mode stdin --keep-alive --protocol v2`, envoie deux images encodées et valide que la réponse contient `status_code == 0`, un `result_count` égal au batch demandé et deux sorties. Lancer avec `python3 tests/protocol_v2_integration.py --binary /chemin/binaire`.
- **Stress keep-alive** : `tests/protocol_v2_keepalive.py` ouvre un seul process, envoie 10 requêtes successives, injecte une trame invalide et vérifie que la suivante réussit toujours, puis termine proprement. Lancer avec `python3 tests/protocol_v2_keepalive.py --binary ./build/bdreader-ncnn-upscaler`.
