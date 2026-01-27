# TODO — Upscale NCNN: batch via stdin/stdout + keep-alive

## Objectif
Un seul process `bdreader-ncnn-upscaler` reste vivant (`--keep-alive`) et traite plusieurs requêtes batch successives via `stdin/stdout` (sans I/O disque).

## 1) Définir/valider le protocole (v2 recommandé)
- [x] Conserver `--mode stdin --keep-alive` comme déclencheur du protocole streamé
- [x] Retirer l’option `--protocol` et rendre la version v2 implicite : `--keep-alive` active toujours le framing BRDR 2.
- [x] Utiliser un framing robuste: `[u32 message_len][payload]` (little-endian)
- [x] Inclure un header minimal dans `payload`:
  - [x] magic (`"BRDR"` = `0x42524452`)
  - [x] version (`2`)
  - [x] msg_type (request/response)
  - [x] request_id (corrélation)
  - [x] valider la longueur totale + garder la loop vivant avec erreurs structurées

## 2) Spécification request (batch)
- [x] Payload request contient:
  - [x] engine (enum)
  - [x] quality/scale (string length + bytes)
  - [x] gpu_id (i32)
  - [x] batch_count (u32)
  - [x] images: répétition de `[u32 img_len][img_bytes]`
- [x] Rejeter proprement `batch_count > --max-batch-items`
- [x] Appliquer des limites de sécurité (message_len/image_len)

## 3) Spécification response
- [x] Payload response contient:
  - [x] request_id
  - [x] status_code (0 = OK)
  - [x] error_len + error_bytes (UTF-8)
  - [x] result_count
  - [x] outputs: répétition de `[u32 out_len][out_bytes]`
- [x] Contrat:
  - [x] si `status_code == 0` → `result_count == batch_count`
  - [x] ordre des outputs = ordre des inputs

## 4) Boucle keep-alive (comportement)
- [x] Initialiser les modèles/engine une seule fois
- [x] Boucler jusqu’à EOF stdin:
  - [x] lire `message_len`
  - [x] lire `payload`
  - [x] parser / valider
  - [x] traiter le batch
  - [x] écrire la response
  - [x] flush stdout
- [x] En cas d’erreur de parsing/traitement:
  - [x] ne pas quitter
  - [x] renvoyer une erreur structurée si possible

## 5) Compatibilité legacy (important pour transition)
- [x] backend migre vers v2
- [x] retirer les références à `--protocol v1` / `--batch-size` pour éviter toute confusion : seul le protocole v2 gardera le process en vie

## 6) Observabilité / debug
- [x] Logguer une ligne par requête: request_id, engine, quality, gpu_id, batch_count, timings
- [x] Logguer clairement les erreurs de protocole (sans spam binaire)
- [x] Optionnel: `--log-protocol` pour debug bas niveau
- [x] Définir précisément `--profiling` en mode keep-alive (ligne métrique par requête: request_id, status_code, batch_count, bytes_in/out, error_len, latence)
- [x] Éviter toute sortie non binaire sur stdout si stdout porte le protocole

## 7) Critères d’acceptation (tests)
- [x] Un même process traite 10 requêtes batch successives sans redémarrer
- [x] Un batch N renvoie N résultats (et dans le bon ordre)
- [x] Une requête invalide renvoie une erreur, la suivante fonctionne
- [x] `batch_count > --max-batch-items` → erreur propre
- [ ] Gains mesurables vs spawn par requête
- [ ] Scénario “petite machine” (RAM limitée) ne crash pas et reste réactif

## 8) Notes backend (déjà amorcé côté Rust)
- [x] Pool keep-alive côté backend
- [x] Batch worker branché sur keep-alive
- [ ] Migrer le backend vers un protocole v2 batché (si/une fois dispo)
- [ ] Ajouter la spécification technique détaillée du protocole NCNN v2 (header BRDR, request/response, limites, status)

## Protocole NCNN v2 - Spécification Technique

**Binaire:** `bdreader-ncnn-upscaler`  
**Mode:** `--mode stdin --keep-alive`  
**Version:** 2  
**Date:** 2026-01-27

### 📐 Format du Protocole

#### REQUEST (stdin → binaire)

```
Frame Length (u32 LE) = 4
BRDR Header = 16
   Magic      (u32 LE) = 0x42524452 ("BRDR")
   Version    (u32 LE) = 2
   Msg Type   (u32 LE) = 1 (REQUEST)
   Request ID (u32 LE)
Payload:
   Number of Images (u32 LE)
   Pour chaque image :
       Image Length (u32 LE)
       Image Bytes (PNG/JPG/WEBP)
```

#### RESPONSE (binaire → stdout)

```
Payload Length (u32 LE) = 4
Payload:
   Request ID                (u32 LE)
   Status Code               (u32 LE)
   Error Message Length      (u32 LE)
   Error Message (UTF-8)     (bytes)
   Result Count              (u32 LE)
   Pour chaque résultat :
       Output Length (u32 LE)
       Output Bytes (WEBP)
```

### 🔢 Constantes

```
BRDR_MAGIC              = 0x42524452
BRDR_VERSION            = 2
BRDR_MSG_TYPE_REQUEST   = 1
MAX_MESSAGE_SIZE        = 64 MiB
MAX_IMAGE_SIZE          = 50 MiB
MAX_BATCH_PAYLOAD       = 48 MiB total
STATUS_OK               = 0
STATUS_INVALID_FRAME    = 1
STATUS_VALIDATION_ERROR = 2
STATUS_RESOURCE_LIMIT   = 3
STATUS_ENGINE_ERROR     = 4
STATUS_TIMEOUT          = 5
```

### ⚙️ Arguments CLI (fournis au binaire, pas par requête)

```
bdreader-ncnn-upscaler \
  --engine realcugan \
  --mode stdin \
  --keep-alive \
  --model <path> \
  --quality <Q> \
  --scale <N> \
  --gpu-id <id> \
  --format webp \
  --max-batch-items 8 \
  --tile-size 512 \
  --log-protocol \
  --verbose
```

### 📝 Exemple de Requête (Rust)

voir documentation externe/internes (script référence)

### 🐛 Problèmes Connus (2026-01-27)

- msg_type non reconnu → parser header BRDR + log détaillé
- request_id erroné dans réponse → vérifier parse et echo
- tests de référence : `tests/protocol_v2_integration.py`, plus large suite batch

### ✅ Checklist Protocole

- [x] Parser header magic/version/msg_type
- [x] Valider request_id/response
- [x] Ajouter tests de référence (payload Python)
- [x] Mettre à jour doc si format diffère

## 9) Flags CLI vus dans `--help` à aligner
- [x] `--model-name` (RealESRGAN) garde la priorité sur `--scale`; si vide, la sélection automatique reste `realesr-animevideov3-x{scale}`.
- [x] `--tile-size 0` laisse l’engine décider (≈512 avec overlap/thresholds); >0 impose un plafond tiling utile pour petites machines.
- [x] `--format` fixe l’encodage (`webp`/`png`/`jpg`) et doit rester stable pour que le client sache quoi parser.
- [x] `--mode stdin --keep-alive` active le protocole BRDR v2 ; les anciens `--mode batch`/`--batch-size` ont été retirés.

## 10) Objectifs RAM & petites machines
- [x] Objectif explicite: limiter les buffers citoyens en gardant le process streaming et en documentant les budgets (64 MiB par message, 50 MiB par image, ~48 MiB par batch).
- [x] Définir un budget RAM cible en restreignant les données compressées retransmises et en prodiguant des recommandations tile-size/format dans la doc.
- [x] `message_len`, `img_len` et `max_batch_items` sont plafonnés en code (`protocol_v2.hpp`) pour limiter l’empreinte mémoire.
- [x] Le batch effectif est déjà limité par la somme des bytes compressés; toute surcharge déclenche `ResourceLimit` sans planter.
- [ ] Écrire la réponse progressive sans charger tous les outputs reste compliqué à cause du header `payload_len`; on note cette limite pour une amélioration future.
- [x] En cas de pression mémoire, on encourage à réduire `--tile-size` / `--max-batch-items` (documenté).

## 11) Tests RAM / robustesse (orientés petites machines)
- [ ] Test “batch large” mais images petites: pas d’explosion RAM
- [ ] Test “images lourdes”: erreur propre (pas OOM, pas crash)
- [ ] Test longue session keep-alive (ex: 50–200 requêtes): pas de fuite mémoire visible
- [ ] Test avec `--tile-size` petit vs grand: vérifier le compromis RAM / perf
- [ ] Test de backpressure: stdin envoie plus vite que le GPU ne traite
- [ ] Mesurer au moins: pic RAM, temps moyen, taux d’erreur

## 12) Gestion des erreurs (contrat + résilience)
- [x] Les logs/profiling restent sur `stderr`; `stdout` reste pur flux binaire.
- [x] Toute erreur connue renvoie une réponse structurée (avec `request_id` quand possible et `status_code` précis).
- [x] `status_code` intègre les catégories protocole/validation/ResourceLimit/engine/Timeout; documentation mise à jour.
- [x] Les messages d’erreur courts + actionnables remontent dans `error_bytes` des réponses (et sont consignés dans les logs).
- [x] Le loop keep-alive ignore les erreurs et continue tant que stdin n’indique pas shutdown.
- [x] Le parsing se resynchronise via `message_len`, et les tailles/magic/batch_count/budget sont validés strictement.
- [ ] Timeout par requête et fallback GPU→IGPU→CPU sont gérés côté moteur (génération d’erreurs Atom), à explorer pour l’avenir.
- [x] Aucune réponse partielle silencieuse : soit `Ok` avec tous les outputs, soit `status!=0` avec série complète.

## 13) Tests d’erreurs (protocole + runtime)
- [ ] Magic/version invalides → erreur propre, requête suivante OK
- [ ] `message_len` invalide/trop grand → erreur propre, pas de crash
- [ ] `batch_count` incohérent vs payload → erreur propre
- [ ] Image invalide/corrompue → erreur explicite
- [ ] GPU indisponible/échec init → erreur explicite ou fallback maîtrisé
- [ ] Timeout traitement → erreur explicite et process toujours vivant
