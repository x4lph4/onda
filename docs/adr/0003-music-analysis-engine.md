# ADR-0003 — Motor de análisis musical (Fase 3)

Estado: aceptado (26 Sep 2026)

## Contexto

La Fase 3 exige comprensión musical de los tracks: BPM, beatgrid, key,
waveform, silencios, tempo map — todo fuera del hilo de audio, con cache
persistente, cola en background y overrides manuales.

## Decisiones

### 1. Pipeline sobre PCM decodificado (48 kHz fijo)

`MusicAnalysisEngine::analyzeTrack` decodifica el fichero completo a
48 kHz mono-summed (mismo decoder FFmpeg de F2/F3) y pasa los análisis
sobre ese buffer. Normalizar la sample rate fija las tablas de
hop/bins — una sola constante temporal para todos los algoritmos.

### 2. Onset detection: spectral flux

STFT 1024/256 (≈21 ms/5.3 ms), flux = Σ subidas de log-magnitud por bin,
picos con umbral adaptativo local (media×1.5 en ventana ±85 ms) y
separación mínima 100 ms. Simple, explicable, suficiente para rejillas.

### 3. BPM: autocorrelación + re-scoring octavizado

Rango 55–220 BPM. Cada lag puntúa además por sus armónicos (2×, 0.5×)
para favorecer el tempo real sobre submúltiplos. **Los candidatos se
conservan** (score normalizado) — la capa superior elige; nada se oculta.
`confidence` = margen del pico vs media + rivalidad con el 2º.

**Estabilidad**: ventanas adaptativas (n/4, clamp 4–16 s) con lag
dominante por ventana. Dispersión >2 % → `variable=true` + `tempoMap`
de segmentos groseros que la BeatGridEngine/UI puede refinar.
**Limitaciones documentadas**: swing puede sesgar a múltiplos; clips
muy cortos no son estimables; rubato devuelve baja confianza a propósito.

### 4. BeatGrid: piecewise-constant, matemática f64

`segments[]` de `{startFrame, startBeat, bpm}`; `frameToBeat`/`beatToFrame`
por búsqueda del último segmento aplicable — sin acumulación de drift
aun en tracks largos (testeado a 10 min). `downbeatOffset` marca la fase
de compás; estimada por la energía acumulada por posición de beat.

Edición manual (`BeatGridEngine`): setFirstBeat, move/nudge, adjustBpm
(escala proporcionalmente en multi-segmento), setTempoSegment re-deriva
frames posteriores, add/removeTempoChange, reset. Los cambios son
recomputados, no parchados — el grid siempre es autoconsistente.

### 5. Key: cromagrama + Krumhansl-Schmuckler

FFT 8192, magnitudes plegadas a 12 pitch-classes (55 Hz–5 kHz),
correlación de Pearson contra los 24 perfiles K-S. Salida: índice 0-23
(12 majors + 12 minors), nombre standard y **Camelot**. `confidence` =
margen vs segundo mejor. Intercambiable: solo `KeyAnalyzer::analyze`
expone el contrato.

### 6. Waveform: dos niveles cacheados

- `overview`: 4096 bins (min/max/RMS por canal) — siempre completo.
- `detail`: un bin por 256 frames (~5.3 ms) — para zoom.
`WaveformData::query(start,end,maxBins,ch)` agrega según nivel —
la UI jamás regenera PCM ni pide más bins de los visibles.
El formato `WaveformBin{min,max,rms}` es POD serializable tal cual
a BLOB (12 B × bins × canales ≈ 96 KB por track de 3 min).

### 7. SQLite propio (amalgamación, `third_party/sqlite`, gitignored)

Sin ORM: wrapper `Database` (prepared statements RAII) + migraciones
`schema_migrations` versionadas en transacciones. Esquema v1: `tracks`,
`analysis`, `beatgrids`, `waveforms`, `user_overrides` — separación
detected/manual que exige el requisito: **los overrides jamás pisan el
valor detectado** (`user_overrides.bpm/key_index` NULL = usar detectado).

### 8. Cache por fingerprint + versión

`fingerprint = FNV-1a(size + mtime + head 64KB + tail 64KB)`. Cambio de
fichero o de `kAnalysisVersion` → `stateFor` devuelve `Outdated` y
`invalidateStale` remarca en masa. Los resultados viven en DB + caché
en memoria (`AnalysisService`).

### 9. Prioridad: RT > UI > análisis

`AnalysisQueue` con N workers (≤4, hw/2) a `THREAD_PRIORITY_BELOW_NORMAL`,
heap por prioridad, cancelación cooperativa por token atómico
(chequeada entre etapas y en el bucle de decodificación), pausa/resume
global, retry automático acotado y re-encolado manual. Los eventos
(Started/Progress/Completed/Failed/Cancelled/Retried) se emiten en el
hilo worker — la UI los marshal-ea (o los encola ella misma).

### 10. De cara a Sync/Quantize (no implementado aún)

`BeatGrid` ya da: `frameToBeat`, `beatToFrame`, `nearestBeatFrame`,
`isDownbeat`, `barOf`, tempo map. Es la referencia temporal estable
que pedirán Sync/Quantize/BeatJump/loops musicales. `DeckStats::bpm/
beatPos/quantize` quedaron preparados en Fase 2 para recibirlos.

## Consecuencias

- `AnalysisService` es la única puerta: UI importa → queue → engine →
  store → cache. Testeable de extremo a extremo sin UI.
- La app abre `onda.db` (WAL) junto al config; LOAD de un deck dispara
  análisis en background y la cabecera muestra BPM/key al terminar.
- No hay UI de beatgrid/waveform aún (siguiente fase UI).
