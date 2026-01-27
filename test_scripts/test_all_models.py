#!/usr/bin/env python3
"""
Script de test pour évaluer tous les modèles RealCUGAN et RealESRGAN
avec mesure des performances CPU/GPU et mode batch.
"""

import os
import subprocess
import time
import json
import glob
from pathlib import Path
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass, asdict
from datetime import datetime

# Configuration
BINARY_PATH = "/config/workspace/BDReader-Rust/ncnn_bin/build/bdreader-ncnn-upscaler"
MODELS_BASE = "/config/workspace/BDReader-Rust/backend/models"
TEST_IMAGES_DIR = "/config/workspace/BDReader-Rust/ncnn_bin/img_test"
RESULTS_DIR = "/config/workspace/BDReader-Rust/ncnn_bin/results"
NUM_TEST_IMAGES = 5
BATCH_SIZE = 5
TEST_CPU = True  # Tester aussi sur CPU (--gpu-id -1)
TEST_GPU = True  # Tester aussi sur GPU (--gpu-id auto)

@dataclass
class TestResult:
    """Résultat d'un test pour un modèle"""
    model_name: str
    engine: str
    model_path: str
    scale: Optional[int] = None
    noise: Optional[int] = None
    quality: Optional[str] = None
    
    # Métriques individuelles (5 images)
    individual_times_cpu: List[float] = None
    individual_times_gpu: List[float] = None
    
    # Métriques batch
    batch_time_cpu: Optional[float] = None
    batch_time_gpu: Optional[float] = None
    
    # Statistiques
    avg_time_per_image: Optional[float] = None
    min_time: Optional[float] = None
    max_time: Optional[float] = None
    batch_throughput: Optional[float] = None  # images/sec
    
    # Erreurs
    error: Optional[str] = None
    
    def __post_init__(self):
        if self.individual_times_cpu is None:
            self.individual_times_cpu = []
        if self.individual_times_gpu is None:
            self.individual_times_gpu = []


def get_test_images() -> List[str]:
    """Récupère les images de test (celles qui commencent par P pour les images couleur)"""
    # Filtrer les images qui commencent par "P"
    all_images = sorted(glob.glob(f"{TEST_IMAGES_DIR}/P*.jpg"))
    images = all_images[:NUM_TEST_IMAGES]
    if len(images) < NUM_TEST_IMAGES:
        print(f"⚠️  Seulement {len(images)} images trouvées, besoin de {NUM_TEST_IMAGES}")
    return images


def discover_realcugan_models() -> List[Dict]:
    """Découvre tous les modèles RealCUGAN disponibles"""
    models = []
    base_path = f"{MODELS_BASE}/realcugan"
    
    # Modèles models-se
    se_path = f"{base_path}/models-se"
    if os.path.exists(se_path):
        for bin_file in glob.glob(f"{se_path}/*.bin"):
            model_name = os.path.basename(bin_file).replace(".bin", "")
            param_file = bin_file.replace(".bin", ".param")
            if os.path.exists(param_file):
                # Déterminer noise level et quality depuis le nom
                noise = -1
                quality = "F"
                if "denoise1x" in model_name:
                    noise = 1
                    quality = "Q"
                elif "denoise2x" in model_name:
                    noise = 2
                    quality = "H"
                elif "denoise3x" in model_name:
                    noise = 3
                    quality = "H"
                elif "no-denoise" in model_name or "conservative" in model_name:
                    noise = -1
                    quality = "F"
                
                models.append({
                    "engine": "realcugan",
                    "model_name": model_name,
                    "model_path": se_path,
                    "noise": noise,
                    "quality": quality,
                    "scale": 2
                })
    
    # Modèles models-nose
    nose_path = f"{base_path}/models-nose"
    if os.path.exists(nose_path):
        for bin_file in glob.glob(f"{nose_path}/*.bin"):
            model_name = os.path.basename(bin_file).replace(".bin", "")
            param_file = bin_file.replace(".bin", ".param")
            if os.path.exists(param_file):
                models.append({
                    "engine": "realcugan",
                    "model_name": model_name,
                    "model_path": nose_path,
                    "noise": -1,
                    "quality": "F",
                    "scale": 2
                })
    
    return models


def discover_realesrgan_models() -> List[Dict]:
    """Découvre tous les modèles RealESRGAN disponibles"""
    models = []
    base_path = f"{MODELS_BASE}/realesrgan"
    
    if not os.path.exists(base_path):
        return models
    
    # Trouver tous les fichiers .bin
    for bin_file in glob.glob(f"{base_path}/*.bin"):
        model_name = os.path.basename(bin_file).replace(".bin", "")
        param_file = bin_file.replace(".bin", ".param")
        
        if not os.path.exists(param_file):
            continue
        
        # Déterminer le scale depuis le nom
        scale = 4  # défaut
        if "x2" in model_name:
            scale = 2
        elif "x3" in model_name:
            scale = 3
        elif "x4" in model_name:
            scale = 4
        
        models.append({
            "engine": "realesrgan",
            "model_name": model_name,
            "model_path": base_path,
            "scale": scale,
            "noise": None,
            "quality": None
        })
    
    return models


def run_single_test(
    engine: str,
    input_image: str,
    output_path: str,
    model_path: str,
    scale: Optional[int] = None,
    noise: Optional[int] = None,
    quality: Optional[str] = None,
    model_name: Optional[str] = None,
    gpu_id: str = "auto"
) -> Tuple[float, float, Optional[str]]:
    """
    Exécute un test unique et retourne (temps_cpu, temps_gpu, erreur)
    """
    args = [
        BINARY_PATH,
        "--engine", engine,
        "--mode", "file",
        "--input", input_image,
        "--output", output_path,
        "--gpu-id", str(gpu_id),
        "--model", model_path,
        "--profiling",
        "--verbose"
    ]
    
    if engine == "realcugan":
        if quality:
            args.extend(["--quality", quality])
        if noise is not None:
            args.extend(["--noise", str(noise)])
    elif engine == "realesrgan":
        if scale:
            args.extend(["--scale", str(scale)])
        if model_name:
            args.extend(["--model-name", model_name])
    
    start_time = time.time()
    try:
        result = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=120  # 2 minutes max par image
        )
        elapsed = time.time() - start_time
        
        if result.returncode != 0:
            error_msg = result.stderr[:500] if result.stderr else "Unknown error"
            return elapsed, elapsed, error_msg
        
        # Parser les métriques depuis stderr (profiling)
        cpu_time = elapsed
        gpu_time = elapsed  # Par défaut, on utilise le temps total
        
        # Essayer d'extraire les métriques depuis stderr
        for line in result.stderr.split('\n'):
            if 'decode' in line.lower() or 'infer' in line.lower() or 'encode' in line.lower():
                # Format JSON possible: {"phase":"infer","ms":320}
                try:
                    if '{' in line:
                        data = json.loads(line)
                        if 'ms' in data:
                            # On prend le temps d'inférence comme référence GPU
                            if 'infer' in data.get('phase', '').lower():
                                gpu_time = data['ms'] / 1000.0
                except:
                    pass
        
        return cpu_time, gpu_time, None
        
    except subprocess.TimeoutExpired:
        return 120.0, 120.0, "Timeout after 120s"
    except Exception as e:
        return 0.0, 0.0, str(e)[:500]


def run_batch_test(
    engine: str,
    input_images: List[str],
    output_dir: str,
    model_path: str,
    scale: Optional[int] = None,
    noise: Optional[int] = None,
    quality: Optional[str] = None,
    model_name: Optional[str] = None,
    gpu_id: str = "auto"
) -> Tuple[float, float, Optional[str]]:
    """
    Exécute un test batch et retourne (temps_cpu, temps_gpu, erreur)
    Note: Le mode batch nécessite un protocole binaire, on simule avec plusieurs appels
    """
    # Pour l'instant, on fait plusieurs appels séquentiels
    # TODO: Implémenter le vrai mode batch avec protocole binaire
    start_time = time.time()
    total_gpu_time = 0.0
    
    for i, img in enumerate(input_images):
        output_path = f"{output_dir}/batch_{i}.webp"
        cpu_time, gpu_time, error = run_single_test(
            engine, img, output_path, model_path,
            scale, noise, quality, model_name, gpu_id
        )
        
        if error:
            return time.time() - start_time, total_gpu_time, error
        
        total_gpu_time += gpu_time
    
    elapsed = time.time() - start_time
    return elapsed, total_gpu_time, None


def test_model(model_config: Dict, test_images: List[str], gpu_id: str = "auto") -> TestResult:
    """Teste un modèle avec toutes les images"""
    device_type = "CPU" if gpu_id == "-1" else "GPU"
    print(f"\n{'='*80}")
    print(f"Testing: {model_config['engine']} - {model_config['model_name']} ({device_type})")
    print(f"{'='*80}")
    
    result = TestResult(
        model_name=f"{model_config['model_name']} ({device_type})",
        engine=model_config['engine'],
        model_path=model_config['model_path'],
        scale=model_config.get('scale'),
        noise=model_config.get('noise'),
        quality=model_config.get('quality')
    )
    
    # Créer le dossier de sortie
    output_dir = f"{RESULTS_DIR}/{model_config['engine']}/{model_config['model_name']}"
    os.makedirs(output_dir, exist_ok=True)
    
    # Tests individuels
    print(f"\n📸 Tests individuels ({len(test_images)} images)...")
    for i, img in enumerate(test_images):
        output_path = f"{output_dir}/img_{i:02d}.webp"
        print(f"  Image {i+1}/{len(test_images)}: {os.path.basename(img)}", end=" ... ", flush=True)
        
        cpu_time, gpu_time, error = run_single_test(
            engine=model_config['engine'],
            input_image=img,
            output_path=output_path,
            model_path=model_config['model_path'],
            scale=model_config.get('scale'),
            noise=model_config.get('noise'),
            quality=model_config.get('quality'),
            model_name=model_config.get('model_name'),
            gpu_id=gpu_id
        )
        
        if error:
            print(f"❌ ERROR: {error[:100]}")
            result.error = error
            break
        
        result.individual_times_cpu.append(cpu_time)
        result.individual_times_gpu.append(gpu_time)
        print(f"✅ {cpu_time:.2f}s (CPU) / {gpu_time:.2f}s (GPU)")
    
    if result.error:
        return result
    
    # Calculer statistiques individuelles
    if result.individual_times_cpu:
        result.avg_time_per_image = sum(result.individual_times_cpu) / len(result.individual_times_cpu)
        result.min_time = min(result.individual_times_cpu)
        result.max_time = max(result.individual_times_cpu)
    
    # Test batch
    print(f"\n📦 Test batch ({len(test_images)} images)...")
    batch_cpu, batch_gpu, batch_error = run_batch_test(
        engine=model_config['engine'],
        input_images=test_images,
        output_dir=output_dir,
        model_path=model_config['model_path'],
        scale=model_config.get('scale'),
        noise=model_config.get('noise'),
        quality=model_config.get('quality'),
        model_name=model_config.get('model_name'),
        gpu_id=gpu_id
    )
    
    if batch_error:
        result.error = f"Batch error: {batch_error}"
    else:
        result.batch_time_cpu = batch_cpu
        result.batch_time_gpu = batch_gpu
        result.batch_throughput = len(test_images) / batch_cpu if batch_cpu > 0 else 0
        print(f"  ✅ Batch: {batch_cpu:.2f}s total ({result.batch_throughput:.2f} img/s)")
    
    return result


def generate_report(results: List[TestResult]) -> str:
    """Génère un rapport markdown"""
    report = []
    report.append("# Rapport de Test - Modèles RealCUGAN et RealESRGAN\n")
    report.append(f"**Date:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
    report.append(f"**Nombre d'images testées:** {NUM_TEST_IMAGES}\n")
    report.append(f"**Taille du batch:** {BATCH_SIZE}\n")
    report.append("\n---\n")
    
    # Résumé
    report.append("## 📊 Résumé Exécutif\n")
    report.append(f"- **Total de modèles testés:** {len(results)}\n")
    successful = len([r for r in results if not r.error])
    report.append(f"- **Modèles réussis:** {successful}\n")
    report.append(f"- **Modèles échoués:** {len(results) - successful}\n")
    
    # Tableau comparatif
    report.append("\n## 📈 Tableau Comparatif\n")
    report.append("| Modèle | Engine | Scale | Quality/Noise | Temps moyen/image | Min | Max | Throughput batch |\n")
    report.append("|--------|--------|-------|---------------|-------------------|-----|-----|------------------|\n")
    
    for r in sorted(results, key=lambda x: (x.engine, x.model_name)):
        if r.error:
            report.append(f"| {r.model_name} | {r.engine} | {r.scale or '-'} | {r.quality or r.noise or '-'} | ❌ ERROR | - | - | - |\n")
        else:
            avg = f"{r.avg_time_per_image:.2f}s" if r.avg_time_per_image else "-"
            min_t = f"{r.min_time:.2f}s" if r.min_time else "-"
            max_t = f"{r.max_time:.2f}s" if r.max_time else "-"
            throughput = f"{r.batch_throughput:.2f} img/s" if r.batch_throughput else "-"
            report.append(f"| {r.model_name} | {r.engine} | {r.scale or '-'} | {r.quality or r.noise or '-'} | {avg} | {min_t} | {max_t} | {throughput} |\n")
    
    # Détails par modèle
    report.append("\n## 🔍 Détails par Modèle\n")
    
    for r in sorted(results, key=lambda x: (x.engine, x.model_name)):
        report.append(f"\n### {r.engine.upper()} - {r.model_name}\n")
        
        if r.error:
            report.append(f"❌ **Erreur:** {r.error}\n")
            continue
        
        report.append(f"- **Chemin modèle:** `{r.model_path}`\n")
        if r.scale:
            report.append(f"- **Scale:** {r.scale}x\n")
        if r.noise is not None:
            report.append(f"- **Noise level:** {r.noise}\n")
        if r.quality:
            report.append(f"- **Quality:** {r.quality}\n")
        
        report.append(f"\n#### Métriques Individuelles (5 images)\n")
        report.append("| Image | Temps CPU | Temps GPU |\n")
        report.append("|-------|-----------|-----------|\n")
        
        for i, (cpu, gpu) in enumerate(zip(r.individual_times_cpu, r.individual_times_gpu)):
            report.append(f"| Image {i+1} | {cpu:.2f}s | {gpu:.2f}s |\n")
        
        report.append(f"\n- **Moyenne:** {r.avg_time_per_image:.2f}s/image\n")
        report.append(f"- **Min:** {r.min_time:.2f}s\n")
        report.append(f"- **Max:** {r.max_time:.2f}s\n")
        
        if r.batch_time_cpu:
            report.append(f"\n#### Métriques Batch\n")
            report.append(f"- **Temps total batch:** {r.batch_time_cpu:.2f}s\n")
            report.append(f"- **Temps GPU cumulé:** {r.batch_time_gpu:.2f}s\n")
            report.append(f"- **Throughput:** {r.batch_throughput:.2f} images/seconde\n")
    
    return "\n".join(report)


def main():
    """Fonction principale"""
    print("🚀 Démarrage des tests de modèles NCNN\n")
    
    # Vérifications
    if not os.path.exists(BINARY_PATH):
        print(f"❌ Binaire introuvable: {BINARY_PATH}")
        return
    
    if not os.path.exists(TEST_IMAGES_DIR):
        print(f"❌ Dossier d'images introuvable: {TEST_IMAGES_DIR}")
        return
    
    # Créer le dossier de résultats
    os.makedirs(RESULTS_DIR, exist_ok=True)
    
    # Découvrir les modèles
    print("🔍 Découverte des modèles...")
    realcugan_models = discover_realcugan_models()
    realesrgan_models = discover_realesrgan_models()
    
    print(f"  ✅ {len(realcugan_models)} modèles RealCUGAN trouvés")
    print(f"  ✅ {len(realesrgan_models)} modèles RealESRGAN trouvés")
    
    # Récupérer les images de test
    test_images = get_test_images()
    print(f"  ✅ {len(test_images)} images de test sélectionnées")
    
    # Tester tous les modèles
    all_results = []
    
    # Déterminer quels devices tester
    devices_to_test = []
    if TEST_GPU:
        devices_to_test.append(("auto", "GPU"))
    if TEST_CPU:
        devices_to_test.append(("-1", "CPU"))
    
    # RealCUGAN
    for model_config in realcugan_models:
        for gpu_id, device_name in devices_to_test:
            result = test_model(model_config, test_images, gpu_id=gpu_id)
            all_results.append(result)
    
    # RealESRGAN
    for model_config in realesrgan_models:
        for gpu_id, device_name in devices_to_test:
            result = test_model(model_config, test_images, gpu_id=gpu_id)
            all_results.append(result)
    
    # Générer le rapport
    print(f"\n📝 Génération du rapport...")
    report = generate_report(all_results)
    
    report_path = f"{RESULTS_DIR}/TEST_REPORT_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md"
    with open(report_path, 'w', encoding='utf-8') as f:
        f.write(report)
    
    print(f"✅ Rapport sauvegardé: {report_path}")
    
    # Sauvegarder aussi en JSON
    json_path = report_path.replace('.md', '.json')
    with open(json_path, 'w', encoding='utf-8') as f:
        json.dump([asdict(r) for r in all_results], f, indent=2, default=str)
    
    print(f"✅ Données JSON sauvegardées: {json_path}")
    
    print("\n🎉 Tests terminés !")


if __name__ == "__main__":
    main()
