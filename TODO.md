# TODO — Upscale NCNN: batch via stdin/stdout + keep-alive

## Objectif
Un seul process `bdreader-ncnn-upscaler` reste vivant (`--keep-alive`) et traite plusieurs requêtes batch successives via `stdin/stdout` (sans I/O disque).

## 1) Définir/valider le protocole (v2 recommandé)
- [x] Conserver `--mode stdin --keep-alive` comme déclencheur du protocole streamé
- [x] Ajouter/figer `--protocol` (ou implicite si `--keep-alive`)
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
- [ ] Définir précisément `--profiling` en mode keep-alive
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

## 9) Flags CLI vus dans `--help` à aligner
- [ ] `--model-name` (RealESRGAN): définir la priorité vs `--scale`
- [ ] `--tile-size`: documenter la sémantique de `0` (auto?) et les recommandations batch
- [ ] `--format`: garantir un format stable en sortie (et envisager de le renvoyer dans la réponse)
- [ ] `--mode batch`: clarifier la différence vs `--mode stdin --batch-size > 0`

## 10) Objectifs RAM & petites machines
- [ ] Objectif explicite: éviter les buffers géants en mémoire (streaming + chunking)
- [ ] Définir un “budget RAM” cible (ex: fonctionne correctement avec 2–4 GB)
- [ ] Limiter la taille d’un message (`message_len`) et d’une image (`img_len`)
- [ ] Limiter le batch effectif par heuristique mémoire (pas seulement `--max-batch-items`)
- [ ] Éviter de construire toute la réponse en RAM avant écriture (écriture progressive)
- [ ] En cas de pression mémoire: réduire batch/tile au lieu de crasher

## 11) Tests RAM / robustesse (orientés petites machines)
- [ ] Test “batch large” mais images petites: pas d’explosion RAM
- [ ] Test “images lourdes”: erreur propre (pas OOM, pas crash)
- [ ] Test longue session keep-alive (ex: 50–200 requêtes): pas de fuite mémoire visible
- [ ] Test avec `--tile-size` petit vs grand: vérifier le compromis RAM / perf
- [ ] Test de backpressure: stdin envoie plus vite que le GPU ne traite
- [ ] Mesurer au moins: pic RAM, temps moyen, taux d’erreur

## 12) Gestion des erreurs (contrat + résilience)
- [ ] Ne jamais écrire de logs/profiling sur stdout si stdout porte le protocole
- [ ] Toujours renvoyer une réponse d’erreur structurée si un `request_id` est connu
- [ ] Définir un `status_code` stable (ex: protocole, validation, GPU, OOM, interne)
- [ ] Inclure un message d’erreur court + actionnable côté client
- [ ] En cas d’erreur sur une requête: ne pas tuer le process keep-alive
- [ ] En cas de corruption du flux: tenter de se resynchroniser via `message_len`
- [ ] Valider strictement: magic, version, tailles, batch_count, limites RAM
- [ ] Timeout raisonnable par requête (éviter les hangs GPU)
- [ ] GPU fallback clair si possible (GPU → iGPU → CPU) sans changer le protocole
- [ ] Garantir la cohérence: pas de réponse partielle silencieuse

## 13) Tests d’erreurs (protocole + runtime)
- [ ] Magic/version invalides → erreur propre, requête suivante OK
- [ ] `message_len` invalide/trop grand → erreur propre, pas de crash
- [ ] `batch_count` incohérent vs payload → erreur propre
- [ ] Image invalide/corrompue → erreur explicite
- [ ] GPU indisponible/échec init → erreur explicite ou fallback maîtrisé
- [ ] Timeout traitement → erreur explicite et process toujours vivant
