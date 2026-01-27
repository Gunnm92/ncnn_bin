# NCNN Upscaler - Modèles Fonctionnels

## ✅ Modèles Validés et Fonctionnels

### RealCUGAN (Recommandé pour Manga/Anime avec Denoising)

**Tous les modèles fonctionnent parfaitement!**

| Quality | Model File | Scale | Denoise | Mean | Status |
|---------|-----------|-------|---------|------|--------|
| F (Fast) | up2x-no-denoise | 2x | None | 179.2 | ✅ |
| E (Balanced) | up2x-denoise1x | 2x | Level 1 | 179.4 | ✅ |
| Q (Quality) | up2x-denoise2x | 2x | Level 2 | 179.5 | ✅ |
| H (High) | up2x-denoise3x | 2x | Level 3 | 179.7 | ✅ |

**Usage:**
```bash
./build/bdreader-ncnn-upscaler \
  --engine realcugan \
  --quality E \
  --model /path/to/models/realcugan/models-se \
  --input image.jpg \
  --output result.webp
```

### RealESRGAN AnimevVideoV3 (Recommandé pour Usage Général)

**Tous les modèles x2/x3/x4 fonctionnent parfaitement!**

| Model | Scale | Mean | Output Size (from 900x1221) | Status |
|-------|-------|------|------------------------------|--------|
| realesr-animevideov3-x2 | 2x | 184.2 | 1800x2442 | ✅ |
| realesr-animevideov3-x3 | 3x | - | 2700x3663 | ✅ |
| realesr-animevideov3-x4 | 4x | 183.8 | 3600x4884 | ✅ |

**Usage:**
```bash
./build/bdreader-ncnn-upscaler \
  --engine realesrgan \
  --scale 2 \
  --model /path/to/models/realesrgan \
  --input image.jpg \
  --output result.webp
```

## ❌ Modèles Non Supportés

### RealESRGAN x4plus-anime

**Problème:** Produit des images très sombres (Mean=58.5 au lieu de 183.6)

**Raison:** Problème de normalisation spécifique à ce modèle dans notre implémentation. Le binaire officiel fonctionne correctement mais pas notre implémentation custom.

**Particularité:** Fonctionne étrangement sur les images noir & blanc mais pas sur les couleurs.

**Solution:** Utiliser le binaire officiel `realesrgan-ncnn-vulkan` si ce modèle est nécessaire.

### RealESRGAN General (Convertis depuis PyTorch)

Les modèles convertis depuis PyTorch ont des problèmes de normalisation:
- `realesr-general-x4v3`: Mean=13.5 (trop sombre)
- `realesr-general-wdn-x4v3`: Mean=10.5 (trop sombre)

## Recommandations d'Usage

### Pour Manga/Anime avec Bruit
→ **RealCUGAN** avec quality E ou Q
- Denoise intégré
- Excellent pour les scans de qualité moyenne
- Rapide (2x seulement)

### Pour Manga/Anime Propre
→ **RealESRGAN animevideov3-x2** ou **x4**
- Pas de denoise
- Meilleur pour les sources propres
- Support 2x/3x/4x

### Pour Haute Qualité 4x
→ **RealESRGAN animevideov3-x4**
- Meilleur compromis qualité/compatibilité
- Testé et validé
- Pas de problème de normalisation

## Performance (NVIDIA RTX 3090)

- **RealCUGAN (2x)**: ~1-2s par image
- **RealESRGAN animevideov3 (2x)**: ~1-2s par image
- **RealESRGAN animevideov3 (4x)**: ~2-3s par image

## Intégration Backend

Le binaire NCNN sera intégré dans le backend Rust avec:
- Support du batch processing
- Gestion automatique GPU/CPU fallback
- Sélection intelligente des modèles selon les besoins
- Streaming stdin/stdout pour éviter les I/O disque

Voir: `/config/workspace/BDReader-Rust/backend/docs/NCNN_STDIN_STDOUT_SPEC.md`
